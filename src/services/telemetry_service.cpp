/**
 * @file telemetry_service.cpp
 * @brief Periyodik telemetri raporu — backend üzerinden araç takip köprüsüne.
 *
 * Tasarım (Faz 3):
 *   - FreeRTOS task (Core 1, düşük öncelik), 60 sn periyot
 *   - WiFi yoksa sessizce bekle (offline kuyruk YOK — v1 basit)
 *   - HTTP POST timeout 5 sn, başarısızlık = log + geç (fire-and-forget)
 *   - device_id = WiFi STA MAC, 12 büyük hex (backend sözleşmesi)
 *
 * Bu dosyanın tamamı BOARD_VARIANT_LCD128 guard'ında — AMOLED build'ı
 * byte-etkisiz kalır (kullanıcı kuralı). Kararlılık kanıtlanınca guard
 * kaldırılıp her iki board'da da etkinleştirilebilir.
 */

#ifdef TRACKER_FEATURES

#include "telemetry_service.h"
#include "wifi_service.h"
#include "gps_service.h"
#include "activity_service.h"
#include "can_service.h"
#include "../core/os_config.h"
#include <nvs.h>

#include <esp_http_client.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "TelemetrySvc";

#define TELEMETRY_PATH         "/api/wear-os/telemetry"
#define TELEMETRY_NVS_BACKEND  "backend"
static char s_backend[96] = OTA_BACKEND_HOST;
static char s_url[128];
static const char* telemetry_url(void) { snprintf(s_url, sizeof(s_url), "%s" TELEMETRY_PATH, s_backend); return s_url; }
#define TELEMETRY_PERIOD_DEFAULT_S 60
#define TELEMETRY_PERIOD_MIN_S     10
#define TELEMETRY_PERIOD_MAX_S     3600
#define TELEMETRY_NVS_NS   "telemetry"
#define TELEMETRY_NVS_KEY  "period_s"
#define TELEMETRY_NVS_REST_KEY   "rest_s"
#define TELEMETRY_NVS_AFTER_KEY  "rest_after"
#define TELEMETRY_REST_PERIOD_DEFAULT_S 600
#define TELEMETRY_REST_AFTER_DEFAULT_S  300
/* WiFi taraması bağlantıyı ~20 sn böldüğü için her raporda değil, N turda bir
 * yapılır (N=5 → ~5 dk'da bir konum tazelenir). Tarama turunda rapor atlanır. */
#define TELEMETRY_SCAN_EVERY_CYCLES 5
#define TELEMETRY_BOOT_DELAY_MS (20 * 1000)   /* boot + WiFi otursun */
#define TELEMETRY_HTTP_TIMEOUT_MS 5000

static char s_device_id[13] = {0};
static uint32_t s_period_s = TELEMETRY_PERIOD_DEFAULT_S;        /* aktif mod */
static uint32_t s_rest_period_s = TELEMETRY_REST_PERIOD_DEFAULT_S;
static uint32_t s_rest_after_s  = TELEMETRY_REST_AFTER_DEFAULT_S;
static tracker_mode_t s_mode = TRK_MODE_ACTIVE;
static int s_force = -1;
static volatile bool s_send_now = false;
static uint32_t s_sent_count = 0;
static int      s_last_http  = 0;
static SemaphoreHandle_t s_send_lock;   /* task ve komut aynı anda göndermesin */

