/**
 * @file main.cpp
 * @brief Embedded Wearable OS - System Entry Point
 *
 * Dual-core FreeRTOS task architecture:
 *   Core 0 (PRO_CPU): UI Task  → LVGL rendering loop + touch input
 *   Core 1 (APP_CPU): System tasks → Sensors, Power, RTC, BLE
 *
 * Boot sequence:
 *   1. HAL init (I2C, SPI buses)
 *   2. PMIC init (AXP2101) → validate power rails
 *   3. Display init (CO5300 QSPI) + LVGL init
 *   4. Sensor / RTC init
 *   5. Spawn FreeRTOS tasks
 *   6. App Manager → launch default WatchFace app
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>

#include "core/os_config.h"
#include "core/os_tick.h"
#include "core/app_manager.h"
#include "core/event_bus.h"
#include "core/button_handler.h"

#include "hal/i2c_bus.h"

#include "drivers/display/co5300_driver.h"
#include "drivers/touch/ft3168_driver.h"
#include "drivers/imu/icm42670_driver.h"
#include "drivers/power/axp2101_driver.h"
#include "drivers/rtc/pcf85063_driver.h"

#include "services/ui_service.h"
#include "assets/corezzle_logo_a8_140.h"   /* Session 37: açılış logosu */
#include <lvgl.h>
#include "services/sensor_service.h"
#include "services/power_service.h"
#include "services/time_service.h"
#include "services/wifi_service.h"
#include "services/weather_service.h"
#include "services/alarm_service.h"
#include "services/audio_service.h"
#include "services/ota_service.h"
#include "services/telemetry_service.h"
#include "services/can_service.h"
#include "drivers/can/twai_driver.h"
#include "services/gps_service.h"
#include "services/activity_service.h"
#include "services/vibration_service.h"  /* v1.2.9b — LCD128 telemetri */
#include "services/command_service.h"    /* USB seri komut arayüzü (LCD128) */
#include "services/ble_service.h"          /* v1.2.8 — iPhone ANCS bridge */
#include "services/ancs_service.h"
#include "services/notification_store.h"
#include "services/notification_banner.h"  /* v1.4.70 — slide-down overlay */
#include "services/selftest_service.h"     /* açılış donanım checklist'i (yalnız seri) */

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_rom_sys.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <nvs_flash.h>

static const char* TAG = "MAIN";

// ─── Synchronization Primitives ──────────────────────────────────────────────

/** Signals that display + LVGL are ready. Sensor task waits on this. */
static EventGroupHandle_t s_boot_events;
#define BOOT_DISPLAY_READY  BIT0
#define BOOT_SENSORS_READY  BIT1
#define BOOT_ALL_READY      (BOOT_DISPLAY_READY | BOOT_SENSORS_READY)

// ─── Task Declarations ───────────────────────────────────────────────────────

static void task_ui(void* arg);
static void task_sensor(void* arg);
static void task_power(void* arg);
static void task_sync_wake(void* arg);  /* sadece timer wake'te */

// ─── Boot Helpers ────────────────────────────────────────────────────────────

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void log_system_info(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════");
    ESP_LOGI(TAG, " Embedded Wearable OS  v" OS_VERSION_STR);
    ESP_LOGI(TAG, " ESP32-S3R8 @ 240MHz");
    ESP_LOGI(TAG, " Free heap  : %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, " PSRAM size : %u bytes", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, " Free PSRAM : %u bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "═══════════════════════════════════════");
}

/**
 * @brief Deep sleep'ten uyanma sebebini logla ve gerekli aksiyonu al.
 *
 * Wakeup sebepleri:
 *   EXT1 (GPIO bitmask) → buton veya IMU WOM
 *   Diğer → normal boot (ilk açılış)
 */
/* Sahte deep-sleep wake teşhisi.
 *
 * Problem: Deep sleep'te FreeRTOS ölü → ISR-içi glitch reject çalışmaz.
 * USB-CDC trafiğinin EXT1 wake pinlerine (GPIO 0 veya GPIO 10) kapasitif
 * kuplajı tek bir µs'lik LOW pulse ile MCU'yu uyandırır → tam reset boot →
 * kullanıcı "ekran kendi kendine açıldı" görür.
 *
 * Çözüm: Boot'un ilk milisaniyelerinde uyandıran pini ~100 ms boyunca
 * sample'la. Gerçek mekanik basışta pin sürekli LOW kalır (insan parmağı
 * 50–200 ms basılı tutar); kapasitif glitch ise zaten geçmiştir → pin
 * hemen HIGH'a dönmüştür. %80'in altında LOW örnek varsa sahte sayılır
 * → EXT1'i tekrar arm edip esp_deep_sleep_start ile MCU hemen geri uyur.
 * Display rays AXP2101'de zaten kapalı kaldığı için ekran açılmaz.
 *
 * Bu yöntem hem GPIO 0 hem GPIO 10 için çalışır; pin değiştirmek (donanım
 * fix) gerektirmez. Gerçek bir buton basışı triviyal şekilde 80%+ LOW örnek
 * üretir → kullanıcı butonla deep sleep'ten uyandırma özelliğini kaybetmez.
 */
/* 2026-05-12: Verify thresholds çok daha katı yapıldı.
 * Sebep: USB-CDC capacitive coupling 200ms'i aşan sürelerle BTN_TOP pinini
 * LOW'da tutuyor → eski 200ms pencere fooled oluyordu, spurious wake "real
 * press" olarak verify edilip UI açılıyordu (kullanıcı raporu 1.4.0-1.4.4
 * "deep sleep'ten sonra ekran 60sn'de açılıyor"). USB enum genelde 300-500ms
 * sürer; 800ms pencere enum sonunu yakalar, pin HIGH'a kayar → fail eder.
 * Gerçek bir basışta kullanıcı parmağı 800ms boyunca pin LOW tutar → pass. */
#define WAKE_VERIFY_BUCKETS      8    // Pencereyi 8 dilime böl
#define WAKE_VERIFY_BUCKET_MS  100    // Her dilim 100 ms → toplam 800 ms
#define WAKE_VERIFY_SAMPLES_PB  50    // Dilim başına 50 örnek (her 2 ms)
#define WAKE_VERIFY_LOW_PCT     85    // İlk dilim %85+ LOW olmalı (çok katı)
#define WAKE_VERIFY_HOLD_PCT    75    // Sonraki dilim ort. ≥%75 LOW (USB enum HIGH'a kayışı yakala)

