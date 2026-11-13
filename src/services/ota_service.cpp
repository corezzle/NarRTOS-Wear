/**
 * @file ota_service.cpp
 * @brief HTTP manifest fetch + esp_https_ota tabanlı OTA implementation.
 */

#include "ota_service.h"
#include "wifi_service.h"
#include "../core/os_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_timer.h>
#include <time.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <atomic>          /* v1.4.55 audit #2a: s_busy race fix */

static const char* TAG = "OTA";

// NVS namespace + keys — kullanıcı tercihleri (auto-update toggle vs).
#define OTA_NVS_NS              "ota"
#define OTA_NVS_KEY_AUTO_UPDATE "auto_update"

// ─── State ───────────────────────────────────────────────────────────────────

static SemaphoreHandle_t s_mutex      = nullptr;
static bool              s_initialized = false;
/* v1.4.55 (2026-05-19) — audit orta öncelik #2a: race fix.
 * Önceden `static bool s_busy` + ayrı "if (s_busy) return; s_busy = true;"
 * iki adımdı → check_async ve install_async aynı anda çağrılırsa (double-tap,
 * dual-core scheduler interleave) ikisi de guard'ı geçip iki paralel task
 * spawn ediliyordu. std::atomic compare_exchange ile single-step. */
static std::atomic<bool> s_busy{false};

static ota_status_t      s_status      = {};

// Arkaplan auto-check timer'ı. İlk fire boot+5dk, sonra periodic 30dk.
static esp_timer_handle_t s_auto_check_timer = nullptr;
static bool               s_auto_first_check_done = false;

// Forced (admin push) install bayrağı — check_task forced=true gördüğünde
// install_async'i tetiklerken bunu set eder, install_task auto-reboot
// kararında bunu kontrol eder (toggle OFF olsa bile forced ise reboot).
static volatile bool s_forced_install = false;

// Gece penceresi sınırları (lokal saat). [BEGIN, END) → 00:00 ≤ saat < 06:00.
// Apple Watch tarzı: kullanıcı uyurken install + reboot olsun, sabah kalkınca
// saat zaten yeni sürümde olsun. Bu pencerenin dışında otomatik install
// tetiklenmez (UPDATE_AVAILABLE state'inde kalır, manuel başlatılabilir).
#define OTA_NIGHT_WINDOW_BEGIN 0
#define OTA_NIGHT_WINDOW_END   6
static char              s_firmware_url[256] = {0};
static char              s_device_id[13] = {0};   /* 12 hex + NUL, MAC cache */

// ─── Device ID (WiFi MAC, 12 uppercase hex) ──────────────────────────────────

static const char* get_device_id(void)
{
    if (s_device_id[0] != '\0') return s_device_id;

    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_read_mac failed: %s", esp_err_to_name(err));
        snprintf(s_device_id, sizeof(s_device_id), "000000000000");
        return s_device_id;
    }
    snprintf(s_device_id, sizeof(s_device_id),
             "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "device_id=%s", s_device_id);
    return s_device_id;
}

// ─── JSON parse helpers (weather_service ile aynı yaklaşım) ──────────────────

/* "key":"value" formatından value'yu çıkarıp out buffer'ına kopyala. */
static bool extract_string(const char* json, const char* key,
                           char* out, size_t outsz)
{
    const char* p = strstr(json, key);
    if (!p) return false;
    p += strlen(key);
    while (*p && *p != '"') ++p;
    if (*p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outsz) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

/* "key":INT formatından integer çıkar. Bulamazsa def döner. */
static int extract_int(const char* json, const char* key, int def)
{
    const char* p = strstr(json, key);
    if (!p) return def;
    p += strlen(key);
    while (*p && (*p == ':' || *p == ' ' || *p == '"' || *p == '\t')) ++p;
    return (int)strtol(p, NULL, 10);
}

// ─── HTTP buffer (manifest fetch için) ───────────────────────────────────────

typedef struct {
    char*  buf;
    size_t cap;
    size_t len;
} http_ctx_t;

static esp_err_t http_event_cb(esp_http_client_event_t* evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    http_ctx_t* ctx = (http_ctx_t*)evt->user_data;
    if (!ctx || !ctx->buf) return ESP_OK;
    size_t rem = (ctx->cap > ctx->len + 1) ? (ctx->cap - ctx->len - 1) : 0;
    if (rem == 0) return ESP_OK;
    size_t copy = (size_t)evt->data_len;
    if (copy > rem) copy = rem;
    memcpy(ctx->buf + ctx->len, evt->data, copy);
    ctx->len += copy;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

// ─── State helpers ───────────────────────────────────────────────────────────

static void set_state(ota_state_t st)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = st;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "State -> %d", (int)st);
}

