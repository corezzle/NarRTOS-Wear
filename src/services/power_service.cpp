/**
 * @file power_service.cpp
 * @brief Batarya izleme ve uyku durum makinesi.
 *
 * State machine (test modu zamanlamaları):
 *   ACTIVE ──(8 sn)──► DIM ──(7 sn)──► DISPLAY_OFF ──(15 sn)──► DEEP_SLEEP
 *      ▲                  │                  │
 *      └─(buton/touch)────┘◄─────────────────┘ (sadece buton — ESP RTC'den uyanır)
 *
 * ACTIVE:      Ekran tam parlaklık.
 * DIM:         Ekran %15 parlaklığa düşer (co5300_set_brightness).
 * DISPLAY_OFF: Display SLPIN + parlaklık 0; FreeRTOS hâlâ koşar; touch ile
 *              uyandırılabilir (FT3168 INT GPIO 38 → MCU IRQ).
 * DEEP_SLEEP:  axp2101_prepare_deep_sleep() (display rayları OFF) →
 *              esp_deep_sleep_start(). Sadece BTN_TOP/BTN_BOTTOM (RTC IO)
 *              ile uyanılabilir; touch GPIO 38 RTC-capable değil.
 *
 * ─── Audit #5 (v1.2.2 review, 2026-05-19) — CLOSED-BY-DESIGN ──────────────────
 *
 * Bu dosyada 4 yerde (ACTIVE→DIM, DIM→OFF, wake from OFF/DIM, deep sleep entry)
 * şu pattern var:
 *
 *   ui_service_lock();          // LVGL durur, flush_cb yeni DMA QUEUE'LAYAMAZ
 *   vTaskDelay(pdMS_TO_TICKS(15));  // ~15 ms: kuyrukta bekleyen DMA drain olur
 *   co5300_set_brightness(...);     // SPI tx_param güvenle gönderilir
 *   ui_service_unlock();
 *
 * Audit raporunda "vTaskDelay LVGL kilidi tutarken — touch debounce window'unu
 * aşıyor, fix: vTaskDelay'ı lock öncesi yap" şeklinde flag edildi. Ama:
 *
 *   1. Önerilen düzeltme (lock öncesi delay) YANLIŞ: delay sırasında flush_cb
 *      aktif olduğu için yeni DMA push edilebilir → 15 ms sonra lock alınınca
 *      kuyrukta hâlâ pending transaction var → SPI cmd/data race.
 *
 *   2. Pratik etki ölçülmüş ihmal edilebilir:
 *      - DIM/OFF geçişleri: kullanıcı idle (saate bakmıyor) → 15 ms freeze
 *        görünmez. Touch IRQ donanım seviyesinde tetiklenir, LVGL queue'da
 *        beklenen 30 ms read period içinde işlenir → event kaybolmaz.
 *      - Wake (L.271): wake zaten 120 ms CO5300 SLPOUT datasheet delay'i
 *        alıyor → 15 ms o blokun yanında toz.
 *      - Deep sleep entry (L.441): 20 ms sonra esp_deep_sleep_start() →
 *        CPU dönmez, "geç freeze" diye bir şey yok.
 *
 *   3. 15 ms keyfi değil ölçülmüş: trans_queue_depth=10 + full-frame flush
 *      ~8 ms → 15 ms tek transaction drain için güvenli margin (8 ms + 7 ms).
 *      Aşağı çekmek (örn 8 ms) SPI race riski yaratır; tasarruf ~7 ms ×
 *      4 yer = görünmez kazanç, risk yüksek.
 *
 *   4. "Doğru" fix DMA-completion semaphore'u olur (on_color_trans_done
 *      ISR'da `xSemaphoreGiveFromISR` + power_service `xSemaphoreTake`).
 *      Ama: ISR-mutex etkileşim riski + reset race + 5-8 ms görünmez kazanç.
 *      Maliyet > fayda.
 *
 * Karar: pattern bilinçli tasarım. Dokunma. v1.2.2 paketinde audit #5 bulgusu
 * **closed-by-design** olarak kapatıldı. Gelecekte SPI driver'ın native
 * `spi_device_acquire_bus` ile drain primitif'i kullanılabilir hâle gelirse
 * tekrar değerlendirilir.
 */

#include "power_service.h"
#include "../drivers/power/axp2101_driver.h"
#include "../drivers/display/co5300_driver.h"
#include "../core/event_bus.h"
#include "../core/button_handler.h"
#include "ui_service.h"
#include "time_service.h"
#include "alarm_service.h"     // v1.0.1 fix (Bug #4): alarm-aware deep sleep

#include <esp_attr.h>          // RTC_DATA_ATTR (v1.0.1 fix)
#include <esp_sleep.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>          // Adım 3 (B): esp_wifi_stop before deep sleep
#include <nvs.h>               // Session 34: kullanıcı parlaklık seviyesi
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* Adım 5 (2026-05-14): USB disconnect during deep sleep.
 * USB peripheral'ı tamamen disable etmek + D+/D- pinlerini high-Z'ye almak
 * için kullanılan IDF API'leri. */
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_private/periph_ctrl.h>
#include <soc/periph_defs.h>

/* Adım 10 (2026-05-14): USB-Serial-JTAG PHY pad disable.
 * ESP32-S3'ün ESP-IDF console monitor'ü için kullandığı USB-Serial-JTAG
 * peripheral'ı, Adım 5'teki PERIPH_USB_MODULE'den FARKLI. Onu doğrudan
 * PHY seviyesinde kapatıyoruz. */
