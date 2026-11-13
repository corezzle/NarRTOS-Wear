#pragma once

// ─── OS Version ──────────────────────────────────────────────────────────────
// v0.1.0: ilk public sürüm. Multi-city weather, selector smoothness, UI polish,
// OTA (Software Updates) altyapısı. Major sürüm 0 = ürün hala pre-release;
// API/ABI stabilite garantisi yok, breaking change'ler MINOR'a girebilir.
// Major bump (1.0.0) ileride hardware / API stabilize olunca yapılacak.
#define OS_VERSION_MAJOR 1
#define OS_VERSION_MINOR 4
#define OS_VERSION_PATCH 81
#define OS_VERSION_STR   "1.4.81"

/* OS_VERSION_CODE — manifest karşılaştırma için tek tam sayı.
 *   0.1.0  → 100
 *   0.1.5  → 105
 *   0.2.0  → 200
 *   0.10.3 → 1003     (MINOR 10 → 0.10.3 = 1003, 0.2.0 = 200; 1003 > 200 ✓)
 *   1.0.0  → 10000
 * Manifest "version_code" alanı bu integer ile karşılaştırılır; semantic
 * version string parse'ı yerine direkt int compare → sürüm 0.10 vs 0.2 gibi
 * leksikografik tuzaklara düşmez (string compare "0.10" < "0.2" hatasına). */
#define OS_VERSION_CODE  ((OS_VERSION_MAJOR) * 10000 + \
                          (OS_VERSION_MINOR) *   100 + \
                          (OS_VERSION_PATCH))

// ─── OTA / Software Update ────────────────────────────────────────────────────
//
// Manifest endpoint'i: cihaz "Software Updates"e tıkladığında bu URL'yi GET
// eder, gelen JSON'dan en güncel sürümü öğrenir, mevcut OS_VERSION_CODE'dan
// büyükse "Update available" gösterir.
//
// Manifest JSON formatı (örnek):
//   {
//     "version":      "1.1.0",        // human-readable
//     "version_code": 10100,          // OS_VERSION_CODE ile karşılaştır
//     "url":          "https://corezzle.example.com/firmware/wear-os-1.1.0.bin",
//     "size":         2480000,        // byte (UI'de "MB" gösterimi için)
//     "notes":        "Bug fixes and selector mode improvements"
//   }
//
// Backend kurulumu (production'da yapılacak):
//   - HTTPS sunucu (nginx/Cloudflare Pages/S3+CloudFront)
//   - manifest.json + firmware-X.Y.Z.bin dosyalarını host et
//   - Build pipeline her release'de otomatik manifest update'i yapsın
//
// Şimdilik HTTP fallback (CONFIG_OTA_ALLOW_HTTP=y, sdkconfig); v2'de mTLS +
// CA cert pinning eklenecek. Manifest URL'si build-time tanımlı; ileride
// Settings'ten kullanıcının değiştirmesine gerek yok (channel kavramı
// olmadıkça beta/stable ayrımı için).
//
// Backend: corezzle-wearos-backend (Express + MikroORM + Postgres wear_os schema).
// Lokalde Mac LAN IP'si üzerinden HTTP, prod'da public domain + HTTPS.
#ifndef OTA_BACKEND_HOST
#define OTA_BACKEND_HOST "http://192.168.1.50:3001"
#endif

/* Manifest endpoint — runtime'da device_id (12-hex MAC) ve current version
 * ile doldurulur. Backend bu iki query param'a göre uygun release'i seçer
 * veya 204 No Content döner (saat zaten güncelse). */
#ifndef OTA_MANIFEST_URL_TEMPLATE
#define OTA_MANIFEST_URL_TEMPLATE OTA_BACKEND_HOST "/api/wear-os/manifest?device_id=%s&current=%s"
#endif

/* Update lifecycle telemetry endpoint — install başladı/başarılı/başarısız
 * raporlamak için. Best-effort: erişilemezse OTA akışı bozulmaz, sessiz geç. */
#ifndef OTA_EVENTS_URL
#define OTA_EVENTS_URL OTA_BACKEND_HOST "/api/wear-os/events"
#endif

/* ─── Store endpoints (v1.2.7) ────────────────────────────────────────────────
 * App Store / mağaza uygulaması için backend catalog endpoint'leri. Backend
 * (corezzle-imager-backend/wear-os modülü) tarafından sağlanır. Metadata-only —
 * gerçek binary saatte zaten compile edilmiş. Backend sadece `app_id` string
 * ve metadata sunar; saat tarafında `app_id` → `APP_ID_*` enum mapping yapılır. */
#ifndef STORE_CATALOG_URL
#define STORE_CATALOG_URL OTA_BACKEND_HOST "/api/wear-os/store/apps"
#endif