static void set_error(const char* msg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = OTA_STATE_ERROR;
    strncpy(s_status.error_msg, msg, sizeof(s_status.error_msg) - 1);
    s_status.error_msg[sizeof(s_status.error_msg) - 1] = '\0';
    xSemaphoreGive(s_mutex);
    ESP_LOGE(TAG, "Error: %s", msg);
}

static void set_progress(uint8_t pct)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.progress_pct = pct;
    xSemaphoreGive(s_mutex);
}

// ─── Gece penceresi (auto-install için) ──────────────────────────────────────

static bool is_night_window(void)
{
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    /* v1.0.1 fix: Zaman geçerli mi? RTC pili yoksa veya NTP henüz sync
     * olmadıysa tm_year compile fallback'i olabilir (< 2025). Compile
     * timestamp'i gece dilimine denk gelirse boot'ta yanlış tetiklenir
     * → auto-install istenmeyen anda çalışır. Geçersiz zamanda asla
     * tetikleme. */
    if (tm_local.tm_year + 1900 < 2025) return false;
    int h = tm_local.tm_hour;
    return (h >= OTA_NIGHT_WINDOW_BEGIN) && (h < OTA_NIGHT_WINDOW_END);
}

// ─── Arkaplan auto-check timer callback ──────────────────────────────────────

static void auto_check_timer_cb(void* /*arg*/)
{
    // İlk fire one-shot 5dk sonraydı; bundan sonra periodic 30dk'ya geç.
    // esp_timer'ı stop edip periodic moda başlatıyoruz; aynı handle reuse.
    if (!s_auto_first_check_done) {
        s_auto_first_check_done = true;
        if (s_auto_check_timer) {
            esp_timer_stop(s_auto_check_timer);
            // 30 dakika = 30 * 60 * 1e6 us
            esp_timer_start_periodic(s_auto_check_timer,
                                     30ULL * 60ULL * 1000ULL * 1000ULL);
        }
    }

    // Conditions: auto_update ON, WiFi var, başka iş yapmıyoruz.
    if (!ota_service_get_auto_update()) {
        ESP_LOGD(TAG, "auto_check: setting OFF, skipping");
        return;
    }
    if (!wifi_service_is_connected()) {
        ESP_LOGD(TAG, "auto_check: no wifi, skipping");
        return;
    }
    if (s_busy) {
        ESP_LOGD(TAG, "auto_check: busy, skipping");
        return;
    }

    ESP_LOGI(TAG, "auto_check fired -> check_async");
    ota_service_check_async();
    // check_task içinde UPDATE_AVAILABLE bulursa is_night_window() kontrolü
    // yapılır; pencere içindeyse install_async otomatik tetiklenir.
}

// ─── Manifest fetch task ─────────────────────────────────────────────────────