#include <hal/usb_serial_jtag_ll.h>

/* Adım 11 (2026-05-14): VCC3V3-direct chip kontrol pin'lerini disable state'e zorla.
 * Audio PA (GPIO 46), Motor (GPIO 18), SD CS (GPIO 17) — bunlar VCC3V3'ten
 * beslenen chip'leri kontrol eden ESP32 GPIO pin'leri. Default state float,
 * chip'in internal pull-up'ı pin'i çekebilir → chip yanlışlıkla enabled. */

static const char* TAG = "PowerSvc";

// ─── Zaman Eşikleri ───────────────────────────────────────────────────────────
#if defined(BOARD_VARIANT_LCD128) || defined(BOARD_VARIANT_NAR)
// NAR (Session 46h): ekran/dokunmatik bring-up bitene kadar uyku yok — dokunmatik
// cevap vermezken kart 33 sn'de uyuyor, USB kapanıyor, uzaktan test yapılamıyor.
// 1.28" LCD board (Faz 1-2 bring-up): dokunmatik driver henüz yok (CST816S,
// Faz 2) → kullanıcı aktivitesi algılanamıyor, kısa timeout'lar ekranı sürekli
// kapatıp cihazı deep sleep'e sokuyor. Geliştirme boyunca ÇOK uzun eşikler.
// CST816S driver'ı gelince bu blok kısaltılacak. AMOLED build'ı etkilenmez.
#define TIMEOUT_DIM_MS          (30 * 60 * 1000)   // 30 dk → DIM
#define TIMEOUT_DISPLAY_OFF_MS  (40 * 60 * 1000)   // 40 dk → DISPLAY_OFF
#define TIMEOUT_DEEP_SLEEP_MS   (60 * 60 * 1000)   // 60 dk → DEEP_SLEEP
#else
// TEST MODU — kısa süreler. Üretimde 30s / 5s / 10dk gibi değerler önerilir.
#define TIMEOUT_DIM_MS          (8  * 1000)        //  8 sn → DIM
#define TIMEOUT_DISPLAY_OFF_MS  (15 * 1000)        // 15 sn → DISPLAY_OFF (wake glow burada test edilir)
#define TIMEOUT_DEEP_SLEEP_MS   (30 * 1000)        // 30 sn → DEEP_SLEEP (DISPLAY_OFF'tan ~15 sn sonra ESP uyur)
#endif

// Deep sleep'ten RTC timer'la yeniden uyanma aralığı (saniye). Test için
// 60 sn — saat uyuyor → 60 sn'de uyanır → OTA check + (varsa) install +
// (yoksa) tekrar uyu. Üretimde ~3600 (saatte bir) makul.
#define POWER_DEEP_SLEEP_SYNC_INTERVAL_S  60

// ─── State ────────────────────────────────────────────────────────────────────
static power_state_t  s_state             = POWER_STATE_ACTIVE;
static uint32_t       s_last_activity_ms  = 0;

// Sync wake flag — main.cpp boot path'i (handle_wakeup_reason) TIMER cause
// tespit ettiğinde true'ya çevirir. sync_wake_task bunu okur, OTA check
// + tekrar sleep akışını çalıştırır.
static bool           s_sync_wake_active  = false;

/* v1.0.1 fix (Bug #4): Alarm wake flag — RTC slow memory'de yaşar
 * (RTC_DATA_ATTR) → deep sleep boyunca korunur. Deep sleep'e girerken bir
 * sonraki alarm sync interval'dan erken ise true'ya set edilir.
 * handle_wakeup_reason bunu görüp UI'yi normal spawn eder; alarm_service_init
 * consume eder. Normal .bss static'lerinin aksine bu, chip reset'inde
 * silinmez. */
RTC_DATA_ATTR static bool s_wake_for_alarm = false;

static QueueHandle_t  s_low_bat_queue     = NULL;
static QueueHandle_t  s_btn_top_queue     = NULL;
static QueueHandle_t  s_btn_bot_queue     = NULL;
static QueueHandle_t  s_wom_queue         = NULL;

static uint8_t        s_last_bat_pct      = 100;

// ─── Yardımcılar ─────────────────────────────────────────────────────────────

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t elapsed_ms(void)
{
    return now_ms() - s_last_activity_ms;
}

// ─── power_service_init ──────────────────────────────────────────────────────

/* ─── Session 34: kullanıcı parlaklık seviyesi — TEK KAYNAK ───────────────────
 *
 * Daha önce seviye settings_app içinde static duruyordu ve power_service her
 * uyanışta co5300_set_brightness(255) yazdığı için kullanıcının seçimi ilk
 * ekran kapanışında kayboluyordu. Artık burada tutulur, NVS'e yazılır ve
 * uyanış/DIM yolları bu değeri temel alır → Control Center ile Ayarlar aynı
 * değeri paylaşır.
 * Alt sınır 10: 0 ekranı tamamen karartır, kullanıcı cihazı "bozuldu" sanar. */
#define BRIGHT_MIN   10
#define BRIGHT_DIM   40     /* ~%15 — DIM durumunda uygulanan tavan */

static uint8_t s_user_brightness = 255;

uint8_t power_service_get_brightness(void) { return s_user_brightness; }