static void load_period(void)
{
    nvs_handle_t h;
    if (nvs_open(TELEMETRY_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint32_t v = 0;
    if (nvs_get_u32(h, TELEMETRY_NVS_KEY, &v) == ESP_OK &&
        v >= TELEMETRY_PERIOD_MIN_S && v <= TELEMETRY_PERIOD_MAX_S) s_period_s = v;
    if (nvs_get_u32(h, TELEMETRY_NVS_REST_KEY, &v) == ESP_OK &&
        v >= TELEMETRY_PERIOD_MIN_S && v <= TELEMETRY_PERIOD_MAX_S) s_rest_period_s = v;
    if (nvs_get_u32(h, TELEMETRY_NVS_AFTER_KEY, &v) == ESP_OK && v >= 10 && v <= 86400) s_rest_after_s = v;
    size_t len = sizeof(s_backend);
    char tmp[96];
    if (nvs_get_str(h, TELEMETRY_NVS_BACKEND, tmp, &len) == ESP_OK && strncmp(tmp, "http", 4) == 0) strlcpy(s_backend, tmp, sizeof(s_backend));
    nvs_close(h);
}

static void build_device_id(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* Faz 2: yakındaki AP taraması cache'i. Aktif tarama
 * BT+WiFi coex'te bağlantıyı ~20 sn bölüyor (POST'u bozar) — bu yüzden tarama
 * her rapor turunda DEĞİL, seyrek (bkz. TELEMETRY_SCAN_EVERY_CYCLES) yapılır;
 * sonuç burada JSON dizi olarak tutulup her raporda kullanılır. Yalnızca
 * telemetry_task okur/yazar → mutex gerekmez. */
static char s_wifi_json[WIFI_SCAN_MAX_APS * 40 + 8] = "[]";

/* Tarama yap + s_wifi_json'ı yenile. Bağlantıyı böleceği için çağıran bu tur
 * rapor GÖNDERMEZ; bir sonraki tur (WiFi toparlanınca) cache ile temiz gider. */
static void refresh_wifi_scan(void)
{
    size_t w = 0;
    s_wifi_json[w++] = '[';
    wifi_bssid_ap_t aps[WIFI_SCAN_MAX_APS];
    uint8_t ap_count = 0;
    if (wifi_service_scan_bssids(aps, WIFI_SCAN_MAX_APS, &ap_count) == ESP_OK) {
        for (uint8_t i = 0; i < ap_count; i++) {
            int m = snprintf(s_wifi_json + w, sizeof(s_wifi_json) - w,
                "%s{\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"rssi\":%d}",
                i ? "," : "",
                aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5],
                aps[i].rssi);
            if (m <= 0 || (size_t)m >= sizeof(s_wifi_json) - w) break;
            w += (size_t)m;
        }
    }
    s_wifi_json[w++] = ']';
    s_wifi_json[w]   = '\0';
    ESP_LOGI(TAG, "wifi scan refreshed: %u AP", ap_count);
}

static void send_report(void)
{
    /* WiFi RSSI — bağlı AP'den anlık sinyal gücü */
    wifi_ap_record_t ap = {};
    int rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;

    /* GPS: taze fix varsa payload'a eklenir; backend GPS'i WiFi'ye tercih
     * eder (cam kenarı → gps, iç mekân → wifi). Fix yoksa alan hiç gitmez. */
    char gps_json[160] = "";
    gps_fix_t fix;
    if (gps_service_get_fix(&fix)) {
        snprintf(gps_json, sizeof(gps_json),
            ",\"gps\":{\"lat\":%.6f,\"lng\":%.6f,\"hdop\":%.1f,\"sats\":%u,\"age_s\":%u%s}",
            fix.lat, fix.lng, fix.hdop, fix.sats, (unsigned)fix.age_s,
            fix.simulated ? ",\"sim\":true" : "");
    }

    activity_t act; activity_service_get(&act);
    /* CAN: kimlik başına son ham frame'ler (çözümsüz). Tablo boşsa alan hiç gitmez.
     * Statik tampon: 48 frame × ~36 karakter telemetri görevinin yığınını şişirmesin. */
    static char can_json[CAN_FRAMES_PER_REPORT * 40 + 96];
    can_json[0] = ',';
    if (!can_service_json(can_json + 1, sizeof(can_json) - 1)) can_json[0] = 0;
    static char body[1024 + sizeof(can_json)];
    int n = snprintf(body, sizeof(body),
        "{\"device_id\":\"%s\",\"wifi_rssi\":%d,\"wifi\":%s,\"uptime_s\":%lld,"
        "\"firmware_version\":\"%s\",\"heap_free\":%u,\"period_s\":%u,"
        "\"mode\":\"%s\",\"activity\":{\"state\":\"%s\",\"steps\":%u,\"steps_per_min\":%u,"
        "\"intensity\":%u,\"since_motion_s\":%u,\"taps_per_min\":%u%s}%s%s}",
        s_device_id, rssi, s_wifi_json,
        (long long)(esp_timer_get_time() / 1000000),
        OS_VERSION_STR,
        (unsigned)esp_get_free_heap_size(),
        (unsigned)(s_mode == TRK_MODE_ACTIVE ? s_period_s : s_rest_period_s),
        s_mode == TRK_MODE_ACTIVE ? "active" : "rest",
        activity_state_name(act.state), (unsigned)act.steps_total, (unsigned)act.steps_per_min,
        (unsigned)act.intensity, (unsigned)act.since_motion_s, (unsigned)act.taps_per_min, act.simulated ? ",\"sim\":true" : "",
        gps_json, can_json);
    if (n <= 0 || n >= (int)sizeof(body)) return;

    esp_http_client_config_t cfg = {};
    cfg.url            = telemetry_url();
    cfg.method         = HTTP_METHOD_POST;
    cfg.timeout_ms     = TELEMETRY_HTTP_TIMEOUT_MS;
    cfg.disable_auto_redirect = true;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return;

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, body, n);

    esp_err_t err = esp_http_client_perform(cli);
    if (err == ESP_OK) {
        int code = esp_http_client_get_status_code(cli);
        s_last_http = code; s_sent_count++;
        ESP_LOGI(TAG, "report sent (HTTP %d, rssi=%d, gps=%s)", code, rssi, gps_json[0] ? "yes" : "no");
    } else {
        s_last_http = -1;
        ESP_LOGW(TAG, "report failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(cli);
}

/* Mod makinesi: true dönerse mod değişti (hemen rapor gerek). */
static bool update_mode(void)
{
    tracker_mode_t want;
    if (s_force == 0) want = TRK_MODE_REST;
    else if (s_force == 1) want = TRK_MODE_ACTIVE;
    else {
        activity_t a; activity_service_get(&a);
        want = (a.since_motion_s >= s_rest_after_s) ? TRK_MODE_REST : TRK_MODE_ACTIVE;
    }
    if (want == s_mode) return false;
    s_mode = want;
    gps_service_set_enabled(want == TRK_MODE_ACTIVE);
    ESP_LOGI(TAG, "mode -> %s (period=%u s, gps=%s)",
             want == TRK_MODE_ACTIVE ? "ACTIVE" : "REST",
             (unsigned)(want == TRK_MODE_ACTIVE ? s_period_s : s_rest_period_s),
             want == TRK_MODE_ACTIVE ? "on" : "off");
    return true;
}

static void telemetry_task(void* arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(TELEMETRY_BOOT_DELAY_MS));
    ESP_LOGI(TAG, "telemetry loop started (period=%u s, url=%s)",
             (unsigned)s_period_s, telemetry_url());

    uint32_t cycle = 0;
    for (;;) {
        if (wifi_service_is_connected()) {
            /* İlk tur + her TELEMETRY_SCAN_EVERY_CYCLES turda bir: WiFi tara
             * (cache'i yenile), bağlantı bozulacağı için bu tur rapor YOK.
             * Diğer turlar: cache'lenmiş wifi listesiyle temiz rapor. */
            if (cycle % TELEMETRY_SCAN_EVERY_CYCLES == 0) {
                refresh_wifi_scan();
            } else {
                xSemaphoreTake(s_send_lock, portMAX_DELAY);
                send_report();
                xSemaphoreGive(s_send_lock);
            }
        }
        cycle++;
        /* Periyot bekleme: 1 sn adımlarla. Her saniye mod makinesi:
         *   hareket → ACTIVE (GPS aç, hemen rapor);  rest_after s hareketsiz →
         *   REST (son rapor, GPS kapat). Mod değişimi beklemeyi keser. */
        uint32_t waited = 0;
        for (;;) {
            uint32_t period = (s_mode == TRK_MODE_ACTIVE) ? s_period_s : s_rest_period_s;
            if (waited >= period || s_send_now) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
            waited++;
            if (update_mode()) break;
        }
        s_send_now = false;
    }
}

esp_err_t telemetry_service_set_period(uint32_t seconds)
{
    if (seconds < TELEMETRY_PERIOD_MIN_S) seconds = TELEMETRY_PERIOD_MIN_S;
    if (seconds > TELEMETRY_PERIOD_MAX_S) seconds = TELEMETRY_PERIOD_MAX_S;
    s_period_s = seconds;
    nvs_handle_t h;
    if (nvs_open(TELEMETRY_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    nvs_set_u32(h, TELEMETRY_NVS_KEY, seconds);
    nvs_commit(h); nvs_close(h);
    ESP_LOGI(TAG, "period set to %u s (NVS)", (unsigned)seconds);
    return ESP_OK;
}
uint32_t telemetry_service_get_period(void) { return s_period_s; }

esp_err_t telemetry_service_set_backend(const char* base)
{
    if (!base || strncmp(base, "http", 4) != 0 || strlen(base) >= sizeof(s_backend)) return ESP_ERR_INVALID_ARG;
    strlcpy(s_backend, base, sizeof(s_backend));
    size_t n = strlen(s_backend); while (n && s_backend[n-1] == '/') s_backend[--n] = 0;   /* sondaki / */
    nvs_handle_t h;
    if (nvs_open(TELEMETRY_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    nvs_set_str(h, TELEMETRY_NVS_BACKEND, s_backend); nvs_commit(h); nvs_close(h);
    ESP_LOGI(TAG, "backend set: %s", s_backend);
    return ESP_OK;
}
const char* telemetry_service_get_backend(void) { return s_backend; }

static esp_err_t nvs_put_u32(const char* key, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(TELEMETRY_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    nvs_set_u32(h, key, v); nvs_commit(h); nvs_close(h); return ESP_OK;
}
esp_err_t telemetry_service_set_rest_period(uint32_t sec)
{
    if (sec < TELEMETRY_PERIOD_MIN_S) sec = TELEMETRY_PERIOD_MIN_S;
    if (sec > TELEMETRY_PERIOD_MAX_S) sec = TELEMETRY_PERIOD_MAX_S;
    s_rest_period_s = sec; return nvs_put_u32(TELEMETRY_NVS_REST_KEY, sec);
}
esp_err_t telemetry_service_set_rest_after(uint32_t sec)
{
    if (sec < 10) sec = 10;
    if (sec > 86400) sec = 86400;
    s_rest_after_s = sec; return nvs_put_u32(TELEMETRY_NVS_AFTER_KEY, sec);
}
uint32_t telemetry_service_get_rest_period(void) { return s_rest_period_s; }
uint32_t telemetry_service_get_rest_after(void)  { return s_rest_after_s; }
tracker_mode_t telemetry_service_get_mode(void)  { return s_mode; }
void telemetry_service_force_mode(int f) { s_force = (f < 0) ? -1 : (f ? 1 : 0); s_send_now = true; }
int  telemetry_service_get_force(void)   { return s_force; }
uint32_t telemetry_service_sent_count(void) { return s_sent_count; }
int      telemetry_service_last_http(void)  { return s_last_http; }

esp_err_t telemetry_service_send_now(void)
{
    if (!wifi_service_is_connected()) return ESP_ERR_INVALID_STATE;
    if (!s_send_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    send_report();
    xSemaphoreGive(s_send_lock);
    s_send_now = true;   /* döngü sayacını da sıfırla */
    return ESP_OK;
}

esp_err_t telemetry_service_init(void)
{
    build_device_id();
    load_period();
    s_send_lock = xSemaphoreCreateMutex();
    BaseType_t r = xTaskCreatePinnedToCore(
        telemetry_task, "telemetry",
        4096, NULL, 3, NULL, 1 /* Core 1 */);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "task spawn failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "init OK (device_id=%s)", s_device_id);
    return ESP_OK;
}

#endif /* TRACKER_FEATURES */