/* Sahte uyandırma şekilleri:
 *
 *   (a) Mikro saniyelik glitch (kapasitif kuplaj):
 *       ─┐    ┌──────────────  Wake olur, pin µs'ler içinde HIGH'a döner.
 *        └────┘                Tüm verify dilimlerinde HIGH örnekler hakim.
 *
 *   (b) USB-CDC enum / sürekli kuplaj (yeni gözlemlenen vaka):
 *       ──┐                    Wake olur, pin USB enumeration süresince
 *         └─────────────       LOW'da kalır. 100 ms verify pencereniz tamamen
 *       LOW örnek görür → "confirmed press" yanlış pozitifi. Ama gerçek bir
 *       basışta pin EVENTUALLY HIGH'a döner (insan parmağı 50–500 ms basar);
 *       USB enum biterken GPIO sürekli LOW kalmaz → 200 ms+ pencere ile
 *       gerçek basıştan ayırt edebiliriz: enum bitince pin HIGH'a kayar,
 *       LOW oranı düşer.
 *
 *   (c) Gerçek mekanik basış:
 *       ──┐                    Pin negedge → kullanıcı bırakana kadar LOW.
 *         └─────────────────   Tüm pencerede LOW (200+ ms hold tipik).
 *
 * Yeni algoritma: pencereyi 5 dilime böl (5×40 ms = 200 ms). Her dilim için
 * ayrı LOW oranı çıkar:
 *   - Dilim 0 (ilk 40 ms): zaten wake tetiklendiği için %80+ LOW olmalı,
 *     yoksa kesin sahte (case a).
 *   - Dilim 1..4 ortalaması: %60+ LOW olmalı, yoksa pin "sallanıyor" demek
 *     (USB enum kararsız) → sahte (case b).
 *   - İkisi de tutarsa gerçek basış (case c).
 */

static void verify_or_resleep(gpio_num_t pin, const char* label)
{
    /* KRİTİK: Deep sleep'ten dönüşte ESP-IDF tüm GPIO'ları sleep_gpio isolation
     * üzerinden izole eder (boot log: "sleep_gpio: Configure to isolate all
     * GPIO pins in sleep state"). EXT1 wake pinleri (GPIO 0, GPIO 10) RTC IO
     * domain'inde wake'i gerçekleştirir, ama wake sonrası pin hâlâ RTC IO
     * subsystem'inde kalır → `gpio_reset_pin` ve `gpio_set_pull_mode` dijital
     * GPIO subsystem'i üzerinden çalışsa da PULL-UP ETKİSİZ olur → leakage ile
     * pin LOW okunur (kullanıcı log'unda 200 ms boyunca 100/100 örnek LOW
     * görüldü, halbuki butona basılmamıştı).
     *
     * Sıra: rtc_gpio_deinit (RTC domain'den dijital domain'e döndür) →
     *       gpio_hold_dis (varsa hold mode'u kaldır) →
     *       gpio_reset_pin → gpio_set_direction + pull-up config →
     *       100 µs settle (10kΩ internal pull-up + parazit kapasitans için RC). */
    rtc_gpio_deinit(pin);
    gpio_hold_dis(pin);
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(100);  /* Pull-up settle */

    int low_per_bucket[WAKE_VERIFY_BUCKETS] = {0};
    const int interval_us = (WAKE_VERIFY_BUCKET_MS * 1000) / WAKE_VERIFY_SAMPLES_PB;  // 2000 µs

    for (int b = 0; b < WAKE_VERIFY_BUCKETS; b++) {
        for (int i = 0; i < WAKE_VERIFY_SAMPLES_PB; i++) {
            if (gpio_get_level(pin) == 0) low_per_bucket[b]++;
            esp_rom_delay_us(interval_us);
        }
    }

    int total_low = 0;
    for (int b = 0; b < WAKE_VERIFY_BUCKETS; b++) total_low += low_per_bucket[b];

    int total_samples = WAKE_VERIFY_BUCKETS * WAKE_VERIFY_SAMPLES_PB;

    /* Diagnostik — bucket başına LOW oranını yazdır */
    ESP_LOGI(TAG, "Wake-verify %s (GPIO%d): buckets [%d %d %d %d %d]/%d (total %d/%d)",
             label, (int)pin,
             low_per_bucket[0], low_per_bucket[1], low_per_bucket[2],
             low_per_bucket[3], low_per_bucket[4],
             WAKE_VERIFY_SAMPLES_PB, total_low, total_samples);

    /* Test 1: ilk dilimde yeterince LOW var mı? (kapasitif glitch testi) */
    int first_threshold = (WAKE_VERIFY_SAMPLES_PB * WAKE_VERIFY_LOW_PCT) / 100;
    bool first_ok = low_per_bucket[0] >= first_threshold;

    /* Test 2: kalan 4 dilimin ORTALAMA LOW oranı yeterince yüksek mi?
     * (USB enum / sürekli kuplaj testi — gerçek basışta pin LOW kalır,
     * USB enum geçince HIGH'a kayar) */
    int hold_low = 0;
    for (int b = 1; b < WAKE_VERIFY_BUCKETS; b++) hold_low += low_per_bucket[b];
    int hold_samples = (WAKE_VERIFY_BUCKETS - 1) * WAKE_VERIFY_SAMPLES_PB;
    int hold_threshold = (hold_samples * WAKE_VERIFY_HOLD_PCT) / 100;
    bool hold_ok = hold_low >= hold_threshold;

    if (!first_ok || !hold_ok) {
        const char* fail_reason =
            !first_ok ? "first-bucket LOW too few (capacitive glitch?)" :
                        "hold LOW too few (USB-CDC coupling?)";

        ESP_LOGW(TAG, "Spurious wake from %s (GPIO%d): %s — back to deep sleep",
                 label, (int)pin, fail_reason);

#if defined(BOARD_VARIANT_NAR) && !defined(NAR_RELEASE)
        /* 2026-11-05: NAR test derlemesi derin uykuya hiç girmez (power_service ile aynı kural);
         * eski firmware'dan kalan bir uyanışta bile geri uyumak yerine açılışa devam edilir. */
        ESP_LOGW(TAG, "NAR test derlemesi: geri uyunmuyor, acilisa devam");
        return;
#endif
        /* EXT1'i tekrar arm et — display rails AXP2101'de zaten kapalı.
         * BTN_BOTTOM (GPIO 10) mask'tan ÇIKARILDI: pin 200 ms boyunca %100
         * LOW okunuyor (donanım sorunu — bkz axp2101_driver.cpp:341+ yorum).
         * Sadece BTN_TOP ile arm et. */
        uint64_t mask = (1ULL << PIN_BTN_TOP);
        esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
        esp_deep_sleep_start();
        /* Bu satıra ulaşılmaz. */
    }

    ESP_LOGI(TAG, "Wakeup: %s (GPIO%d, %d/%d samples LOW — confirmed press)",
             label, (int)pin, total_low, total_samples);
}

/* Forward decl — power_service.cpp tarafından implement edilen iç API.
 * Header'a koymadık çünkü main.cpp dışından kimse çağırmamalı. */
extern "C" void power_service_mark_sync_wake(void);

