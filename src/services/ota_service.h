#pragma once
/**
 * @file ota_service.h
 * @brief WiFi üzerinden OTA (Over-The-Air) firmware güncelleme servisi.
 *
 * Akış:
 *   1. Kullanıcı Settings → General → Software Updates'e tıklar.
 *   2. ota_service_check_async() → manifest.json'u HTTP GET eder.
 *   3. Manifest'teki version_code mevcut OS_VERSION_CODE'dan büyükse
 *      "Update available" gösterilir, aksi halde "Up to date".
 *   4. Kullanıcı "Install"a tıklarsa ota_service_install_async() →
 *      esp_https_ota ile pasif partition'a flash, sha256 doğrulama,
 *      esp_ota_set_boot_partition, esp_restart.
 *
 * Tasarım notları:
 *   - Tüm uzun işler ayrı FreeRTOS task'ında (UI bloklamasın).
 *   - State + progress callback'lerle UI güncellenir; callback'ler LVGL
 *     thread'ine `lv_async_call` ile post edilir (UI thread-safety).
 *   - Manifest parse: weather_service'le aynı strstr-based hafif çıkarım;
 *     cJSON bağımlılığı yok.
 *   - A/B partition layout (partitions_custom.csv): app0/app1, esp_ota_*
 *     API'leri otomatik pasif partition'ı seçer.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_STATE_IDLE             = 0,  // Hiçbir iş yok / başlangıç
    OTA_STATE_CHECKING         = 1,  // Manifest fetch ediliyor
    OTA_STATE_UP_TO_DATE       = 2,  // Manifest version_code <= mevcut
    OTA_STATE_UPDATE_AVAILABLE = 3,  // Yeni sürüm bulundu
    OTA_STATE_DOWNLOADING      = 4,  // Firmware indiriliyor + flash
    OTA_STATE_INSTALLED        = 5,  // İndirme bitti, reboot bekleniyor
    OTA_STATE_ERROR            = 6,  // Network/parse/flash hatası
} ota_state_t;

typedef struct {
    ota_state_t state;
    char        latest_version[24];   // "1.1.0" gibi
    int32_t     latest_code;          // 10100 gibi (mevcutla compare için)
    char        notes[128];           // Kullanıcıya gösterilecek release notes
    uint32_t    download_size;        // Toplam byte
    uint8_t     progress_pct;         // 0..100 (DOWNLOADING state'inde)
    char        error_msg[64];        // ERROR state'inde insan-okur mesaj
} ota_status_t;

/**
 * Servisi başlat. main.cpp içinde WiFi init sonrası çağrılır. UI ekranına
 * bağlı bir polling/timer kurmaz; check_async / install_async ile tetiklenir.
 */
void ota_service_init(void);

/**
 * Manifest'i fetch et ve karşılaştır. Async; arka plan task spawn eder.
 * UI bu süre boyunca CHECKING state'inde spinner göstermeli.
 *
 * Tamamlanınca state UP_TO_DATE / UPDATE_AVAILABLE / ERROR'a geçer; UI bu
 * geçişi `ota_service_get_status` polling ile veya callback ile öğrenir.
 *
 * Eğer halihazırda bir check/install devam ediyorsa noop döner.
 */
void ota_service_check_async(void);

/**
 * Mevcut available update'i indir + flash et. Async; download bittiğinde
 * INSTALLED state'ine geçer. ERROR durumunda durum mesajı kullanıcıya
 * gösterilir.
 *
 * Sadece state == UPDATE_AVAILABLE iken çalışır.
 */
void ota_service_install_async(void);

/**
 * Güncel durumu kopyala (mutex-korumalı). UI 1 Hz polling ile çağırır.
 * @return state değeri
 */
ota_state_t ota_service_get_status(ota_status_t* out);

/**
 * Sürümün başarıyla yüklendiğini onayla (boot sonrası). esp_ota_mark_app_valid_*
 * çağırır → CONFIG_APP_ROLLBACK_ENABLE açıksa anti-rollback'i tetikler.
 * v1'de rollback enabled değil; çağrı no-op'a benzer ama gelecek-uyumlu.
 */
void ota_service_confirm_boot(void);

/**
 * Kullanıcı tercihi (NVS-persisted): Otomatik Güncellemeler.
 *
 * Açıkken: install tamamlanınca UI otomatik reboot eder ("Simdi Baslat"
 * butonuna basmaya gerek kalmaz). Kullanıcı kontrolünü kaybetmesin diye
 * 5 sn'lik bir countdown gösterilir.
 *
 * Kapalıyken: mevcut davranış — "Simdi Baslat" butonu çıkar, manuel
 * tetikleme.
 *
 * Default: false (kullanıcı bilinçli açsın).
 */
bool ota_service_get_auto_update(void);
void ota_service_set_auto_update(bool enabled);

/**
 * Arkaplan otomatik check timer'ını başlat. main.cpp boot sonrası bir
 * kere çağırır. Timer şöyle çalışır:
 *
 *   - İlk fire: boot'tan 5 dakika sonra (WiFi + servisler hazır olsun)
 *   - Sonraki fire'lar: her 30 dakikada bir
 *   - Her fire'da: auto_update setting ON mı? WiFi var mı? Busy mı?
 *     hepsi tamam ise check_async tetiklenir.
 *
 * check_task'ın sonunda (UPDATE_AVAILABLE bulunursa) "gece penceresi"
 * (lokal saat 00:00-06:00) kontrol edilir; içindeyse install_async
 * otomatik tetiklenir → kullanıcı uyanırken saat zaten güncel olur.
 *
 * Pencere dışında: state UPDATE_AVAILABLE'da kalır, kullanıcı Software
 * Updates'i açtığında "Indir ve Yukle" butonuyla manuel başlatabilir;
 * veya bir sonraki gece penceresine kadar bekler.
 */
void ota_service_start_auto_check(void);

#ifdef __cplusplus
}
#endif
