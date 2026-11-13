#include "time_service.h"
#include "../drivers/rtc/pcf85063_driver.h"
#include "../core/event_bus.h"
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_system.h>          // esp_reset_reason — deep sleep wake check
#include <nvs.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "TimeSvc";
static uint8_t s_sync_counter = 0;
static uint16_t s_nvs_save_counter = 0;
static bool s_sntp_started = false;
static bool s_ntp_synced = false;

/* ─── NVS time backup ──────────────────────────────────────────────────────────
 * PCF85063 RTC chip backup pili / supercap olmayan boardlarda USB-power
 * kesilince zamanı kaybeder (boot'ta tm_year < 2025 ile gelir → time_service
 * compile zamanına düşer). NVS partition app flash dışında (offset 0x9000),
 * `pio run -t upload` flash'ı sadece app0/app1'i siler; NVS dokunulmaz.
 * Dolayısıyla:
 *   - NTP başarılı olduğunda epoch hem RTC'ye hem NVS'e yazılır
 *   - time_service_update periyodik olarak (saatte 1) NVS'i tazeler
 *   - Boot'ta RTC geçersizse NVS'ten en son bilinen zaman geri yüklenir
 *     (güç-kesik süresince donmuş kalır → ileride NTP yeniden senkronlar)
 *
 * Avantaj: kullanıcı bir kez WiFi+NTP eşledikten sonra reflash etse bile
 * watch yaklaşık doğru zamanı gösterir; WiFi'yi yeniden açmaya gerek kalmaz. */
#define TIME_NVS_NAMESPACE  "time_svc"
#define TIME_NVS_KEY_EPOCH  "epoch"

static void persist_time_to_nvs(time_t epoch)
{
    if (epoch <= 0) return;
    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_i64(h, TIME_NVS_KEY_EPOCH, (int64_t)epoch) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static bool load_time_from_nvs(time_t* out_epoch)
{
    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    int64_t epoch = 0;
    bool ok = (nvs_get_i64(h, TIME_NVS_KEY_EPOCH, &epoch) == ESP_OK) && epoch > 0;
    nvs_close(h);
    if (ok) *out_epoch = (time_t)epoch;
    return ok;
}

static void sntp_sync_cb(struct timeval* tv)
{
    (void)tv;
    /* SNTP has already called settimeofday(UTC) before invoking this
     * callback, so system epoch is now correct. Persist it to RTC
     * (storing TRT wall-clock fields, to match the get/set contract)
     * so the next cold boot without WiFi also starts with correct
     * time. Without this step, the RTC keeps advancing from whatever
     * stale compile-time was written at boot, and every subsequent
     * boot without NTP is wrong again.
     *
     * This callback runs in the SNTP LwIP task, which has its own
     * stack; keep work minimal (no blocking I/O besides the short
     * I²C write inside pcf85063_set_time). */
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    (void)pcf85063_set_time(&ti);
    persist_time_to_nvs(now);   /* Pil yedeği yoksa NVS reflash sonrası kurtarır */
    s_ntp_synced = true;
    ESP_LOGI(TAG, "NTP auto-sync: %04d-%02d-%02d %02d:%02d:%02d TRT → RTC + NVS updated",
        ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
        ti.tm_hour, ti.tm_min, ti.tm_sec);
}

/* Centralized SNTP bring-up. Idempotent: safe to call many times. */
static void start_sntp_daemon_once(void)
{
    if (s_sntp_started) return;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "tr.pool.ntp.org");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP daemon started (will auto-sync when network is up)");
}

void time_service_ntp_autosync_start(void)
{
    start_sntp_daemon_once();
}

bool time_service_is_ntp_synced(void)
{
    return s_ntp_synced;
}

/* Public: power_service deep sleep'e girmeden hemen önce çağırır.
 * Uyku boyunca herhangi bir ray kesintisinde (örn. AXP2101 sleep mode'u
 * sonradan tekrar etkinleştirilirse veya pil tükenirse) RTC durur ama
 * NVS flash'ta saklanan epoch sayesinde wake'te time_service_init NVS'ten
 * en son bilinen zamanı kurtarır. tm_year < 2025 ise compile fallback'tir
 * → NVS'i kirletmemek için kaydetmez. */
void time_service_save_to_nvs(void)
{
    time_t now = 0;
    time(&now);
    if (now <= 0) return;
    struct tm ti;
    localtime_r(&now, &ti);
    if (ti.tm_year + 1900 < 2025) {
        ESP_LOGD(TAG, "save_to_nvs skipped: time looks like compile fallback (%04d)",
            ti.tm_year + 1900);
        return;
    }
    persist_time_to_nvs(now);
    ESP_LOGI(TAG, "Time saved to NVS: %04d-%02d-%02d %02d:%02d:%02d TRT (pre-sleep)",
        ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
        ti.tm_hour, ti.tm_min, ti.tm_sec);
}