static void handle_wakeup_reason(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    esp_reset_reason_t       reset = esp_reset_reason();

    ESP_LOGI(TAG, "Wakeup cause raw: %d, reset_reason: %d", (int)cause, (int)reset);

    /* Strateji (2026-05-12 v2): wake cause TEK BAŞINA güvenilmez (capacitive
     * coupling, USB-CDC noise vs nedeniyle yanlış sınıflandırılabilir).
     * Daha sağlam: `esp_reset_reason()` saatin nereden boot ettiğini söyler:
     *   ESP_RST_POWERON  → ilk açılış (UI'lı)
     *   ESP_RST_SW       → software reset (OTA sonrası esp_restart) (UI'lı)
     *   ESP_RST_DEEPSLEEP → deep sleep'ten uyandı → SİLENT (UI YOK!)
     *   diğer            → defansif olarak UI'lı (çökme sonrası vs)
     *
     * EXT1 verify hâlâ çalışıyor (sahte button wake'leri filtrele), ama UI
     * spawn kararı RESET REASON'a göre veriliyor — daha düşük seviyede ve
     * daha güvenilir. */

    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        /* Button verification — sahte wake ise zaten geri uyutur, buraya
         * dönmez. Real press dönerse devam ederiz. */
        uint64_t wakeup_pins = esp_sleep_get_ext1_wakeup_status();
        if (wakeup_pins & (1ULL << PIN_BTN_TOP)) {
            verify_or_resleep((gpio_num_t)PIN_BTN_TOP, "BTN_TOP");
        } else if (wakeup_pins & (1ULL << PIN_BTN_BOTTOM)) {
            verify_or_resleep((gpio_num_t)PIN_BTN_BOTTOM, "BTN_BOTTOM");
        } else if ((PIN_IMU_INT1 >= 0) && (wakeup_pins & (1ULL << PIN_IMU_INT1))) {
            /* Hangi APEX olayı olduğu icm42670_init'te (sıcak kurulum) status'tan okunur,
             * sensor_service_init bunu "uyandiran olay" olarak loglar/yayımlar. */
            ESP_LOGI(TAG, "Wakeup: IMU INT1 (GPIO%d) — APEX olayi", PIN_IMU_INT1);
        } else {
            ESP_LOGI(TAG, "Wakeup: EXT1 (pins: 0x%llX)", wakeup_pins);
        }
        /* Verified button press → UI spawn. sync_wake flag SET ETMIYORUZ. */
        ESP_LOGI(TAG, "Wakeup: confirmed user wake (EXT1) → UI ON");
        return;
    }

    /* İki ayrı sinyal kontrolü — daha güvenilir (belt and suspenders):
     *   1. esp_sleep_get_wakeup_cause() == TIMER: RTC timer tetikledi
     *   2. esp_reset_reason() == ESP_RST_DEEPSLEEP: chip deep sleep'ten geldi
     * Herhangi biri "evet" derse silent. Bazı edge case'lerde sadece biri
     * doğru olabilir; ikisini birlikte kontrol → false-negative riski sıfır. */
    bool is_silent_wake = (cause == ESP_SLEEP_WAKEUP_TIMER) ||
                          (reset == ESP_RST_DEEPSLEEP);
    if (is_silent_wake) {
        /* v1.0.1 fix (Bug #4): Alarm timer wake mı yoksa sync timer wake mi?
         * power_service deep sleep'e girerken alarm yakınsa s_wake_for_alarm
         * flag'ini RTC slow memory'e yazdı. Alarm wake ise UI'yi spawn et
         * (sync olarak işaretleme) → alarm_service_init flag'i consume eder
         * → tick_cb dakika sınırında alarmı fire eder. */
        if (power_service_is_alarm_wake()) {
            ESP_LOGI(TAG, "Wakeup: ALARM timer wake (cause=%d, reset=%d) — UI ON",
                     (int)cause, (int)reset);
            /* mark_sync_wake çağırma — UI normal spawn olsun */
        } else {
            ESP_LOGI(TAG, "Wakeup: SILENT sync wake (cause=%d, reset=%d) — UI OFF",
                     (int)cause, (int)reset);
            power_service_mark_sync_wake();
        }
    } else {
        /* Power-on, software reset (OTA sonrası), watchdog, panic vs →
         * normal boot, UI açık. */
        ESP_LOGI(TAG, "Boot: normal (cause=%d, reset=%d) → UI ON",
                 (int)cause, (int)reset);
    }
}

// ─── Task: UI (Core 0) ───────────────────────────────────────────────────────

/**
 * Owns: LVGL mutex, display DMA, touch polling
 * Stack: 8KB internal SRAM (LVGL internals must not be in PSRAM for speed)
 * Priority: UI_TASK_PRIORITY (5)
 */