void power_service_set_brightness(uint8_t level)
{
    if (level < BRIGHT_MIN) level = BRIGHT_MIN;
    if (level == s_user_brightness) return;
    s_user_brightness = level;

    /* Ekran açıkken anında uygula; DIM/kapalıyken yalnız kaydet — uyanışta
     * zaten bu değerle açılacak (aksi halde karanlıkta ekran parlar). */
    if (s_state == POWER_STATE_ACTIVE) {
        ui_service_lock();
        co5300_set_brightness(s_user_brightness);
        ui_service_unlock();
    }

    nvs_handle_t h;
    if (nvs_open("pwr", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bright", s_user_brightness);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void brightness_load(void)
{
    nvs_handle_t h;
    if (nvs_open("pwr", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, "bright", &v) == ESP_OK && v >= BRIGHT_MIN) s_user_brightness = v;
    nvs_close(h);
}

void power_service_init(void)
{
    s_last_activity_ms = now_ms();
    s_state = POWER_STATE_ACTIVE;

    brightness_load();                       /* Session 34: kayıtlı seviye */
    co5300_set_brightness(s_user_brightness);

    // Event abonelikleri
    s_low_bat_queue = event_bus_subscribe(EVENT_LOW_BATTERY);
    s_btn_top_queue = event_bus_subscribe(EVENT_BTN_TOP);
    s_btn_bot_queue = event_bus_subscribe(EVENT_BTN_BOTTOM);
    s_wom_queue     = event_bus_subscribe(EVENT_GESTURE_WRIST_UP);

    ESP_LOGI(TAG, "Power service initialized");
}

// ─── power_service_update ────────────────────────────────────────────────────

void power_service_update(void)
{
    // 1. Batarya durumunu oku ve event yayımla
    axp2101_charge_thermal_guard();   /* yalniz lineer surum PMIC (0x47) + sarj + yuksek sicaklikta etkin */
    axp2101_bat_status_t bat;
    if (axp2101_get_bat_status(&bat) == ESP_OK) {
        if (bat.percent != s_last_bat_pct) {
            s_last_bat_pct = bat.percent;
            event_bus_publish(EVENT_BATTERY_LEVEL, bat.percent);
            event_bus_publish(EVENT_CHARGING_STATE, (uint32_t)bat.charge_state);
            ESP_LOGD(TAG, "Battery: %d%% %dmV %s",
                bat.percent, bat.voltage_mv,
                bat.charge_state == AXP_CHARGE_CHARGING ? "CHG" : "DCH");
        }

        // TEST MODU: Deep sleep devre dışı
        // if (bat.is_low && ...) { power_service_enter_deep_sleep(); }
    }

    // 2. Kullanıcı etkileşim event'lerini kontrol et (buton, WOM)
    //    NOT: Source ayrı tutulur ki sahte wake'i log'dan teşhis edebilelim
    //    (örn. USB-CDC trafiği GPIO 0 boot pinine kuplaj yapıp BTN_TOP fire
    //    ediyorsa wake-source log "BTN_TOP" olarak görünür).
    {
        os_event_t ev;
        if (event_bus_receive(s_btn_top_queue, &ev, 0)) {
            power_service_user_activity(POWER_WAKE_SRC_BTN_TOP);
        }
        if (event_bus_receive(s_btn_bot_queue, &ev, 0)) {
            power_service_user_activity(POWER_WAKE_SRC_BTN_BOTTOM);
        }
        if (event_bus_receive(s_wom_queue,     &ev, 0)) {
            power_service_user_activity(POWER_WAKE_SRC_WOM);
        }
    }

    // 3. IRQ üzerinden gelen low-bat event'ini kontrol et
    {
        os_event_t ev;
        if (event_bus_receive(s_low_bat_queue, &ev, 0)) {
            ESP_LOGD(TAG, "PMIC IRQ received, status refreshed");
        }
    }

    // 3b. [wake-trace] Buton ISR'ı yutulan glitch sayaçlarını her ~5 sn'de
    //     bir log'la. >0 değer demek ki donanım glitch reject sahte kenarları
    //     yakalıyor — eğer aynı anda ekran "açılıyorsa" reject yetersiz
    //     (ör. delay'i artırmak gerekir).
    static uint32_t s_glitch_log_last_ms = 0;
    if ((now_ms() - s_glitch_log_last_ms) >= 5000) {
        s_glitch_log_last_ms = now_ms();
        uint32_t gt = 0, gb = 0;
        button_handler_drain_glitch_counts(&gt, &gb);
        if (gt || gb) {
            ESP_LOGI(TAG, "[wake-trace] btn glitches rejected last 5s: TOP=%lu BOTTOM=%lu",
                     (unsigned long)gt, (unsigned long)gb);
        }
    }

    // 4. İnaktivite state machine
    uint32_t idle = elapsed_ms();

    switch (s_state) {
        case POWER_STATE_ACTIVE:
#if defined(BOARD_VARIANT_LCD128) || defined(BOARD_VARIANT_NAR)
            /* 1.28" LCD board bring-up: EKRAN HİÇ KARARMASIN. Dokunmatik
             * yok (Faz 2) VE kasada erişilebilir buton yok → ekran bir kez
             * karardı mı kullanıcı geri açamıyor. CST816S gelince bu guard
             * kaldırılıp normal DIM akışı test edilecek. */
            break;
#endif
            if (idle >= TIMEOUT_DIM_MS) {
                ESP_LOGI(TAG, "Inactivity: ACTIVE → DIM");
                // SPI bus yarışına karşı LVGL kilidi + DMA drain (bkz. user_activity).
                // v1.2.2 audit #5: closed-by-design (lock + 15ms drain pattern). Detay → file header.
                ui_service_lock();
                vTaskDelay(pdMS_TO_TICKS(15));
                /* Session 34: kullanıcı seviyesi zaten DIM tavanının altındaysa
                 * DIM'e geçerken ekranı PARLATMA — o değerde bırak. */
                co5300_set_brightness((s_user_brightness < BRIGHT_DIM)
                                          ? s_user_brightness : BRIGHT_DIM);
                ui_service_unlock();
                s_state = POWER_STATE_DIM;
            }
            break;

        case POWER_STATE_DIM:
            if (idle >= TIMEOUT_DISPLAY_OFF_MS) {
                ESP_LOGI(TAG, "Inactivity: DIM → DISPLAY_OFF");
                // NOT: Display raylarını (DCDC2/LDO2) kapatmıyoruz.
                // Bu raylar FT3168 touch IC'yi de besliyor; kapatılırsa
                // ekran karardığında dokunmatik tamamen ölüyor → ekrana
                // dokunarak uyandırma çalışmıyor.
                // Onun yerine sadece display panelini sleep'e al + parlaklık 0.
                // v1.2.2 audit #5: closed-by-design (lock + 15ms drain pattern). Detay → file header.
                ui_service_lock();
                vTaskDelay(pdMS_TO_TICKS(15));
                co5300_set_brightness(0);
                co5300_sleep(true);
                ui_service_unlock();
                s_state = POWER_STATE_DISPLAY_OFF;
            }
            break;

        case POWER_STATE_DISPLAY_OFF:
#if defined(BOARD_VARIANT_LCD128) || defined(BOARD_VARIANT_NAR)
            /* 1.28" LCD board: DEEP SLEEP DEVRE DIŞI (bring-up dönemi).
             * İki sebep: (1) dokunmatik yok (Faz 2) → kullanıcı cihazı
             * uyandıramıyor; dahili pil yüzünden USB çekilse bile saat
             * kapanmıyor, uykuya girip "ölü" görünüyor (2026-07-10 yaşandı).
             * (2) Deep sleep prep'i (rail kapatma, GPIO hold listesi)
             * AMOLED board'un pin haritasına göre yazılmış — bu board'da
             * o pinler başka işlevlerde. CST816S driver'ı gelince bu blok
             * yeniden değerlendirilecek. AMOLED build'ı etkilenmez. */
            (void)TIMEOUT_DEEP_SLEEP_MS;
#else
            if (idle >= TIMEOUT_DEEP_SLEEP_MS) {
                ESP_LOGI(TAG, "Inactivity: DISPLAY_OFF → DEEP_SLEEP (with %us timer wake)",
                         (unsigned)POWER_DEEP_SLEEP_SYNC_INTERVAL_S);
                /* Timer wake ile uyu — periodic sync için. Button da hâlâ
                 * aktif, kullanıcı isterse manuel uyandırabilir. */
                power_service_enter_deep_sleep_with_timer(POWER_DEEP_SLEEP_SYNC_INTERVAL_S);
            }
#endif
            break;

        case POWER_STATE_DEEP_SLEEP:
            // Bu noktaya normalde ulaşılmaz; esp_deep_sleep_start() dönmez.
            break;
    }
}

// ─── power_service_get_state ─────────────────────────────────────────────────

power_state_t power_service_get_state(void)
{
    return s_state;
}

// ─── power_service_user_activity ─────────────────────────────────────────────

static const char* wake_src_str(power_wake_source_t s)
{
    switch (s) {
        case POWER_WAKE_SRC_TOUCH:      return "TOUCH";
        case POWER_WAKE_SRC_TOUCH_OFF:  return "TOUCH(off→on)";
        case POWER_WAKE_SRC_TOUCH_DIM:  return "TOUCH(dim→on)";
        case POWER_WAKE_SRC_BTN_TOP:    return "BTN_TOP";
        case POWER_WAKE_SRC_BTN_BOTTOM: return "BTN_BOTTOM";
        case POWER_WAKE_SRC_WOM:        return "WOM";
        default:                        return "UNKNOWN";
    }
}

void power_service_user_activity(power_wake_source_t src)
{
    // [wake-trace] HER çağrıyı log'la — state ACTIVE olsa bile. Sahte
    // tetiklemeleri (USB-CDC kuplaj, EMI, vb.) teşhis için.
    ESP_LOGI(TAG, "[wake-trace] activity src=%s  state=%d  idle=%lu ms",
             wake_src_str(src), (int)s_state, (unsigned long)elapsed_ms());

    s_last_activity_ms = now_ms();

    bool waking_from_off = (s_state == POWER_STATE_DISPLAY_OFF);
    bool waking_from_dim = (s_state == POWER_STATE_DIM);

    if (waking_from_off || waking_from_dim) {
        // SPI bus yarışını önle:
        //   1. ui_service_lock → LVGL flush_cb yeni tx_color'ı kuyruğa ekleyemez.
        //   2. Kısa drain bekle → kuyruğa zaten girmiş DMA transaction'ları
        //      tamamlansın (her transaction'ın cmd fazı sırasında SPI driver
        //      bus'ı polling moduna kilitliyor; bu sırada tx_param yapamayız).
        //   trans_queue_depth=10 ve full-frame flush ~8ms olduğu için 15ms
        //   güvenli bir margine.
        // v1.2.2 audit #5: closed-by-design (ölçülmüş margin, audit raporundaki
        // "lock öncesi delay" önerisi yanlış — yeni DMA queue'lanır). Detay → file header.
        // Buradaki 15 ms wake'in toplam ~120 ms CO5300 SLPOUT süresinin yanında ihmal.
        ui_service_lock();
        vTaskDelay(pdMS_TO_TICKS(15));
        if (waking_from_off) {
            co5300_sleep(false);   // SLPOUT (kendi içinde 120ms delay)
        }
        co5300_set_brightness(s_user_brightness);   /* Session 34 */
        ui_service_unlock();
    }

    if (s_state != POWER_STATE_ACTIVE) {
        // Wake kaynağını açıkça etiketle. Sahte wake'leri (örn. USB-CDC
        // trafiğinin boot pinine kuplajı) teşhis için kritik.
        ESP_LOGI(TAG, "User activity → ACTIVE  (src=%s)", wake_src_str(src));
    }
    s_state = POWER_STATE_ACTIVE;

    // Apple Watch tarzı uyandırma çerçevesi — yalnızca ekran tamamen kapalıyken
    // tetiklenir (DIM'den dönüşte yapay görünüyor).
    if (waking_from_off) {
        ui_service_show_wake_glow();
    }
}

// ─── Adım 5 (2026-05-14): USB peripheral disconnect for deep sleep ──────────
//
// Hedef: USB host (Mac) saatin USB cihazı olarak "disconnected" gördüğünü
// algılaması → D+ pull-up pasifleşir → USB enumeration kalmaz → USB hattındaki
// akım PMIC + LED hardware baseline'a düşer (yazılım sınırı).
//
// Yapılanlar:
//   1. USB-Serial-JTAG controller clock'unu kes (PERIPH_USB_DEVICE_MODULE)
//      → ESP32-S3 download/console için kullandığımız peripheral. Disable
//      olunca D+ pull-up switch (internal) pasifleşir.
//   2. USB-OTG controller clock'unu kes (PERIPH_USB_MODULE) — defansif;
//      bu boardda USB-OTG aktif değil ama emin olalım.
//   3. GPIO 19 (D-) ve GPIO 20 (D+) — IO mux'u disable et + hold tut.
//      Deep sleep boyunca high-Z'de kalır, ESP32 driver'ları tekrar
//      çağrılana kadar (yani wake'te) sürmeye başlamaz.
//
// Wake davranışı: chip reset → bootloader → app_main → USB-CDC console
// init etmediği sürece USB peripheral kapalı kalır. CONFIG_ESP_CONSOLE_USB_CDC
// set ise app_main öncesi bootloader USB'yi tekrar aktive eder → boot
// log'larını USB üstünden görürüz. Sleep'e dönerken yine disable edilir.

static void usb_disconnect_for_sleep(void)
{
    /* 1) USB-OTG peripheral clock'u kes (Adım 5).
     *    Bu, USB-OTG controller'ı (PERIPH_USB_MODULE) kapatır.
     *    Ama ESP32-S3'te ayrı bir USB-Serial-JTAG var (aşağıda). */
    periph_module_disable(PERIPH_USB_MODULE);

    /* 2) Adım 10 (2026-05-14): USB-Serial-JTAG PHY pad disable.
     *    ESP-IDF console monitor'ü tarafından kullanılan AYRI peripheral.
     *    PERIPH_USB_MODULE bunu KAPSAMIYOR. Onun PHY pad'ini doğrudan
     *    kapatınca:
     *      - USB-Serial-JTAG D+ pull-up bırakılır
     *      - USB host "device disconnected" görür
     *      - PHY analog devresi güç kaybeder
     *    Tahmini: 5-15 mA tasarruf. */
    usb_serial_jtag_ll_phy_enable_pad(false);

    /* 3) D-/D+ pinlerini RTC IO seviyesinde isolate.
     *    rtc_gpio_isolate: IO mux + pull-up/down + input/output tamamen
     *    pasif. Deep sleep boyunca high-Z'de kalır. */
    rtc_gpio_isolate(GPIO_NUM_19);
    rtc_gpio_isolate(GPIO_NUM_20);

    ESP_LOGI(TAG, "USB disconnect: OTG off + Serial-JTAG PHY off + GPIO19/20 isolated");
}

/* Adım 7 (2026-05-14): Aggressive GPIO isolation + extra power domain shutdown
 * REVERTED — wake path'i kırdı:
 *   - rtc_gpio_isolate display pinlerini hold'da kilitledi → wake'te ekran açılmadı
 *   - VDDSDIO OFF Octal PSRAM domain'ini etkiledi → wake init unstable
 * v1.4.22 davranışına geri dönüldü. Yazılım sleep current optimizasyonu
 * tükendi; geri kalan ~20 mA hardware baseline (PWR LED + USB hattı). */

// ─── Adım 11 (2026-05-14): VCC3V3-direct chip control pin shutdown ──────────
//
// Audio PA chip, vibration motor driver, ve SD card adapter board üzerinde
// VCC3V3 (always-on rail) ile besleniyor. Bunları kontrol eden ESP32 GPIO
// pin'leri default state'te float — chip'in internal pull-up/down'a göre
// enabled veya disabled durumda olabilir. Audio sürücümüz olmadığı için
// hiç kontrol etmiyoruz → riskli durum.
//
// Sleep'e girerken bu pin'leri açıkça disable state'ine zorla + hold:
//   - GPIO 46 (PA_CTRL): LOW → PA shutdown
//   - GPIO 18 (MOTOR):   LOW → vibration off
//   - GPIO 17 (SDCS):    HIGH → SD card deselected
//
// Hold'lar wake'te main.cpp defansif unhold kodu tarafından temizlenir.

/* Adım 12 (2026-05-14): I²C bus drive LOW — REVERTED.
 *
 * Önceki fikir: I²C SDA/SCL'i output LOW + hold yaparak phantom power'ı
 * kesmek. Ama Codec schematic'inde R23 + R49 = 2.2 kΩ pull-up'lar VCC3V3'e
 * bağlı. ESP32 hatları LOW'a çekince → 3.3V / 2.2k × 2 hat = **3 mA boşa
 * harcama**. Phantom power kazancı belirsizdi (1-5 mA tahmin) → net zarar
 * veya break-even.
 *
 * Karar: Pull-up'ların pin'leri default HIGH çekmesine izin ver, drive
 * LOW yapma. Kod referans için tutuldu ama çağrılmıyor. */
#if 0
static void break_i2c_phantom_power_DISABLED(void)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << GPIO_NUM_14) | (1ULL << GPIO_NUM_15);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    gpio_set_level(GPIO_NUM_14, 0);
    gpio_set_level(GPIO_NUM_15, 0);
    rtc_gpio_hold_en(GPIO_NUM_14);
    rtc_gpio_hold_en(GPIO_NUM_15);
}
#endif

static void disable_vcc3v3_control_pins(void)
{
    /* PA_CTRL (GPIO 46) → LOW. PA chip shutdown. */
    gpio_config_t pa_cfg = {};
    pa_cfg.pin_bit_mask = (1ULL << GPIO_NUM_46);
    pa_cfg.mode = GPIO_MODE_OUTPUT;
    pa_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    pa_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pa_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&pa_cfg);
    gpio_set_level(GPIO_NUM_46, 0);
    gpio_hold_en(GPIO_NUM_46);

    /* MOTOR (GPIO 18) → LOW. Vibration motor off. */
    gpio_config_t motor_cfg = pa_cfg;
    motor_cfg.pin_bit_mask = (1ULL << GPIO_NUM_18);
    gpio_config(&motor_cfg);
    gpio_set_level(GPIO_NUM_18, 0);
    gpio_hold_en(GPIO_NUM_18);

    /* SDCS (GPIO 17) → HIGH. SD card deselected (CS active-low). */
    gpio_config_t sd_cfg = pa_cfg;
    sd_cfg.pin_bit_mask = (1ULL << GPIO_NUM_17);
    gpio_config(&sd_cfg);
    gpio_set_level(GPIO_NUM_17, 1);
    gpio_hold_en(GPIO_NUM_17);

    /* Deep sleep boyunca digital pad hold global enable. */
    gpio_deep_sleep_hold_en();

    ESP_LOGI(TAG, "Adım 11: PA_CTRL=LOW + MOTOR=LOW + SDCS=HIGH (holds enabled)");
}