/**
 * @brief Derleme zamanını struct tm olarak döndürür.
 *
 * __DATE__ formatı: "Apr  9 2026"
 * __TIME__ formatı: "14:30:00"
 *
 * Geliştirici Türkiye'de (UTC+3) derlediği için bu saat TRT'dir.
 * mktime() zaten TZ=TRT-3 ile çalışacak, epoch doğru hesaplanır.
 */
static struct tm build_compile_time(void)
{
    struct tm t = {};
    const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                             "Jul","Aug","Sep","Oct","Nov","Dec"};
    for (int i = 0; i < 12; i++) {
        if (strncmp(__DATE__, months[i], 3) == 0) { t.tm_mon = i; break; }
    }
    // __DATE__ + 4: gün (boşluklu: "  9" → atoi=9)
    t.tm_mday = atoi(__DATE__ + 4);
    // __DATE__ + 7: yıl "2026"
    t.tm_year = atoi(__DATE__ + 7) - 1900;
    // __TIME__: "HH:MM:SS" — atoi ':' de durur
    t.tm_hour = atoi(__TIME__);
    t.tm_min  = atoi(__TIME__ + 3);
    t.tm_sec  = atoi(__TIME__ + 6);
    t.tm_isdst = -1;
    return t;
}

void time_service_init(void)
{
    // Türkiye saati: UTC+3, DST yok (2016'dan beri kalıcı yaz saati)
    setenv("TZ", "TRT-3", 1);
    tzset();

    /* 2026-05-13 — Adım 2 power optimization:
     * Deep sleep'ten uyandıysak ESP32 internal RTC zamanı tutmuş olmalı
     * (DCDC1 sleep boyunca açık, RTC clock saymaya devam etti). PCF85063
     * artık deep sleep'te güçsüz kalıyor → wake'te onu okumak anlamsız +
     * I2C hatası verir. Mevcut system time'a güvenip erken çıkıyoruz.
     *
     * Kontrol: time(NULL) 2025'ten yeniyse → ESP RTC zamanı geçerli, kullan.
     * Aksi halde (POR/SW reset → time 0 epoch) → normal init mantığına devam. */
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        time_t current = time(NULL);
        struct tm now_tm;
        localtime_r(&current, &now_tm);
        if (now_tm.tm_year + 1900 >= 2025) {
            ESP_LOGI(TAG, "Deep sleep wake — ESP32 RTC time valid (epoch=%lld, %04d-%02d-%02d %02d:%02d:%02d), skipping PCF85063",
                     (long long)current,
                     now_tm.tm_year + 1900, now_tm.tm_mon + 1, now_tm.tm_mday,
                     now_tm.tm_hour, now_tm.tm_min, now_tm.tm_sec);
            return;
        }
        ESP_LOGW(TAG, "Deep sleep wake but ESP RTC invalid — falling through to normal init");
    }

    // Derleme zamanı (geliştirici TRT bölgesinde derlediği için TRT)
    struct tm ct = build_compile_time();
    time_t compile_epoch = mktime(&ct);

    struct tm rtc_time = {};
    bool rtc_ok    = (pcf85063_get_time(&rtc_time) == ESP_OK);
    bool rtc_valid = rtc_ok && (rtc_time.tm_year + 1900 >= 2025);

    time_t use_epoch = compile_epoch;
    const char* src  = "compile";

    /* NVS'te en son bilinen NTP-eşli epoch — RTC backup'ı olarak (pil olmasa
     * bile reflash arası korunur). Boot'ta RTC ile NVS karşılaştırılır;
     * hangisi daha güncelse o kullanılır. */
    time_t nvs_epoch = 0;
    bool nvs_ok = load_time_from_nvs(&nvs_epoch);

    if (rtc_valid) {
        time_t rtc_epoch = mktime(&rtc_time);

        /* RTC vs NVS — hangisi daha güncelse onu kullan. PCF85063 pilsiz olup
         * sıfırlandıysa RTC güncel ama "donmuş eski" olabilir; NVS'teki son
         * NTP zamanı ondan ileride olabilir. */
        time_t pick = rtc_epoch;
        const char* pick_src = "RTC";
        if (nvs_ok && nvs_epoch > rtc_epoch) {
            pick = nvs_epoch;
            pick_src = "NVS (>RTC)";
            /* RTC'yi NVS değerine senkronla (pil yoksa zaten kaybolur ama
             * bu boot için tutarlı kalsın). */
            struct tm tmp;
            localtime_r(&nvs_epoch, &tmp);
            pcf85063_set_time(&tmp);
        }

        // Derleme zamanı seçilenden 10 dakikadan fazla ileriyse seçilen eskimiş
        double diff = difftime(compile_epoch, pick);
        if (diff > 600.0) {
            use_epoch = compile_epoch;
            src = "compile (>RTC/NVS)";
            pcf85063_set_time(&ct);   // RTC'yi de güncelle
        } else {
            use_epoch = pick;
            src = pick_src;
        }
    } else {
        // RTC okunamadı veya yıl geçersiz → NVS varsa onu kullan, yoksa compile
        if (nvs_ok && nvs_epoch > compile_epoch) {
            use_epoch = nvs_epoch;
            src = "NVS (RTC invalid)";
            /* RTC'ye yaz; pil varsa kalıcı olur, yoksa en azından bu boot
             * için time_service_update sync'inde tutarlı çalışır. */
            struct tm tmp;
            localtime_r(&nvs_epoch, &tmp);
            if (rtc_ok) pcf85063_set_time(&tmp);
        } else {
            use_epoch = compile_epoch;
            src = "compile (RTC+NVS invalid)";
            if (rtc_ok) pcf85063_set_time(&ct);
        }
    }

    struct timeval tv = { .tv_sec = use_epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    // Log: kullanılan zamanı TRT olarak göster
    struct tm display;
    localtime_r(&use_epoch, &display);
    ESP_LOGI(TAG, "Time set [%s]: %04d-%02d-%02d %02d:%02d:%02d TRT",
        src,
        display.tm_year + 1900, display.tm_mon + 1, display.tm_mday,
        display.tm_hour, display.tm_min, display.tm_sec);
}