static void check_task(void* /*arg*/)
{
    set_state(OTA_STATE_CHECKING);

    if (!wifi_service_is_connected()) {
        set_error("No Wi-Fi");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    /* HTTP buffer — manifest beklenen ~256-512 byte; 2 KB güvenli üst sınır. */
    char* buf = (char*)malloc(2048);
    if (!buf) {
        set_error("Out of memory");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    http_ctx_t ctx = { buf, 2048, 0 };
    buf[0] = '\0';

    /* Manifest URL'i runtime'da kur: device_id (MAC) + current version.
     * Backend bu iki query param'a göre release seçimi yapar veya 204 döner. */
    char manifest_url[256];
    snprintf(manifest_url, sizeof(manifest_url),
             OTA_MANIFEST_URL_TEMPLATE,
             get_device_id(), OS_VERSION_STR);
    ESP_LOGI(TAG, "Manifest URL: %s", manifest_url);

    esp_http_client_config_t cfg = {};
    cfg.url           = manifest_url;
    cfg.timeout_ms    = OTA_HTTP_TIMEOUT_MS;
    cfg.event_handler = http_event_cb;
    cfg.user_data     = &ctx;
    cfg.buffer_size   = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf);
        set_error("Failed to create HTTP client");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    /* Hata mesajını kullanıcı için anlaşılır yap. perform() seviyesinde fail
     * (DNS / TCP / TLS) ile sunucudan dönen HTTP error code'unu ayrı ayrı
     * göster. Default placeholder URL `corezzle.example.com` resolve etmez →
     * kullanıcıya "Sunucu bulunamadi (manifest URL ayarli mi?)" mesajı,
     * gerçek backend down ise "Sunucu hatasi (404)" gibi.
     *
     * NOT: ESP-IDF esp_http_client perform() DNS fail durumunda ESP_ERR_HTTP_*
     * değil ESP_FAIL döndürebiliyor; spesifik kod seti net değil. Bu yüzden
     * any-non-OK durumda generic "Aglarda hata" + hex code log'la (kullanıcı
     * destek için yararlı). */
    if (err != ESP_OK) {
        char msg[64];
        if (err == ESP_ERR_HTTP_CONNECT) {
            snprintf(msg, sizeof(msg), "Server not found");
        } else {
            snprintf(msg, sizeof(msg), "Network error (0x%x)", err);
        }
        ESP_LOGE(TAG, "perform() failed: err=%s url=%s",
                 esp_err_to_name(err), manifest_url);
        free(buf);
        set_error(msg);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }
    if (status < 200 || status >= 300) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Server error (HTTP %d)", status);
        ESP_LOGE(TAG, "HTTP status %d url=%s", status, manifest_url);
        free(buf);
        set_error(msg);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    /* HTTP 204 No Content = backend "saat zaten guncel" diyor; body bos olur. */
    if (status == 204) {
        free(buf);
        ESP_LOGI(TAG, "Up to date (HTTP 204)");
        set_state(OTA_STATE_UP_TO_DATE);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    /* Parse manifest */
    char    version[24] = {0};
    int     code        = extract_int(buf, "\"version_code\"", -1);
    char    url[256]    = {0};
    int     size        = extract_int(buf, "\"size\"", 0);
    char    notes[128]  = {0};
    extract_string(buf, "\"version\"", version, sizeof(version));
    extract_string(buf, "\"url\"",     url,     sizeof(url));
    extract_string(buf, "\"notes\"",   notes,   sizeof(notes));
    /* "forced":true → admin push edilmiş zorla yükleme. JSON boolean'lar
     * sade strstr ile yakalanır (cJSON yok). false ise veya alan yoksa
     * normal akış (gece penceresi + auto_update toggle koşulları). */
    bool forced = (strstr(buf, "\"forced\":true") != NULL);
    free(buf);

    if (code < 0 || url[0] == '\0') {
        set_error("Invalid manifest");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Manifest: version=%s code=%d size=%d url=%s",
             version, code, size, url);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_status.latest_version, version, sizeof(s_status.latest_version) - 1);
    s_status.latest_version[sizeof(s_status.latest_version) - 1] = '\0';
    s_status.latest_code   = code;
    s_status.download_size = (uint32_t)size;
    strncpy(s_status.notes, notes, sizeof(s_status.notes) - 1);
    s_status.notes[sizeof(s_status.notes) - 1] = '\0';
    s_status.progress_pct  = 0;
    xSemaphoreGive(s_mutex);

    /* URL'yi servis-içi sabit buffer'a sakla — install_task kullanacak */
    strncpy(s_firmware_url, url, sizeof(s_firmware_url) - 1);
    s_firmware_url[sizeof(s_firmware_url) - 1] = '\0';

    if (code > OS_VERSION_CODE) {
        ESP_LOGI(TAG, "Update available: current=%d latest=%d",
                 OS_VERSION_CODE, code);
        set_state(OTA_STATE_UPDATE_AVAILABLE);
    } else {
        ESP_LOGI(TAG, "Up to date: current=%d latest=%d",
                 OS_VERSION_CODE, code);
        set_state(OTA_STATE_UP_TO_DATE);
    }

    s_busy = false;

    /* Otomatik install tetikleyici. İki yol var:
     *   - Forced (admin push) → her zaman tetikle. Gece penceresi + auto_update
     *     toggle kontrolleri bypass edilir; admin "şimdi yükle" demiştir.
     *   - Normal → auto_update setting açıksa VE gece penceresindeyse tetikle.
     *
     * İkisi de yoksa state UPDATE_AVAILABLE'da kalır, kullanıcı manuel
     * "Indir ve Yukle" basabilir veya bir sonraki gece penceresinde
     * (toggle açıksa) otomatik tetiklenir. */
    if (code > OS_VERSION_CODE) {
        if (forced) {
            ESP_LOGI(TAG, "Auto-install: FORCED (admin push) -> install_async");
            s_forced_install = true;
            ota_service_install_async();
        } else if (ota_service_get_auto_update() && is_night_window()) {
            ESP_LOGI(TAG, "Auto-install: gece penceresi + setting on -> install_async");
            ota_service_install_async();
        }
    }

    vTaskDelete(NULL);
}

// ─── Event reporter (best-effort, backend ulasilamazsa sessiz gec) ───────────

static void post_event(const char* event, const char* error_msg)
{
    if (!wifi_service_is_connected()) {
        ESP_LOGW(TAG, "post_event '%s' skipped (no wifi)", event);
        return;
    }

    /* Snapshot version stringlerini mutex altinda al */
    char to_version[24];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(to_version, s_status.latest_version, sizeof(to_version) - 1);
    to_version[sizeof(to_version) - 1] = '\0';
    xSemaphoreGive(s_mutex);

    char body[512];
    if (error_msg && error_msg[0]) {
        snprintf(body, sizeof(body),
                 "{\"device_id\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
                 "\"event\":\"%s\",\"error\":\"%s\"}",
                 get_device_id(), OS_VERSION_STR, to_version,
                 event, error_msg);
    } else {
        snprintf(body, sizeof(body),
                 "{\"device_id\":\"%s\",\"from\":\"%s\",\"to\":\"%s\","
                 "\"event\":\"%s\"}",
                 get_device_id(), OS_VERSION_STR, to_version, event);
    }

    esp_http_client_config_t cfg = {};
    cfg.url        = OTA_EVENTS_URL;
    cfg.timeout_ms = OTA_HTTP_TIMEOUT_MS;
    cfg.method     = HTTP_METHOD_POST;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "post_event '%s' init failed", event);
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Event '%s' posted (HTTP %d)", event, status);
    } else {
        ESP_LOGW(TAG, "Event '%s' POST failed: %s", event, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

// ─── esp_https_ota install task ──────────────────────────────────────────────

static void install_task(void* /*arg*/)
{
    set_state(OTA_STATE_DOWNLOADING);
    set_progress(0);

    if (!wifi_service_is_connected()) {
        set_error("No Wi-Fi");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    if (s_firmware_url[0] == '\0') {
        set_error("URL empty");
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    /* Telemetry: install basladi */
    post_event("started", NULL);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url               = s_firmware_url;
    http_cfg.timeout_ms        = OTA_HTTP_TIMEOUT_MS;
    http_cfg.keep_alive_enable = true;
    http_cfg.buffer_size       = OTA_DOWNLOAD_BUF_SZ;
    /* HTTP fallback — production'da HTTPS + cert pinning gerekir.
     * sdkconfig CONFIG_OTA_ALLOW_HTTP=y olduğu için HTTP de çalışır;
     * v2 release'inde mTLS upgrade. */

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "OTA start error: 0x%x", err);
        post_event("failed", msg);
        set_error(msg);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    int total       = esp_https_ota_get_image_size(handle);
    if (total <= 0 && s_status.download_size > 0) total = s_status.download_size;
    int read_so_far = 0;
    uint8_t last_pct = 0;

    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        int now = esp_https_ota_get_image_len_read(handle);
        if (total > 0 && now >= 0) {
            uint8_t pct = (uint8_t)(((int64_t)now * 100) / total);
            if (pct != last_pct) {
                last_pct = pct;
                set_progress(pct);
            }
        }
        read_so_far = now;
        /* Yumuşak yield — diğer task'lar (UI vs) çalışsın */
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        char msg[64];
        snprintf(msg, sizeof(msg), "Download error: 0x%x", err);
        post_event("failed", msg);
        set_error(msg);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    /* Image complete — finalize, validate, set boot partition */
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Finalize error: 0x%x", err);
        post_event("failed", msg);
        set_error(msg);
        s_busy = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA installed: %d bytes downloaded, reboot pending",
             read_so_far);
    set_progress(100);
    set_state(OTA_STATE_INSTALLED);

    /* Telemetry: install basariyla bitti (reboot bekleniyor). */
    post_event("success", NULL);

    s_busy = false;

    /* Otomatik reboot:
     *   - Forced install: toggle off olsa bile reboot lazım (admin zorla
     *     yükledi, kullanıcı ekrana bakmıyor).
     *   - Auto-update toggle ON: gece otomatik install sonrası reboot.
     *   - Manuel install (toggle OFF, forced değil): kullanıcı "Simdi Baslat"
     *     butonuna basana kadar bekle (mevcut davranış).
     *
     * 5 saniyelik delay UI poll devraldıysa "Yeniden baslatiliyor / Lutfen
     * bekleyin" mesajını gösterme şansı verir. */
    bool was_forced = s_forced_install;
    s_forced_install = false;  // tek seferlik, sonraki cycle için temizle
    if (was_forced || ota_service_get_auto_update()) {
        ESP_LOGI(TAG, "Auto-reboot: 5s delay -> esp_restart (forced=%d, toggle=%d)",
                 (int)was_forced, (int)ota_service_get_auto_update());
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        // never returns
    }

    vTaskDelete(NULL);
}

// ─── Public API ──────────────────────────────────────────────────────────────

void ota_service_init(void)
{
    if (s_initialized) return;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Mutex create failed");
        return;
    }
    s_status.state = OTA_STATE_IDLE;
    s_initialized  = true;

    /* Boot sonrası yeni firmware'i "valid" olarak markla — rollback enabled
     * değilse no-op, ileride enabled olduğunda crash-loop koruması verir. */
    ota_service_confirm_boot();

    ESP_LOGI(TAG, "OTA service initialized (version=%s code=%d)",
             OS_VERSION_STR, OS_VERSION_CODE);
}

void ota_service_check_async(void)
{
    if (!s_initialized) return;
    /* v1.4.55 audit #2a: atomic compare-exchange — race fix.
     * compare_exchange başarısızsa s_busy zaten true → başka task aktif. */
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) return;

    /* 8 KB stack — HTTP + JSON parse + heap alloc */
    BaseType_t r = xTaskCreatePinnedToCore(
        check_task, "ota_check", 8192, NULL, 3, NULL, 1);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "check_task spawn failed");
        s_busy = false;
        set_error("Failed to create task");
    }
}