static void task_ui(void* arg)
{
    ESP_LOGI(TAG, "[Core0] UI Task started");

    // 1. Init display hardware (DISPON yapılır ama brightness=0 → görünmez)
    ESP_ERROR_CHECK(co5300_init());

    // 2. Init LVGL + bind display/input drivers
    ui_service_init();

    // 2b. BOOT SPINNER — yeşil flash anti-pattern düzeltmesi.
    //
    // Önceki davranış: co5300_init brightness'i 255'e ramp edip bitiyordu →
    // panel internal RAM'de garbage (genelde yeşil bar) görünüyordu = wake
    // sırasında yeşil flash (kullanıcı raporu 2026-05-04, video kanıtı).
    //
    // Yeni davranış:
    //   1. co5300_init brightness=0 ile bitiyor (DISPON yapılır ama görünmez)
    //   2. LVGL'i siyah arka plan + ortada spinner ile fill et
    //   3. Force render → panel RAM'e siyah+spinner flush'lanır
    //   4. Brightness'i 255'e ramp et → panel RAM'deki garbage yerine bizim
    //      content görünür, hiç yeşil flash yok
    //   5. Spinner watchface açılınca otomatik kapanır (screen değişimi)
    {
        lv_obj_t* boot_screen = lv_screen_active();
        lv_obj_set_style_bg_color(boot_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(boot_screen, LV_OPA_COVER, 0);

        /* Session 37: iPhone tarzı açılış markası — siyah zeminde ortada
         * Corezzle logosu (kadranın üstünde kullanılan A8 asset'in aynısı,
         * beyaza boyanır), altında ince spinner. Logo ekran yüksekliğinin
         * ~%40'ına ölçeklenir; asset 140x175 olduğu için ölçek katsayısı
         * hedef yükseklikten türetilir (256 = 1x). */
#ifdef BOARD_VARIANT_LCD128
        /* Session 37c: 96 → 140 → 165 px. Yuvarlak ekranda pratik üst sınır:
         * asset oranı 140/175 = 0.8, yani çizilen kutu 132×165, merkez
         * (120,106) → en uzak köşe r≈117 (< 120). Spinner logonun 8 px
         * altına indi; daha büyük logo spinner'ı bezelin altına iter. */
        const int32_t logo_h    = 165;   /* 240 ekranda ~%69 */
        const int32_t logo_dy   = -14;
        const int32_t spin_d    = 34;
        const int32_t spin_dy   = 94;
#else
        const int32_t logo_h    = 330;   /* 502 ekranda ~%66 */
        const int32_t logo_dy   = -30;
        const int32_t spin_d    = 60;
        const int32_t spin_dy   = 170;
#endif
        lv_obj_t* logo = lv_image_create(boot_screen);
        lv_image_set_src(logo, &corezzle_logo_a8_140);
        lv_image_set_pivot(logo, corezzle_logo_a8_140.header.w / 2,
                                 corezzle_logo_a8_140.header.h / 2);
        lv_image_set_scale(logo, (uint32_t)(256 * logo_h / corezzle_logo_a8_140.header.h));
        /* A8 (yalnız alfa) asset → renk recolor ile verilir. */
        lv_obj_set_style_image_recolor(logo, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
        lv_obj_align(logo, LV_ALIGN_CENTER, 0, logo_dy);

        lv_obj_t* spinner = lv_spinner_create(boot_screen);
        lv_obj_set_size(spinner, spin_d, spin_d);
        lv_obj_align(spinner, LV_ALIGN_CENTER, 0, spin_dy);
        lv_obj_set_style_arc_color(spinner, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner, lv_color_white(),                LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);

        /* Force LVGL to render+flush this first frame so panel RAM has our
         * black+spinner instead of garbage. lv_refr_now triggers immediate
         * rendering of pending dirty regions. */
        ui_service_lock();
        lv_refr_now(NULL);
        ui_service_unlock();

        /* Şimdi panel RAM dolu, brightness'i 255'e ramp et — kullanıcı
         * yeşil garbage yerine spinner görür. */
        co5300_set_brightness(255);
    }

    // 3. Init touch controller
    // ft3168_init() boot sırasında I2C okuma başarısız dönebilir (cihaz hazır değil),
    // ama GPIO (INT pin) kurulumu yapılmış olur. touch_read_cb'deki INT pin kontrolü
    // zaten güvenli okuma sağladığından, init sonucundan bağımsız touch'ı register et.
    esp_err_t touch_ret = ft3168_init();
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "FT3168 I2C check failed at boot (err=0x%x) — registering anyway", touch_ret);
    }
    ui_service_register_touch();  // Her zaman register et

    // 4. Ana ekranı başlat (WearOS tarzı uygulama grid'i)
    app_manager_init();

    // 4b. Alarm scheduler — LVGL timer kullanır, dolayısıyla ui_service_init
    //     tamamlandıktan sonra çağrılmalı. Pending bir alarm tetiklendiğinde
    //     EVENT_APP_SWITCH(APP_ID_ALARM) yayınlar; app_manager bu event'i
    //     dispatch döngüsünde yakalayıp alarm uygulamasını ön plana çıkarır.
    alarm_service_init();

    // 4c. Audio servisi — alarm tetiklendiğinde ses çalmak için. LEDC PWM ile
    //     PIN_BUZZER üzerinde square-wave tone üretir. Init başarısız olsa
    //     bile sistem normal çalışır (audio_service_play_alarm no-op olur).
    //     LEDC modülü bağımsız çalışır; UI/sensor task'larıyla yarış yok.
    //     NAR: PIN_BUZZER = -1 (GPIO21 IMU INT1) → atlanır.
    if (PIN_BUZZER >= 0) audio_service_init();

    /* WatchFace launch — app_manager_launch yeni LVGL screen oluşturur ve
     * lv_screen_load_anim ile aktive eder. Eski boot screen (spinner)
     * otomatik olarak yerini watchface'e bırakır. */
    app_manager_launch(APP_ID_WATCHFACE);  // İlk ekran: saat kadranı

    /* v1.4.70 — Notification banner overlay. lv_layer_top() üzerinde slide-down
     * overlay; her app üstünde görünür. notification_store listener olarak
     * register olur, yeni bildirim geldiğinde lv_async_call ile UI thread'de
     * banner çizer. LVGL hazır olduktan SONRA init edilmeli. */
    notification_banner_init();

    // Signal other tasks
    xEventGroupSetBits(s_boot_events, BOOT_DISPLAY_READY);

    // Wait for all subsystems before running main loop
    xEventGroupWaitBits(s_boot_events, BOOT_ALL_READY, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));

    ESP_LOGI(TAG, "[Core0] Entering LVGL main loop");

    // ── Main LVGL loop ────────────────────────────────────────────────────
    // ui_service_tick() bir sonraki LVGL timer'ına kalan süreyi döner.
    // Adaptif vTaskDelay: min 3 ms (touch polling daha sık → daha responsive),
    // max 16 ms (60 FPS cap; idle'da CPU'yu kurtarır ama input lag'ı 16 ms'i
    // geçmez). Eski 5..30 aralığı en kötü ihtimalde 30 ms input gecikmesi
    // bırakıyordu — kullanıcı "bazen iki üç sefer tıklamam gerekiyor" raporu
    // direkt bunun belirtisiydi (tap finger-up esnasına denk gelirse missed).
    /* Session 13 — DÜZELTME A: LVGL saati artık GERÇEK saatten okunuyor.
     * Eski davranış her döngüde sabit lv_tick_inc(3) idi; döngü gerçekte
     * 3-16 ms bekleme + 30-60 ms çizim sürdüğünden LVGL'in iç saati gerçek
     * zamanın ~%32'si hızında akıyordu (ölçüm: lvspd=%29-44). Sonuç:
     * animasyonlar 3x uzuyor, kaydırma ivmesi geç sönüyor, uzun-basma eşiği
     * kayıyor — "her şey ağır" hissi. lv_tick_set_cb ile esp_timer'a bağlandı. */
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)(esp_timer_get_time() / 1000ULL); });

    /* AKICILIK ÖLÇÜMÜ (C6 Session 11 kalıbı):
     *  [stall] : tek bir lv_timer_handler çağrısı 100 ms'i aşarsa (hangi app)
     *  [beat]  : 5 sn'de bir boş heap, LVGL havuz doluluğu, aralıktaki en uzun
     *            kare (hmax) ve takılma sayacı.
     * Ölçüm esp_timer (gerçek saat) ile yapılır — LVGL tick'inden bağımsız. */
    int64_t  last_beat  = 0;
    uint32_t hmax_ms    = 0;   /* beat aralığındaki en uzun handler */
    uint32_t stall_cnt  = 0;   /* >100 ms kare sayısı (kümülatif) */
    uint32_t last_lvtick = 0;  /* LVGL saatinin gerçek saate göre hızı (lvspd) */
    for (;;) {
        int64_t  h0 = esp_timer_get_time();
        uint32_t time_to_next = ui_service_tick();   // lv_timer_handler() iç çağrı
        uint32_t h_ms = (uint32_t)((esp_timer_get_time() - h0) / 1000);
        if (h_ms > hmax_ms) hmax_ms = h_ms;
        if (h_ms > 100) {
            stall_cnt++;
            /* Büyük AMOLED'de tam kare çizimi tek başına ~100-110 ms sürebilir;
             * her karede uyarı basmak seri konsolu boğar (komut arayüzü
             * okunmaz hale gelir). En fazla 5 sn'de bir yaz — ayrıntı zaten
             * [beat] satırındaki stalls/hmax alanlarında. */
            static int64_t s_last_stall_log_us = 0;
            if (h0 - s_last_stall_log_us > 5000000) {
                s_last_stall_log_us = h0;
                ESP_LOGW(TAG, "[stall] lv_timer_handler %lu ms (app=%d) (toplam %lu)",
                         (unsigned long)h_ms, (int)app_manager_current_id(),
                         (unsigned long)stall_cnt);
            }
        }
        uint32_t delay_ms = time_to_next;
        if (delay_ms < 3)  delay_ms = 3;
        if (delay_ms > 16) delay_ms = 16;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));

        int64_t now = esp_timer_get_time();
        if (now - last_beat > 5000000) {
            last_beat = now;
            lv_mem_monitor_t m;
            lv_mem_monitor(&m);
            char ts[96];
            ui_service_debug_state(ts, sizeof(ts));
            /* lvspd: LVGL iç saati gerçek zamanın yüzde kaçı hızında akıyor.
             * %100 = doğru. Düşük değer → animasyonlar uzuyor, scroll ivmesi
             * geç sönüyor, long-press eşiği kayıyor ("her şey ağır" hissi). */
            uint32_t lvt   = lv_tick_get();
            int      lvspd = 0;
            static int64_t s_prev_beat_us = 0;
            if (s_prev_beat_us && lvt > last_lvtick) {
                lvspd = (int)((int64_t)(lvt - last_lvtick) * 100 /
                              ((now - s_prev_beat_us) / 1000));
            }
            s_prev_beat_us = now;
            last_lvtick    = lvt;
            ESP_LOGI(TAG, "[beat] heap=%lu lvgl=%%%d app=%d hmax=%lums stalls=%lu lvspd=%%%d %s",
                     (unsigned long)esp_get_free_heap_size(),
                     (int)m.used_pct, (int)app_manager_current_id(),
                     (unsigned long)hmax_ms, (unsigned long)stall_cnt, lvspd, ts);
            hmax_ms = 0;
        }
    }
}