void time_service_update(void)
{
    // Her 6 saniyede bir RTC sync (50ms × 120 = 6 s).
    // sensor_service ~50ms periyodla çağırıyor.
    if (++s_sync_counter < 120) return;
    s_sync_counter = 0;

    struct tm rtc_time;
    esp_err_t ret = pcf85063_get_time(&rtc_time);
    time_t epoch = 0;
    bool sync_ok = false;
    if (ret == ESP_OK) {
        epoch = mktime(&rtc_time);
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        sync_ok = true;
    } else {
        ESP_LOGD(TAG, "RTC sync skipped (err=0x%x)", ret);
    }

    /* ─── Her dakika değişiminde NVS yedekle ────────────────────────────────
     * Kullanıcı raporu 2026-05-05: "Cihaz dün akşamdan beri kapalıydı, açtım
     * saat yanlış." PCF85063 RTC'nin backup pili olmadığı için tam-power-off
     * sırasında saat kayboluyor → boot'ta NVS fallback kullanılıyor → NVS
     * ne kadar günceldi ise saat o kadar yakın çıkıyor.
     *
     * 2 dak → "minute-change" tabanlı save'e geçildi: dakika her değiştiğinde
     * (saniyede 60 sample → minute crossing'de bir kez) NVS yazılır.
     * Olası power-loss'ta saat en fazla 1 dakika geride kalır (öncesi 2 dak).
     *
     * NVS endurance: günlük 8 saat aktif × 60 yazma/saat = 480 yazma/gün
     * = 175K/yıl. Wear-leveling ile partition endurance ~600K toplam,
     * yaklaşık 3.5 yıl dayanır. Kabul edilebilir. */
    if (sync_ok && epoch > 0) {
        // Yıl sanity kontrolü: 2025'ten küçükse compile/eski zaman, kaydetme
        if (rtc_time.tm_year + 1900 >= 2025) {
            static int last_saved_min = -1;
            if (rtc_time.tm_min != last_saved_min) {
                last_saved_min = rtc_time.tm_min;
                persist_time_to_nvs(epoch);
                ESP_LOGD(TAG, "NVS time refreshed (minute change → %02d:%02d)",
                         rtc_time.tm_hour, rtc_time.tm_min);
            }
        }
    }
}

bool time_service_sync_from_ntp(uint32_t timeout_ms)
{
    /* Start the SNTP daemon if it hasn't been started yet. Once started,
     * it keeps polling in the background for the lifetime of the app,
     * so a later WiFi reconnect auto-syncs without another call. */
    start_sntp_daemon_once();

    /* Do NOT reset s_ntp_synced here — if a previous background sync
     * already succeeded, we want this call to return true immediately. */

    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (s_ntp_synced || esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            /* sntp_sync_cb already persisted to RTC; just log and return. */
            time_t now;
            struct tm ti;
            time(&now);
            localtime_r(&now, &ti);
            ESP_LOGI(TAG, "NTP sync OK: %04d-%02d-%02d %02d:%02d:%02d TRT",
                ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                ti.tm_hour, ti.tm_min, ti.tm_sec);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }

    ESP_LOGW(TAG, "NTP sync timeout (%ums) — daemon still running in background", (unsigned)timeout_ms);
    return false;
}