// ─── power_service_enter_deep_sleep ──────────────────────────────────────────

void power_service_enter_deep_sleep(void)
{
#if defined(BOARD_VARIANT_NAR) && !defined(NAR_RELEASE)
    /* 2026-10-30 (Emin): test aşamasında NAR derin uykuya HİÇ girmesin;
     * release derlemesinde -DNAR_RELEASE=1 ile geri açılır. */
    ESP_LOGW(TAG, "NAR test derlemesi: derin uyku devre disi (NAR_RELEASE yok), uyanik kaliniyor");
    return;
#endif
    ESP_LOGI(TAG, "Entering deep sleep...");
    s_state = POWER_STATE_DEEP_SLEEP;

    /* 0) Mevcut sistem zamanını NVS'e yedekle. AXP2101 deep sleep
     *    konfigürasyonu ALDO1'i AÇIK bırakıyor → PCF85063 RTC chip uyku
     *    boyunca saymaya devam etmeli. AMA kullanıcı pili tüketirse veya
     *    ileride sleep mode politikası değişirse NVS son güvenli sığınak
     *    olur: wake'te time_service_init NVS'ten en son bilinen TRT
     *    zamanını kurtarır. NVS write ~10ms, deep sleep'i geciktirmez. */
    time_service_save_to_nvs();

    // 1) UI task'ı kilitle → LVGL flush_cb yeni SPI DMA göndermesin.
    //    Ardından ortada zaten kuyruğa girmiş DMA transaction'ları
    //    bitebilsin diye kısa drain bekle.
    // v1.2.2 audit #5: closed-by-design. Deep sleep entry'de zaten esp_deep_sleep_start()
    // çağrılacak (CPU dönmez) → 20 ms blocking görünür etki yok. Detay → file header.
    ui_service_lock();
    vTaskDelay(pdMS_TO_TICKS(20));

    // 2) Display panelini SLPIN'e al (zaten kapalıysa zarar vermez).
    co5300_sleep(true);

    // 3) WiFi explicit stop — RF kalıntısı kalmaması için (Adım 3-F).
    //    Driver başlatılmamış olabilir → ESP_ERR_WIFI_NOT_INIT görmezden gel.
    (void)esp_wifi_stop();

    // 4) PMIC ray kapatma + IMU/Touch suspend + ADC off + AXP2101 SLEEP mode.
    //    Adım 2 (2026-05-13): ALDO1 dahil tüm yardımcı raylar OFF.
    //    Adım 3 (2026-05-13): AXP2101 SLEEP mode + ADC OFF.
    //    Hedef pille çalışırken: ~0.3-0.5 mA.
    axp2101_prepare_deep_sleep();

    // 4b) Adım 11 (2026-05-14): VCC3V3-direct chip control pins disable.
    //     Audio PA + Motor + SDCS — default float state'te chip'ler enabled
    //     olabilir → ~5-15 mA. Açıkça disable state'e zorla.
    disable_vcc3v3_control_pins();

    // 4b2) Adım 14 (2026-05-14): GPIO10 (SYS_OUT / BSS138 gate) LOW + hold.
    //      KEYS bloğunda BSS138 MOSFET T1, SYS_OUT (GPIO10) ile sürülüyor.
    //      Default state'te external pull-up HIGH → BSS138 ON → drain
    //      tarafındaki yük (bilinmeyen) sürekli akım çeker.
    //      LOW'a sürerek BSS138'i kapatıyoruz. GPIO10 wake mask'ta zaten yok
    //      (donanım sorunu nedeniyle çıkarılmıştı), bu yüzden hold etmek
    //      wake'i etkilemez.
    {
        gpio_config_t sys_out_cfg = {};
        sys_out_cfg.pin_bit_mask = (1ULL << GPIO_NUM_10);
        sys_out_cfg.mode = GPIO_MODE_OUTPUT;
        sys_out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        sys_out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        sys_out_cfg.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&sys_out_cfg);
        gpio_set_level(GPIO_NUM_10, 0);
        rtc_gpio_hold_en(GPIO_NUM_10);
        ESP_LOGI(TAG, "Adım 14: GPIO10 (SYS_OUT) LOW + hold (BSS138 OFF)");
    }

    // 4c) Adım 12 — REVERT (2026-05-14):
    //     Adım 12 (I²C drive LOW + hold) phantom power'ı kesmeye çalışıyordu
    //     ama codec schematic R23 + R49 = 2.2 kΩ I²C pull-up'ları VCC3V3'e.
    //     Driving I²C LOW → 3.3V / 2.2k × 2 hat = 3 mA boşa harcama (kesin).
    //     Phantom power kazancı belirsizdi (1-5 mA tahmin) → net zarar veya
    //     break-even. Reverted, pull-up'lar pin'leri default HIGH çekecek.
    // break_i2c_phantom_power();   ← ÇAĞRILMIYOR ARTIK

    // 5) Adım 5 (2026-05-14): USB peripheral disconnect.
    //    USB host'a "device disconnected" sinyali verir → D+ pull-up pasifleşir
    //    → USB hattında protokol akımı kalmaz. AXP2101 charge devresi üstünden
    //    geçmeyen, doğrudan VBUS→ESP32 yolu üzerindeki ~10-15 mA'ı hedefliyor.
    usb_disconnect_for_sleep();

    // 6) ESP32-S3 RTC peripheral domain'ini söndür (kullanmıyoruz).
    //    RTC_FAST_MEM (timer state) ve RTC_SLOW_MEM AUTO'da kalır — bunlara
    //    dokunmuyoruz, sadece RTC_PERIPH'i kapatıyoruz. ~100-200 µA tasarruf.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);

    // 6b) Adım 14B (2026-05-14): VDDSDIO domain OFF — octal PSRAM power-down.
    //     ESP-IDF deep sleep'te VDDSDIO default'ta on kalıyor → octal PSRAM
    //     ACTIVE modda 10-30 mA çekiyor. PSRAM içeriği wake'te kayboluyor ama
    //     biz zaten boot'ta her şeyi yeniden init ediyoruz, sorun değil.
    //     Adım 7'de bu denenmişti ama GPIO holds wake'i kırıyordu — şimdi
    //     GPIO holds yok, sadece VDDSDIO OFF.