void ota_service_install_async(void)
{
    if (!s_initialized) return;
    /* Sadece available state'inde tetiklen */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool can_install = (s_status.state == OTA_STATE_UPDATE_AVAILABLE);
    xSemaphoreGive(s_mutex);
    if (!can_install) {
        ESP_LOGW(TAG, "install_async: state not UPDATE_AVAILABLE, ignoring");
        return;
    }
    /* v1.4.55 audit #2a: atomic compare-exchange (state check sonrası gerek
     * çünkü state OTA_STATE_UPDATE_AVAILABLE iken double-tap olabilir). */
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) return;

    /* esp_https_ota internally needs ~6 KB; 10 KB güvenli */
    BaseType_t r = xTaskCreatePinnedToCore(
        install_task, "ota_install", 10240, NULL, 3, NULL, 1);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "install_task spawn failed");
        s_busy = false;
        set_error("Failed to create task");
    }
}

ota_state_t ota_service_get_status(ota_status_t* out)
{
    if (!s_initialized || !s_mutex) {
        if (out) memset(out, 0, sizeof(*out));
        return OTA_STATE_IDLE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (out) *out = s_status;
    ota_state_t st = s_status.state;
    xSemaphoreGive(s_mutex);
    return st;
}

void ota_service_confirm_boot(void)
{
    /* Yeni boot sonrası mevcut partition'ı "valid" olarak markla. Rollback
     * disabled iken bu çağrı no-op'tur ama API kullanılması gelecek-uyumlu. */
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Boot confirmed: %s marked valid", running->label);
        }
    }
}