#ifndef STORE_EVENTS_URL
#define STORE_EVENTS_URL  OTA_BACKEND_HOST "/api/wear-os/store/events"
#endif

#define STORE_HTTP_TIMEOUT_MS   8000     /* Catalog fetch timeout */
#define STORE_CATALOG_BUF_SZ    8192     /* HTTP response buffer (JSON) */
#define STORE_REMOTE_MAX        16       /* Memory'de tutulan max remote app sayısı */

#define OTA_HTTP_TIMEOUT_MS  15000     // Manifest fetch + her HTTP req timeout
#define OTA_DOWNLOAD_BUF_SZ  4096      // esp_https_ota chunk buffer
#define OTA_PROGRESS_PUSH_MS 250       // Progress UI update min interval

// ─── GPIO Pin Map ─────────────────────────────────────────────────────────────
// v1.2.9: Board varyantı — esp32s3_lcd128 env'i (-DBOARD_VARIANT_LCD128) kendi
// pin haritasını ayrı dosyadan alır. Default AMOLED build'ında bu macro tanımsız
// → aşağıdaki 2.06" pin map'i OLDUĞU GİBİ derlenir, davranış değişmez.
#ifdef BOARD_VARIANT_LCD128
#include "os_config_lcd128_pins.h"
#else

// (ESP32-S3-Touch-AMOLED-2.06)
// Display (CO5300/SH8601 QSPI) — verified from BSP
#define PIN_DISP_CS         12
#define PIN_DISP_SCLK       11
#define PIN_DISP_D0          4
#define PIN_DISP_D1          5
#define PIN_DISP_D2          6
#define PIN_DISP_D3          7
#define PIN_DISP_RST         8
#define PIN_DISP_TE         -1   // Not connected on this board

// I2C Bus (shared: FT3168, AXP2101, PCF85063) — verified from BSP
#define PIN_I2C_SDA         15
#define PIN_I2C_SCL         14
#define I2C_MASTER_FREQ_HZ  400000   // 400kHz Fast Mode

// I2C Addresses
#define I2C_ADDR_FT3168     0x38
#define I2C_ADDR_ICM42670   0x68
#define I2C_ADDR_AXP2101    0x34
#define I2C_ADDR_PCF85063   0x51
#define I2C_ADDR_CST820     0x15   // NAR-ESP32S3 dokunmatik (FT3168 yerine)

// NAR-ESP32S3 (SiP iç şeması): RTC kesme çıkışı PCF85063 INT → GPIO39.
// Henüz kullanılmıyor (saat polling ile okunuyor); alarm/uyandırma için hazır.
#ifdef BOARD_VARIANT_NAR
#define PIN_RTC_INT         39
#endif

// Touch
#define PIN_TOUCH_RST        9
#define PIN_TOUCH_INT       38

// IMU IRQ. Hazır geliştirme kartlarında QMI8658 INT1 → GPIO21 ama sürücü ICM42670 olduğu için kullanılmıyor.
// NAR şeması (2026-11-04): ICM-42670-P pin 4 INT1 → GPIO21 (SiP pin 27, TP43), INT2 bağlı değil.
// GPIO21 RTC IO → derin uykudan EXT1 ile uyandırabilir. Donanımcı doğruladı (2026-11-04).
#ifdef BOARD_VARIANT_NAR
#define PIN_IMU_INT1        21
#else
#define PIN_IMU_INT1        -1   // Not used
#endif

// PMIC IRQ
#define PIN_PMIC_IRQ        -1   // Not wired on this board

// Buttons
#define PIN_BTN_TOP          0   // Active LOW (boot button reused)
#define PIN_BTN_BOTTOM      10   // Active LOW (side button, doğrulandı)

// TF Card (SDMMC)
#define PIN_SD_D0            3
#define PIN_SD_CMD           1
#define PIN_SD_CLK           2

// Buzzer / Speaker (LEDC PWM ile sürülür — alarm tonları için)
// ESP32-S3'te dahili DAC yoktur; tone üretmenin tek yolu LEDC üzerinden
// square wave PWM'dir. GPIO 21 boş ve LEDC uyumlu (octal PSRAM 33-37'yi
// kullanır → 33+ pinler kullanılamaz). Kullanıcı board'unda buzzer veya
// pasif piezo speaker'ı bu pin ile GND arasına bağlamalıdır. Aktif
// buzzer'lar (kendi osilatörü olan) tek bir frekans üretir → tercih edilmez;
// pasif piezo (~3-12 V tolerant, 16-32 Ω veya yüksek empedans) ile çoklu
// frekans alarm pattern'i çalınabilir. Hardware speaker yoksa bu pin
// sessiz kalır, alarm UI normal çalışır.
// NAR: buzzer YOK (şema 2026-11-04: titreşim motoru GPIO18/Q1); GPIO21 = IMU INT1 → LEDC çıkışı
// IMU'nun push-pull INT çıkışına karşı sürerdi. -1 = audio_service başlatılmaz.
#ifndef PIN_BUZZER
#ifdef BOARD_VARIANT_NAR
#define PIN_BUZZER          -1
#else
#define PIN_BUZZER          21
#endif
#endif