#ifdef BOARD_VARIANT_NAR
    /* 2026-10-30 (canlı teşhis, NAR-ESP32S3R16-32 dev kit): VDDSDIO OFF ile
     * derin uykudan uyanışta ROM flash'ı okuyamıyor → "invalid header:
     * 0xffffffff" döngüsü, yalnız güç kesilince düzeliyor (chip reset
     * düzeltmiyor). VDDSDIO bu SiP'te flash (GD25LQ256, 1,8 V) + 16 MB oktal
     * PSRAM'in ortak beslemesi; uyanışta flash hazır olmadan okunuyor ve
     * tanımsız duruma düşüyor. Zamanlayıcı uyanışıyla doğrulandı: ON iken
     * 0 invalid header. Bedeli: uykuda flash+PSRAM standby akımı. Saat
     * (AMOLED referans kartı) etkilenmiyor, orada 14B aynen kalıyor. */
    esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_ON);
    ESP_LOGI(TAG, "VDDSDIO uykuda ON (NAR SiP: flash uyanista okunamiyordu, 2026-10-30)");
#else
    esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_OFF);
    ESP_LOGI(TAG, "Adım 14B: VDDSDIO domain OFF (octal PSRAM power-down)");
#endif

    ESP_LOGI(TAG, "Going to sleep. Goodbye.");
    vTaskDelay(pdMS_TO_TICKS(20));  // log + I2C drain için kısa bekleme

    /* ═══════════════════════════════════════════════════════════════════════
     * SLEEP MODE SEÇİMİ (v1.4.35'te normal deep sleep'e geri alındı)
     *
     * İki seçenek var, biri aktif:
     *
     * SEÇENEK A — esp_deep_sleep_start() — AKTİF, varsayılan davranış
     *   - ESP32 RTC slow domain canlı
     *   - Üst BOOT buton wake çalışır
     *   - Alarm + sync wake (timer) çalışır
     *   - Akım ~0.02 A (KEWEISI), tahmini gerçek ~10-15 mA
     *   - Standart wearable davranışı
     *
     * SEÇENEK B — axp2101_soft_power_off() — KILL MODE, ihtiyaç halinde
     *   - AXP2101 tüm rail'leri keser (DCDC1 dahil), ESP32 ölür
     *   - Akım ~0.00 A (KEWEISI), gerçek ~5 mA
     *   - Buton wake ÇALIŞMAZ (R11 pull-up VCC3V3'te ölü, hardware kısıtlaması)
     *   - Sadece USB unplug-replug ile açılır
     *   - Wearable için uygun değil, sadece "demo" veya "uzun depolama" için
     *
     * Seçeneği değiştirmek için: aşağıdaki iki satırın yorum durumunu çevir.
     * Ayrıntı: aşağıdaki adım notları (Adım 17).
     * ═══════════════════════════════════════════════════════════════════════ */

    // ── SEÇENEK A: Normal deep sleep (AKTİF) ──
    esp_deep_sleep_start();   /* DÖNMEZ — ESP32 deep sleep, RTC canlı */

    // ── SEÇENEK B: Soft Power Off (KILL MODE, devre dışı) ──
    // axp2101_soft_power_off();   /* DÖNMEZ — AXP tüm rail'leri keser */

    // Bu satıra ulaşılmaz (yukarıdaki fonksiyon dönmez).
    ESP_LOGE(TAG, "FATAL: deep sleep returned — should never happen");
}