// ─── Kullanıcı tercihi: Otomatik Güncellemeler (NVS-persisted) ───────────────

bool ota_service_get_auto_update(void)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, OTA_NVS_KEY_AUTO_UPDATE, &v) != ESP_OK) v = 0;
    nvs_close(h);
    return v != 0;
}

void ota_service_set_auto_update(bool enabled)
{
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "auto_update: NVS open failed");
        return;
    }
    nvs_set_u8(h, OTA_NVS_KEY_AUTO_UPDATE, enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "auto_update preference saved: %s", enabled ? "ON" : "OFF");
}

void ota_service_start_auto_check(void)
{
    if (s_auto_check_timer) return;  /* idempotent */

    const esp_timer_create_args_t args = {
        .callback = &auto_check_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ota_auto_check",
        .skip_unhandled_events = false,
    };
    if (esp_timer_create(&args, &s_auto_check_timer) != ESP_OK) {
        ESP_LOGE(TAG, "auto_check_timer create failed");
        return;
    }
    /* İlk fire one-shot: 5 dakika sonra. Callback içinde periodic'e geç. */
    esp_timer_start_once(s_auto_check_timer,
                         5ULL * 60ULL * 1000ULL * 1000ULL);
    ESP_LOGI(TAG, "Auto-check scheduled: 5 min initial, then 30 min periodic");
}