// ─── Task: Sensor (Core 1) ───────────────────────────────────────────────────

/**
 * Owns: I2C bus, ICM-42670-P, PCF85063 polling
 * Stack: 4KB
 * Priority: SENSOR_TASK_PRIORITY (4)
 */
static void task_sensor(void* arg)
{
    ESP_LOGI(TAG, "[Core1] Sensor Task started");

    // Wait for display to be ready before touching shared buses
    xEventGroupWaitBits(s_boot_events, BOOT_DISPLAY_READY, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));

    esp_err_t imu_ret = icm42670_init();
    if (imu_ret != ESP_OK) {
        /* Session 46g: NAR kartında IMU açılışın 4. sn'sinde cevap vermedi,
         * 13. sn'de verdi (A/B testi). Bulunamazsa 30 sn boyunca saniyede bir
         * yeniden dene; ilk cevabın zamanını logla (teşhis + sağlamlık). */
        int64_t t0 = esp_timer_get_time();
        for (int i = 1; i <= 30 && imu_ret != ESP_OK; i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_log_level_set("ICM42670", ESP_LOG_NONE);
            imu_ret = icm42670_init();
            esp_log_level_set("ICM42670", ESP_LOG_INFO);
            if (imu_ret == ESP_OK)
                ESP_LOGW(TAG, "[imu-retry] IMU %d. denemede cevap verdi (+%lld ms, acilistan %lld ms)",
                         i, (long long)((esp_timer_get_time() - t0) / 1000),
                         (long long)(esp_timer_get_time() / 1000));
        }
        if (imu_ret != ESP_OK)
            ESP_LOGW(TAG, "[imu-retry] 30 sn boyunca cevap yok — step counter disabled");
    }

    sensor_service_init();

    xEventGroupSetBits(s_boot_events, BOOT_SENSORS_READY);

    ESP_LOGI(TAG, "[Core1] Sensor loop running");

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        sensor_service_update();   // Read IMU, update step count, push events
        time_service_update();     // Sync RTC → system time if drift > threshold

        // 50ms sensor tick = 20Hz. Step counter algorithm needs ≥20Hz.
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(50));
    }
}

// ─── Task: Power (Core 1) ────────────────────────────────────────────────────

/**
 * Owns: AXP2101 I2C reads, deep-sleep arbitration
 * Stack: 3KB
 * Priority: POWER_TASK_PRIORITY (6) — highest, must pre-empt sensor on low-bat
 */
static void task_power(void* arg)
{
    ESP_LOGI(TAG, "[Core1] Power Task started");

    // AXP2101 app_main'de zaten başlatıldı (display güç sırası için).
    // Burada sadece power_service başlatılıyor.
    ESP_LOGI(TAG, "[Core1] Power service init (AXP2101 already initialized)");
    power_service_init();

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        power_service_update();   // Battery %, charge state, thermal → event_bus

        // 1s power check interval. PMIC IRQ line also triggers this via semaphore
        // in a real implementation (interrupt-driven approach preferred).
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
    }
}

// ─── task_sync_wake — RTC timer wake'inden sonra çalışır ─────────────────────
// Akış:
//   1. WiFi'nin bağlanmasını bekle (15 sn timeout)
//   2. ota_service_check_async() çağır — manifest sorgusu yap
//   3. Manifest forced=true ise: check_task içinde install_async otomatik
//      tetiklenir, install_task bitince esp_restart eder → buradan dönmez
//   4. Forced yoksa (UP_TO_DATE veya ERROR): tekrar deep sleep'e gir,
//      60 sn sonra RTC timer ile tekrar uyandır (sync döngüsü)
//
// Bu task SADECE handle_wakeup_reason TIMER cause tespit ettiğinde spawn
// edilir; normal boot'ta yok. Diğer task'lar (UI, sensor, power) yine
// çalışıyor — saat görsel olarak normal görünür, sadece sync amaçlı task
// arka planda iş yapıp uyutur.
#define SYNC_WAKE_WIFI_TIMEOUT_S    15
#define SYNC_WAKE_CHECK_TIMEOUT_S   60   /* sadece check için — install başlarsa ayrı bekleme */
#define SYNC_WAKE_INSTALL_TIMEOUT_S 600  /* install başlarsa max 10 dk bekle (download + flash) */
#define SYNC_WAKE_NEXT_INTERVAL_S   60   /* test için 60 sn, üretimde ~3600 */