// ─── power_service_enter_deep_sleep_with_timer ───────────────────────────────
// RTC timer wake + button (EXT1) wake birlikte aktif.
// Saat N saniye uyur, sonra TIMER cause ile uyanır → main.cpp sync_wake
// flow'unu tetikler → OTA check + (varsa) install + (yoksa) tekrar uyu.

void power_service_enter_deep_sleep_with_timer(uint32_t seconds)
{
#if defined(BOARD_VARIANT_NAR) && !defined(NAR_RELEASE)
    /* 2026-10-30 (Emin): test aşamasında NAR derin uykuya HİÇ girmesin;
     * release derlemesinde -DNAR_RELEASE=1 ile geri açılır. */
    ESP_LOGW(TAG, "NAR test derlemesi: derin uyku devre disi (NAR_RELEASE yok), uyanik kaliniyor");
    (void)seconds;
    return;
#endif
    /* v1.0.1 fix (Bug #4): Alarm-aware wake.
     * Bir sonraki aktif alarm sync interval'dan erken ise alarm zamanına
     * kadar uyu (en az 2 sn — timer precision margin'i) ve s_wake_for_alarm
     * flag'ini RTC slow memory'de set et. main.cpp handle_wakeup_reason bu
     * flag'i görüp UI'yi normal spawn eder, sync_wake_task atlanır →
     * alarm_service tick_cb dakika sınırında alarmı fire eder, alarm
     * uygulaması ön plana çıkar. */
    s_wake_for_alarm = false;
    int64_t alarm_secs = alarm_service_seconds_to_next();
    uint32_t actual_seconds = seconds;
    if (alarm_secs > 0 && (uint64_t)alarm_secs < (uint64_t)seconds) {
        actual_seconds = (alarm_secs < 2) ? 2 : (uint32_t)alarm_secs;
        s_wake_for_alarm = true;
        ESP_LOGI(TAG, "Deep sleep: alarm in %llds, wake in %us (alarm wake)",
                 (long long)alarm_secs, (unsigned)actual_seconds);
    }

    ESP_LOGI(TAG, "Entering deep sleep WITH timer wakeup in %u seconds",
             (unsigned)actual_seconds);
    if (actual_seconds > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)actual_seconds * 1000000ULL);
    }
    // Button (EXT1) wake source'u zaten power_service_enter_deep_sleep
    // içinde axp2101_prepare_deep_sleep aşamasında konfigüre ediliyor.
    // Burada onu çağırarak aynı yolu yürürüz; timer config buraya eklendi.
    power_service_enter_deep_sleep();
    // Bu satıra ulaşılmaz.
}

// ─── v1.0.1 fix (Bug #4): Alarm wake flag accessors ──────────────────────────

bool power_service_is_alarm_wake(void)
{
    return s_wake_for_alarm;
}

bool power_service_consume_alarm_wake(void)
{
    bool was = s_wake_for_alarm;
    s_wake_for_alarm = false;
    return was;
}

bool power_service_is_sync_wake(void)
{
    return s_sync_wake_active;
}

// İç API — main.cpp'deki handle_wakeup_reason çağırır.
// Public API olmaması için header'da yok; extern "C" forward decl ile main
// dosyasından erişiyoruz.
extern "C" void power_service_mark_sync_wake(void)
{
    s_sync_wake_active = true;
}