#endif /* BOARD_VARIANT_LCD128 */

// ─── Task Configuration ───────────────────────────────────────────────────────
#ifndef UI_TASK_CORE
#define UI_TASK_CORE       0
#endif

#ifndef SENSOR_TASK_CORE
#define SENSOR_TASK_CORE   1
#endif

#ifndef UI_TASK_PRIORITY
#define UI_TASK_PRIORITY   5
#endif

#ifndef SENSOR_TASK_PRIORITY
#define SENSOR_TASK_PRIORITY 4
#endif

#ifndef POWER_TASK_PRIORITY
#define POWER_TASK_PRIORITY  6
#endif

// ─── App IDs ──────────────────────────────────────────────────────────────────
typedef enum {
    APP_ID_WATCHFACE    = 0,
    APP_ID_STEP_COUNTER = 1,
    APP_ID_SETTINGS     = 2,
    APP_ID_LAUNCHER     = 3,
    APP_ID_WATCHFACE2   = 4,
    APP_ID_HEARTRATE    = 5,
    APP_ID_WEATHER      = 6,
    APP_ID_TIMER        = 7,
    APP_ID_NOTIFY       = 8,
    APP_ID_GALLERY      = 9,
    APP_ID_ALARM        = 10,
    APP_ID_STORE        = 11,   /* v1.2.4 (2026-05-19): App Store / mağaza */
    APP_ID_HQMSG        = 12,   /* v1.2.9 (2026-08-19): Genel Müdürlük mesaj ekranı */
    APP_ID_COUNT
} app_id_t;

// ─── Display Buffer ───────────────────────────────────────────────────────────
// v1.4.54 (2026-05-19): Audit #6 fix — target 320 → 80.
//
// Önceki tasarım (320 satır → ekranın 2/3'ü tek chunk) iyi niyetli ama
// gerçeklikte tutmuyordu: ESP32-S3R8 internal SRAM = 512 KB; statik kullanım
// + FreeRTOS task stack'leri + driver buffer'ları sonrası DMA-capable
// contiguous serbest alan ~250-350 KB. 320×2 = 524 KB target asla allocate
// edilemiyordu → fallback ladder ile sürekli düşüyordu (en son saatte ölçüm:
// lines=60, 49200 B × 2 = 98 KB). Sorun: ladder `lines /= 2` ile büyük
// adımlarla düşüyor (320→160→80→40) → boot heap fragmentation'a göre
// **non-deterministic** allocation seviyesi. Bir boot 80, diğer 40, diğer
// 60 (eski ladder ile 240→120→60) → reproducible olmayan render perf.
//
// Yeni yaklaşım: **target = 80 (deterministic floor) + smart ladder**.
//   - 80 lines × 410 × 2 = 65,600 B per buffer → 131 KB double-buffer
//   - 131 KB internal DMA-RAM tipik boot fragmentation'da rahat sığar
//   - ui_service ladder: 80 → 60 → 40 → 20 → 10 → 8 (linear düşüş, /=2
//     yerine -20 step) → fallback regression daha sığ
//   - Chunk büyüklüğü: 410 × 80 = ekranın ~%16'sı (320'deki ~%64'ten az ama
//     mevcut ölçülen 60'tan büyük → hafif iyileşme)
//   - Full-screen flush: 502/80 = 7 chunk (önceden 60'da 9 chunk idi)
//
// Audit #6: "DISPLAY_BUF_LINES = 60-80, double-buffer'ı garanti altına al"
// → ulaşılan: target=80, deterministic, double-buffer her durumda.
//
// Ayırılma (target hit): 80 × 410 × 2 = 65,600 B per buffer = 131 KB toplam.
#ifndef DISPLAY_BUF_LINES
#define DISPLAY_BUF_LINES  80
#endif

// ─── Batarya ─────────────────────────────────────────────────────────────────
// Kartta TEK pil var (+ USB girişi). Şarj akımı bu kapasiteden 0.3C olarak
// hesaplanır (axp2101_set_charge_current_auto): ör. 340 mAh → 100 mA,
// 1000 mAh → 300 mA. AXP2101'in yazabildiği kademeye AŞAĞI yuvarlanır.
// !!! KENDİ PİLİNİN GERÇEK mAh DEĞERİNİ YAZ !!!
#ifndef BATT_CAPACITY_MAH
#define BATT_CAPACITY_MAH  500
#endif