static void task_sync_wake(void* arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[sync_wake] task started — waiting for WiFi");

    // 1) WiFi bağlanana kadar bekle (timeout).
    for (int i = 0; i < SYNC_WAKE_WIFI_TIMEOUT_S * 2; i++) {
        if (wifi_service_is_connected()) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!wifi_service_is_connected()) {
        ESP_LOGW(TAG, "[sync_wake] WiFi yok, geri uyu");
        power_service_enter_deep_sleep_with_timer(SYNC_WAKE_NEXT_INTERVAL_S);
        // Bu satıra ulaşılmaz.
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "[sync_wake] WiFi bagli, OTA check tetikleniyor");

    // 2) OTA check. check_task forced bulursa install_async'i kendi
    //    tetikler; biz sadece state'i poll edip beklemeliyiz.
    ota_service_check_async();

    // 3) State machine'i takip et. İki aşama var:
    //    a) Check aşaması (max 60 sn): manifest sorgusu + state belirlenmesi.
    //       UP_TO_DATE / ERROR → install gerek yok, deep sleep'e geri dön.
    //       DOWNLOADING → install başladı, b) aşamasına geç.
    //    b) Install aşaması (max 10 dk): download + flash bitene kadar bekle.
    //       Bu süre ZORUNLU UZUN çünkü install ortalama 30-60 sn, yavaş
    //       WiFi'da 2-3 dk olabilir. Aksi halde sync_wake_task timeout'a
    //       düşüp install'ı yarıda bırakırdı → saat tekrar uyur → manifest
    //       check → forced=true → tekrar install başlat... → sonsuz döngü
    //       (kullanıcı raporu 2026-05-12: "5 dakika oldu hâlâ install bitmedi").
    bool install_in_progress = false;
    int timeout_iterations = SYNC_WAKE_CHECK_TIMEOUT_S * 2;  // başlangıçta 60 sn
    for (int i = 0; i < timeout_iterations; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        ota_status_t st;
        ota_state_t state = ota_service_get_status(&st);
        if (state == OTA_STATE_DOWNLOADING) {
            if (!install_in_progress) {
                install_in_progress = true;
                // Install başladı → timeout'u 10 dakikaya uzat.
                timeout_iterations = SYNC_WAKE_INSTALL_TIMEOUT_S * 2;
                i = 0;  // counter'ı sıfırla, yeni timeout'tan saymaya başla
                ESP_LOGI(TAG, "[sync_wake] Install basladi, timeout uzatildi (%us)",
                         (unsigned)SYNC_WAKE_INSTALL_TIMEOUT_S);
            }
        } else if (state == OTA_STATE_INSTALLED) {
            ESP_LOGI(TAG, "[sync_wake] Install bitti, install_task reboot edecek");
            // install_task auto-reboot yapacak (forced flag set olduğundan).
            // Defansif olarak biz de bekle, gerekirse manuel restart.
            vTaskDelay(pdMS_TO_TICKS(7000));
            esp_restart();
        } else if (state == OTA_STATE_ERROR) {
            ESP_LOGW(TAG, "[sync_wake] OTA ERROR — geri uyu, sonraki cycle'da yeniden dene");
            break;
        } else if (state == OTA_STATE_UP_TO_DATE) {
            if (install_in_progress) continue;  // pre-install state
            ESP_LOGI(TAG, "[sync_wake] Check tamam, install gerekmiyor — tekrar uyu");
            break;
        }
        // CHECKING / IDLE / UPDATE_AVAILABLE → bekle
    }

    // 4) Sync döngüsünü sürdür: tekrar deep sleep + timer wake.
    ESP_LOGI(TAG, "[sync_wake] Bir sonraki sync %u sn sonra. Uykuya gec.",
             (unsigned)SYNC_WAKE_NEXT_INTERVAL_S);
    power_service_enter_deep_sleep_with_timer(SYNC_WAKE_NEXT_INTERVAL_S);
    // Bu satıra ulaşılmaz.
    vTaskDelete(NULL);
}

// ─── app_main ────────────────────────────────────────────────────────────────

