/**
 * @file button_handler.cpp
 * @brief GPIO buton ISR + debounce + kısa/uzun basma algılaması.
 *
 * Akış (yeni — hızlı tıklamaları kaçırmayan):
 *   1. Negedge ISR → press_start_us = now, is_pressed = true (ANINDA)
 *   2. Posedge ISR → duration = now - press_start_us
 *      - duration < MIN_PRESS_US (15ms) → gürültü sayılır, iptal
 *      - duration ≥ LONG_PRESS_US (500ms) → uzun basma
 *      - aksi halde → kısa basma
 *   3. Basma/bırakma sonrası COOLDOWN_US (50ms) boyunca tüm kenarlar yok sayılır
 *      (release kontakt bounce'ını reddeder).
 *
 * Eski tasarım 50ms'den kısa tıklamaları kaçırıyordu: negedge-sonrası
 * 50ms debounce biterken kullanıcı bıraktıysa is_pressed hiç set edilmiyordu.
 */

#include "button_handler.h"
#include "os_config.h"
#include "event_bus.h"
#include "../services/power_service.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>

static const char* TAG = "BtnHandler";

/* 2026-05-12: USB-CDC capacitive coupling 15ms'lik LOW pulse'lar üretebiliyor
 * → eski MIN_PRESS_US=15ms eşiği aşılıyor → sahte button event → ekran kendi
 * kendine açılıyor. Eşik 150ms'e yükseltildi: insan parmağı min 100-200ms tutar,
 * USB noise nadiren bu kadar uzun sürer. Glitch recheck 30µs → 300µs (USB noise
 * frequency'sini de yakalar). */
#define MIN_PRESS_US     150000   // 150ms — USB-CDC noise'u filtrele
#define COOLDOWN_US       50000   // 50ms — event sonrası bounce'a karşı sessiz süre
#define LONG_PRESS_US    500000   // 500ms uzun basma eşiği
#define GLITCH_RECHECK_US   300   // 300µs — kenar sonrası pin recheck (USB CDC noise için artırıldı)

// ─── Buton State ─────────────────────────────────────────────────────────────

typedef struct {
    gpio_num_t       pin;
    event_type_t     event_type;
    int64_t          press_start_us;   // negedge'de set edilir
    int64_t          last_event_us;    // son publish anı (cooldown için)
    bool             is_pressed;
    volatile uint32_t glitch_count;    // [wake-trace] ISR-içi recheck'te yutulan kenar sayısı
} btn_state_t;

static btn_state_t s_btn_top    = { .pin = (gpio_num_t)PIN_BTN_TOP,    .event_type = EVENT_BTN_TOP,    .press_start_us = 0, .last_event_us = 0, .is_pressed = false, .glitch_count = 0 };
static btn_state_t s_btn_bottom = { .pin = (gpio_num_t)PIN_BTN_BOTTOM, .event_type = EVENT_BTN_BOTTOM, .press_start_us = 0, .last_event_us = 0, .is_pressed = false, .glitch_count = 0 };

// ─── GPIO ISR ────────────────────────────────────────────────────────────────

static void IRAM_ATTR btn_isr(void* arg)
{
    btn_state_t* btn = (btn_state_t*)arg;
    int64_t now = esp_timer_get_time();

    // Event sonrası cooldown: release kontakt bounce'ını reddet
    if (now - btn->last_event_us < COOLDOWN_US) {
        return;
    }

    int level = gpio_get_level(btn->pin);

    // ── Donanım glitch reject ─────────────────────────────────────────────────
    // GPIO 0 (PIN_BTN_TOP) ESP32-S3 boot strap pini ve PCB'de USB-OTG
    // hatlarına yakın. USB-CDC monitor trafiği sırasında hızlı sinyallerden
    // kapasitif kuplaj alabiliyor → sahte negedge/posedge ISR'ları → ekran
    // kendi kendine uyanıyor. Çare: kenar görüldükten 30 µs sonra pini yeniden
    // oku; hâlâ aynı seviyede değilse glitch say ve event yayma. Gerçek bir
    // mekanik basışta level milisaniye'lerce stable kalır → recheck triviyal
    // şekilde geçer; kullanıcı hissetmez.
    esp_rom_delay_us(GLITCH_RECHECK_US);
    int level_after = gpio_get_level(btn->pin);
    if (level_after != level) {
        // Geçici darbe — yok say. Diagnostik için sayacı artır
        // (atomik: tek yazıcı ISR, tek okuyucu task = drain_glitch_counts).
        btn->glitch_count++;
        return;
    }

    if (level == 0) {
        // Negedge: basma başladı (zaten basılıysa bounce, yok say)
        if (!btn->is_pressed) {
            btn->is_pressed     = true;
            btn->press_start_us = now;
        }
    } else {
        // Posedge: bırakma
        if (!btn->is_pressed) return;

        int64_t duration = now - btn->press_start_us;

        if (duration < MIN_PRESS_US) {
            // Çok kısa → bounce / gürültü; press'i iptal et, event yayma
            btn->is_pressed = false;
            return;
        }

        btn->is_pressed    = false;
        btn->last_event_us = now;

        uint32_t press_type = (duration >= LONG_PRESS_US) ? 1 : 0;  // 0=kısa, 1=uzun

        // ISR context'ten event yayımla
        event_bus_publish_from_isr(btn->event_type, press_type);

        // Ekranı uyandır (ISR'dan güvenli: sadece semaphore/queue işlemi)
        // power_service_user_activity() doğrudan çağrılamaz (FreeRTOS API içeriyor)
        // Bu event power_service tarafından dinlenecek ve activity tetiklenecek.
    }
}

// ─── button_handler_init ─────────────────────────────────────────────────────

static esp_err_t init_button(btn_state_t* btn)
{
    // GPIO yapılandır
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << btn->pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,   // hem basma hem bırakma
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    // ISR handler ekle (gpio_install_isr_service daha önce çağrılmış olmalı)
    ESP_ERROR_CHECK(gpio_isr_handler_add(btn->pin, btn_isr, btn));

    ESP_LOGD(TAG, "Button GPIO%d initialized", btn->pin);
    return ESP_OK;
}

esp_err_t button_handler_init(void)
{
    ESP_LOGI(TAG, "Initializing buttons (GPIO%d, GPIO%d)", PIN_BTN_TOP, PIN_BTN_BOTTOM);

    // gpio_install_isr_service icm42670_init'te çağrılmıştı;
    // çift çağrıya karşı hata görmezden gelinir.
    gpio_install_isr_service(0);

    ESP_ERROR_CHECK(init_button(&s_btn_top));
    ESP_ERROR_CHECK(init_button(&s_btn_bottom));

    ESP_LOGI(TAG, "Buttons initialized");
    return ESP_OK;
}

void button_handler_drain_glitch_counts(uint32_t* out_top, uint32_t* out_bottom)
{
    // ISR sayacı 32-bit volatile; oku-sıfırla kombosu non-atomik (oku, sonra
    // yazana kadar yeni ISR fire ederse 1 sayı kaybedilir). Diagnostik amaç
    // için kabul edilebilir hata payı — ana fikir: glitch'lerin var olduğunu
    // göstermek, mutlak sayım vermek değil.
    if (out_top)    { *out_top    = s_btn_top.glitch_count;    s_btn_top.glitch_count    = 0; }
    if (out_bottom) { *out_bottom = s_btn_bottom.glitch_count; s_btn_bottom.glitch_count = 0; }
}