extern "C" void app_main(void)
{
    /* ═══════════════════════════════════════════════════════════════════════
     * Wake verification öncesi rail restore zorunluluğu
     * ───────────────────────────────────────────────────────────────────────
     * Deep sleep prep'inde AXP2101 Rail 1 ve Rail 3 OFF yapılıyor (display
     * rays tasarrufu için). PCB üzerinde BTN_BOTTOM (GPIO 10) external
     * pull-up rezistörü bu rail'lerden birinden besleniyor — wake sonrası
     * AXP2101 init henüz çalışmadığı için rail Vcc node'u 0 V'da → external
     * pull-up artık pin'i HIGH'a değil GND'ye çekiyor → 100/100 örnek LOW
     * okundu (kullanıcı log'unda doğrulandı, RTC IO deinit + internal
     * pull-up bunu yenemedi çünkü external 10kΩ daha güçlü).
     *
     * Çözüm: I2C bus + AXP2101 init'i `handle_wakeup_reason()` ÖNCESİNE al.
     * Rails ON olunca external pull-up doğru çalışır → pin gerçek state'e
     * döner → verify doğru karar verir (gerçek basış: LOW kalır; sahte:
     * HIGH'a kayar).
     * ═══════════════════════════════════════════════════════════════════════ */

    /* DEFANSİF: Önceki firmware (v1.4.23 deneme) Adım 7'de display + I²C + SD
     * gibi pinleri rtc_gpio_isolate ile hold'a almıştı. Hold state RTC IO
     * subsystem'de USB takılı olduğu sürece persist eder → flash sonrası yeni
     * firmware bu pinleri reconfigure etse bile hold takip eder → display init
     * fail. Boot başlangıcında potansiyel tüm hold'ları açıkça temizliyoruz.
     * Hold zaten yoksa no-op. */
    {
        const gpio_num_t hold_pins[] = {
            GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_3,
            GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
            GPIO_NUM_8, GPIO_NUM_9,
            GPIO_NUM_10,                     /* Adım 14: SYS_OUT (BSS138) */
            GPIO_NUM_11, GPIO_NUM_12,
            GPIO_NUM_14, GPIO_NUM_15,
            GPIO_NUM_17, GPIO_NUM_18,        /* Adım 11: SDCS + MOTOR */
            GPIO_NUM_19, GPIO_NUM_20,
            GPIO_NUM_21,
        };
        const size_t n = sizeof(hold_pins) / sizeof(hold_pins[0]);
        for (size_t i = 0; i < n; i++) {
            rtc_gpio_hold_dis(hold_pins[i]);
        }
        /* Adım 11: GPIO 46 (PA_CTRL) non-RTC pin, digital hold ile tutuluyor.
         * rtc_gpio_hold_dis çalışmaz, gpio_hold_dis kullan. */
        gpio_hold_dis(GPIO_NUM_46);
        gpio_deep_sleep_hold_dis();   /* digital domain hold global off */
    }

    // 1. I2C bus — AXP2101 için ön koşul (RTC I2C bus interrupts henüz çalışmaz
    //    çünkü gpio_install_isr_service ileride çağrılır, ama I2C polling-mode
    //    bu aşamada çalışır).
    ESP_ERROR_CHECK(i2c_bus_init());

    // 2. AXP2101 — Rails ON. ALDO2 (display) + diğer rails default'a döner.
    //    BTN_BOTTOM external pull-up'ı için Vcc geri gelir.
    {
        esp_err_t axp_ret = axp2101_init();
        if (axp_ret != ESP_OK) {
            ESP_LOGW(TAG, "AXP2101 pre-verify init failed (0x%x) — continuing", axp_ret);
        } else {
            ESP_LOGI(TAG, "AXP2101 pre-verify init OK — rails ON (BTN pull-ups restored)");
        }
    }
    // Rails stabilization: external pull-up RC settle (10kΩ + ~100pF ≈ 1µs ama
    // AXP2101 LDO turn-on time'ı ~5ms tipik; defansif 50ms.)
    vTaskDelay(pdMS_TO_TICKS(50));


    // 3. Wake verification — rails ON, pin'ler gerçek state'inde.
    handle_wakeup_reason();

    // 3b. Sync wake (silent uyanma) ise tüm gereksiz rayları kapat.
    //     axp2101_init normal kullanım için ALDO1'i (IMU/Touch/RTC) açtı,
    //     ALDO2 (display) zaten yok. Sync wake'te bunların hiçbirine ihtiyaç
    //     yok — sadece WiFi + OTA check için DCDC1 yeterli. Tüm yardımcı
    //     rayları kapatarak ~5-8 mA ek tasarruf (Adım 2 power optimization).
    if (power_service_is_sync_wake()) {
        axp2101_set_rail(AXP_RAIL_LDO1, false);   // IMU + Touch + PCF85063 off
        axp2101_set_rail(AXP_RAIL_LDO2, false);   // display AVDD off
        axp2101_set_rail(AXP_RAIL_DCDC2, false);  // display IO off
        ESP_LOGI(TAG, "Sync wake — all non-essential rails OFF (silent + low power)");
    }

    // 4. Monitor'ün bağlanması için kısa bekleme (debug; verify zaten geçti).
    vTaskDelay(pdMS_TO_TICKS(2000));

    // ── Temel sistem başlatma (sıra önemli) ──────────────────────────────────

    ESP_ERROR_CHECK(init_nvs());

    log_system_info();

    // (i2c_bus_init + axp2101_init yukarıda yapıldı; tekrarlamıyoruz)

    // 5. Event bus — task'lar arası iletişim altyapısı
    event_bus_init();

    // 6. Buton handler (gpio_install_isr_service burada yapılır;
    //    ICM42670 init'i de ISR ekleyecek)
    ESP_ERROR_CHECK(button_handler_init());

    // 6. RTC + zaman servisi — UI task app launch'tan önce ayarla
    {
        /* Session 46i: SiP iç şemasında PCF85063 SiP'in İÇİNDE, ESP32_SDA/SCL
         * hattında ve 32.768 kHz kristaliyle var (besleme AXP2101 RTCLDO →
         * VCC-RTC, kesme RTC_INT → GPIO39). "NAR'da RTC yok" varsayımı
         * kaldırıldı; cevap vermezse zaten uyarıyla iç saate düşüyor. */
        esp_err_t rtc_ret = pcf85063_init();
        if (rtc_ret != ESP_OK) {
            ESP_LOGW(TAG, "PCF85063 RTC not found (err=0x%x) — using internal clock", rtc_ret);
        }
        time_service_init();
    }

    // 7. WiFi + NTP senkronu — kullanıcı tercihine saygı göster.
    //    Kullanıcı Settings'ten WiFi'yi kapattıysa NVS'te pref=OFF kalmıştır;
    //    boot'ta auto-connect'i atla. Pref=ON ise (varsayılan) bağlanmayı dene.
    //    SNTP daemon yine her durumda kalkar — kullanıcı sonradan WiFi'yi
    //    elle açarsa NTP otomatik sync olur.
    {
        wifi_service_init();
        bool wifi_pref = wifi_service_get_user_preference();
        if (wifi_pref) {
            esp_err_t wifi_ret = wifi_service_connect(8000);
            if (wifi_ret == ESP_OK) {
                time_service_sync_from_ntp(8000);
            } else {
                ESP_LOGW(TAG, "WiFi not connected (err=0x%x) — NTP skipped initially", wifi_ret);
            }
        } else {
            ESP_LOGI(TAG, "WiFi user preference = OFF, skipping auto-connect at boot");
        }
        /* Always arm the SNTP background daemon, regardless of whether
         * initial WiFi came up. As soon as the network becomes available
         * (user enables WiFi later in Settings, or reconnect after
         * roaming), the daemon's poll cycle fires sntp_sync_cb which
         * updates system time AND persists to RTC. */
        time_service_ntp_autosync_start();
    }

    // 8. Hava durumu servisi — Wi-Fi hazırsa arka planda çekmeye başlar.
    //    Core 1'de ayrı task; boot anında başlatılabilir (Wi-Fi'yi bekler).
    weather_service_init();

    // 9. OTA servisi — Software Updates ekranı bunu kullanır. init() sadece
    //    state hazırlığı yapar (mutex + boot validation), aktif iş yok;
    //    kullanıcı Settings → General → Software Updates'e tıklayana kadar
    //    sessiz. esp_ota_mark_app_valid_cancel_rollback boot sonrası tetiklenir
    //    (rollback enabled olunca anti-crash-loop koruması).
    ota_service_init();
#ifdef TRACKER_FEATURES
    /* Takip (tasma) servisleri — LCD128 ve NAR derlemelerinde (platformio.ini
     * -DTRACKER_FEATURES=1). AMOLED build'ı değişmez. */
    /* GPS/GNSS (UART1, NMEA) — fix varsa telemetriye "gps" alanı eklenir,
     * yoksa backend WiFi konumuna düşer. Modül yoksa sessizce boş bekler. */
    gps_service_init();
    /* Piezo titreşim sensörü (PIEZO_PIN tanımlıysa) — aktiviteye darbe/dk katar */
    vibration_service_init();
    /* Ham CAN (araç veri yolu): kimlik başına son frame tablosu; cihaz çözmez, telemetriye
     * "can" alanı olarak gider. TWAI yalnız ENABLE_CAN=1 + transceiver + PIN_CAN_TX/RX ile;
     * aksi hâlde tablo `can inject` komutuyla beslenir (donanımsız test). NAR'da 4/5 ekran
     * hatları: can_twai_start hemen döner, kurulum ekran init'i bitince ayrı görevde yapılır. */
    can_service_init();
    {
        esp_err_t ce = can_twai_start(can_service_bitrate());
        if (ce == ESP_ERR_NOT_SUPPORTED) ESP_LOGI(TAG, "twai: derleme disi (ENABLE_CAN=0) - `can inject` ile besleyin");
        else if (ce != ESP_OK)           ESP_LOGE(TAG, "twai baslatilamadi: %s", esp_err_to_name(ce));
    }
    /* Hareket durumu (IMU adım tempoundan + piezo) — tracker aktif/dinlenme modu buna bakar */
    activity_service_init();
    telemetry_service_init();
    /* USB seri komut arayüzü — ekranları/uygulamaları dışarıdan sürme
     * (test/demo/otomasyon). Seri terminale 'help' yazın. */
    command_service_init();
#endif
    /* Arkaplan otomatik check timer'ı — auto_update setting açıkken
     * her 30 dakikada bir manifest sorar (boot+5dk ilk fire). Pencerede
     * (gece 00:00-06:00) UPDATE_AVAILABLE bulursa install_async + auto-
     * reboot tetiklenir. Setting kapalıysa timer çalışsa da hiçbir şey
     * yapmaz (sadece NVS okur ve geçer). */
    ota_service_start_auto_check();

    /* 10. BLE + ANCS — iPhone bildirim mirror'ı.
     *
     * Sync wake (silent boot) durumunda atlanır: BLE 5-10 mA tüketir, sync
     * sürecinde gereksiz. Normal boot'ta arka planda advertise eder, kullanıcı
     * iPhone Settings → Bluetooth'tan "NarRTOS Watch" görüp eşleştirir.
     * İlk eşleşmeden sonra her boot'ta NVS'teki bond ile otomatik reconnect.
     *
     * Akış: ble_service init → connect/bonded → ancs_service GATT discovery
     * → Notification Source/Data Source subscribe → bildirim → store → UI. */
    if (!power_service_is_sync_wake()) {
        notification_store_init();
        ancs_service_init();
        ble_service_set_connection_cb(
            [](uint16_t conn_handle, void* /*ud*/) {
                ESP_LOGI(TAG, "BLE bonded — starting ANCS discovery");
                ancs_service_on_connected(conn_handle);
            },
            [](uint16_t conn_handle, void* /*ud*/) {
                ancs_service_on_disconnected(conn_handle);
            },
            NULL);
        esp_err_t ble_err = ble_service_init();
        if (ble_err != ESP_OK) {
            ESP_LOGW(TAG, "BLE init failed (0x%x) — notifications disabled", ble_err);
        }
    }

    s_boot_events = xEventGroupCreate();
    configASSERT(s_boot_events);

    // ── Spawn tasks pinned to specific cores ──────────────────────────────
    // Her spawn dönüş değeri kontrol edilir; herhangi biri başarısızsa boot
    // tamamlanmaz (fail-fast → wdt reset → next boot temiz başlar). Sessiz
    // başarısızlık daha kötü: cihaz açılır görünür ama sensor/power servisi
    // çalışmaz, kullanıcı saatin "donmuş" olduğunu sanar.
    BaseType_t r;

    /* Sync wake (RTC timer'dan uyanma) durumunda UI task'ını HIÇ SPAWN
     * ETMİYORUZ. Apple Watch tarzı sessiz uyanma: display off kalır,
     * LVGL init olmaz, kullanıcı uyanmayı fark etmez. Sadece sensor +
     * power + sync_wake_task çalışır → OTA check + (varsa) install +
     * tekrar deep sleep. Display init için co5300_init bu task içinde
     * yapılıyordu; atlanırsa display SLPIN'de kalır (zaten deep sleep
     * prep'inde co5300_sleep(true) yapılmıştı). */
    const bool sync_wake = power_service_is_sync_wake();
    if (sync_wake) {
        ESP_LOGI(TAG, "Sync wake — UI task spawn SKIPPED (silent mode)");
    } else {
        r = xTaskCreatePinnedToCore(
            task_ui, "ui",
            12288,                  // 12KB: LVGL + touch + app manager + SPIFFS first-boot format
            NULL, UI_TASK_PRIORITY, NULL,
            UI_TASK_CORE            // Core 0
        );
        if (r != pdPASS) { ESP_LOGE(TAG, "UI task spawn FAILED — aborting boot"); abort(); }
    }

    r = xTaskCreatePinnedToCore(
        task_sensor, "sensor",
        6144,                   // 6KB: I2C + NVS + float aritmetik
        NULL, SENSOR_TASK_PRIORITY, NULL,
        SENSOR_TASK_CORE        // Core 1
    );
    if (r != pdPASS) { ESP_LOGE(TAG, "Sensor task spawn FAILED — aborting boot"); abort(); }

    r = xTaskCreatePinnedToCore(
        task_power, "power",
        4096,                   // 4KB: I2C + esp_sleep API
        NULL, POWER_TASK_PRIORITY, NULL,
        SENSOR_TASK_CORE        // Core 1 — shares core with sensor
    );
    if (r != pdPASS) { ESP_LOGE(TAG, "Power task spawn FAILED — aborting boot"); abort(); }

    /* Açılış donanım checklist'i — narrtos-board-selftest'in otomatik testleri,
     * sonuçlar YALNIZ seri monitöre basılır (ekrana çizilmez). Soğuk açılışta
     * bir kez; sessiz sync uyanışında ve deep sleep uyanışında çalışmaz. */
    if (!sync_wake) {
        selftest_service_start();
    }

    // ── Sync wake task (sadece RTC timer'dan uyandıysak) ──────────────────
    // Saat deep sleep'ten timer-wake ile çıktıysa burada özel bir akış var:
    //   1. WiFi up gelmesini bekle (15 sn timeout)
    //   2. Heartbeat/veri gönder (şu an: skip — endpoint yok)
    //   3. ota_service_check_async — manifest forced=true ise install_task
    //      auto-tetiklenir, install+reboot ota_service içinde halledilir.
    //   4. Forced yoksa check + UP_TO_DATE → tekrar deep sleep'e gir
    //      (60 sn timer ile bir sonraki sync wake için).
    // Bu task SADECE timer wake'te spawn edilir; normal boot'ta yok.
    if (power_service_is_sync_wake()) {
        ESP_LOGI(TAG, "Sync wake detected — spawning sync_wake_task");
        r = xTaskCreatePinnedToCore(
            task_sync_wake, "sync_wake",
            6144, NULL, SENSOR_TASK_PRIORITY, NULL,
            SENSOR_TASK_CORE
        );
        if (r != pdPASS) ESP_LOGE(TAG, "sync_wake_task spawn FAILED");
    }

    // app_main must not return; FreeRTOS scheduler takes over
    // (ESP-IDF already started the scheduler before calling app_main)
}
