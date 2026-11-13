/**
 * @file store_service.cpp
 * @brief Backend catalog fetcher — v1.2.7 (2026-05-22).
 *
 * Backend `GET /api/wear-os/store/apps` → in-memory cache. Async fetch
 * (xTaskCreate), her remote entry için `app_id` → `APP_ID_*` mapping ve
 * `min_firmware_version_code` kontrolü.
 *
 * Pattern referans: `services/ota_service.cpp` (HTTP event callback + buffer
 * accumulate). Burada cJSON kullanıyoruz çünkü 15+ field var, strstr kırılgan.
 */

#include "store_service.h"
#include "store_state.h"
#include "../../core/os_config.h"
#include "../../services/wifi_service.h"

#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>

static const char* TAG = "StoreService";

// ─── Cache state ─────────────────────────────────────────────────────────────

static SemaphoreHandle_t   s_lock          = NULL;   /* cache read/write koruma */
static store_remote_app_t  s_cache[STORE_REMOTE_MAX];
static size_t              s_cache_count   = 0;
static int64_t             s_last_ok_us    = 0;      /* son başarılı refresh */
static volatile bool       s_busy          = false;  /* in-flight guard */

/* HTTP buffer accumulator — ota_service pattern'i */
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

// ─── Init ────────────────────────────────────────────────────────────────────

void store_service_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "lock create fail");
        return;
    }
    s_cache_count = 0;
    s_last_ok_us  = 0;
    s_busy        = false;
    ESP_LOGI(TAG, "store_service init OK");
}

// ─── Cache accessors ────────────────────────────────────────────────────────

size_t store_service_remote_count(void)
{
    if (!s_lock) return 0;
    size_t c;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c = s_cache_count;
    xSemaphoreGive(s_lock);
    return c;
}

const store_remote_app_t* store_service_remote_at(size_t idx)
{
    if (!s_lock) return NULL;
    if (idx >= STORE_REMOTE_MAX) return NULL;
    /* Snapshot pointer; caller refresh sırasında okumamalı (UI task tek erişimci
     * — refresh async sonrası bir frame async geçişle pointer geçerli kalır). */
    return &s_cache[idx];
}

bool store_service_has_cache(void)
{
    return s_last_ok_us > 0;
}

// ─── JSON parse ──────────────────────────────────────────────────────────────

/* Tek bir app JSON object'i → store_remote_app_t.
 * False return: gerekli alan eksik, entry skip edilir. */
static bool parse_app_object(const cJSON* obj, store_remote_app_t* out)
{
    if (!cJSON_IsObject(obj)) return false;
    memset(out, 0, sizeof(*out));

    const cJSON* j = cJSON_GetObjectItem(obj, "app_id");
    if (!cJSON_IsString(j) || !j->valuestring) return false;
    strncpy(out->app_id, j->valuestring, sizeof(out->app_id) - 1);

    j = cJSON_GetObjectItem(obj, "name");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(out->name, j->valuestring, sizeof(out->name) - 1);
    }

    j = cJSON_GetObjectItem(obj, "subtitle");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(out->subtitle, j->valuestring, sizeof(out->subtitle) - 1);
    }

    j = cJSON_GetObjectItem(obj, "description");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(out->description, j->valuestring, sizeof(out->description) - 1);
    }

    j = cJSON_GetObjectItem(obj, "icon_symbol");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(out->icon_symbol, j->valuestring, sizeof(out->icon_symbol) - 1);
    }

    j = cJSON_GetObjectItem(obj, "color_hex");
    if (cJSON_IsString(j) && j->valuestring) {
        uint8_t r = 0x80, g = 0x80, b = 0x80;   /* fallback gri */
        store_color_from_hex(j->valuestring, &r, &g, &b);
        out->cr = r; out->cg = g; out->cb = b;
    } else {
        out->cr = out->cg = out->cb = 0x80;
    }

    j = cJSON_GetObjectItem(obj, "version");
    if (cJSON_IsNumber(j)) out->version = (int)j->valuedouble;
    else                    out->version = 1;

    j = cJSON_GetObjectItem(obj, "min_firmware_version_code");
    if (cJSON_IsNumber(j)) out->min_firmware_version_code = (int)j->valuedouble;
    else                    out->min_firmware_version_code = 0;

    j = cJSON_GetObjectItem(obj, "display_order");
    if (cJSON_IsNumber(j)) out->display_order = (int)j->valuedouble;
    else                    out->display_order = 0;

    /* Runtime mapping */
    out->mapped_id        = store_appid_from_string(out->app_id);
    out->binary_available = (out->mapped_id != APP_ID_COUNT) &&
                            (OS_VERSION_CODE >= out->min_firmware_version_code);

    return true;
}

/* HTTP response body → cache. Locked write. */
static esp_err_t parse_catalog_into_cache(const char* json_str)
{
    cJSON* root = cJSON_Parse(json_str);
    if (!root) {
        const char* err_ptr = cJSON_GetErrorPtr();
        ESP_LOGE(TAG, "cJSON_Parse fail near: %s", err_ptr ? err_ptr : "(null)");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON* apps_arr = cJSON_GetObjectItem(root, "apps");
    if (!cJSON_IsArray(apps_arr)) {
        ESP_LOGE(TAG, "JSON: 'apps' missing or not array");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* v1.4.62 (2026-06-07): Direct write into s_cache under lock.
     *
     * Önceki tasarım `local[STORE_REMOTE_MAX]` ile çift kopyalıyordu — ya yığında
     * (13 KB → stack overflow, v1.4.61'de fix edildi) ya da heap'te calloc
     * (13 KB contiguous → fragmentation altında ESP_ERR_NO_MEM). Düzeltme:
     * geçici tampona hiç gerek yok. `s_cache_count = 0` yapıp lock tutarken
     * doğrudan s_cache[i]'ye yaz; count'u en sona artır → reader'lar count'a
     * göre iterate eder, partial state hiç görmez. Lock'un kapsadığı iş
     * sadece struct fill loop (HTTP/cJSON yok), tipik <1 ms. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cache_count = 0;   /* reader'lar bu noktadan sonra "boş" görür */

    cJSON* item = NULL;
    size_t i = 0;
    cJSON_ArrayForEach(item, apps_arr) {
        if (i >= STORE_REMOTE_MAX) {
            ESP_LOGW(TAG, "remote catalog > STORE_REMOTE_MAX (%d), truncated",
                     STORE_REMOTE_MAX);
            break;
        }
        if (parse_app_object(item, &s_cache[i])) {
            i++;
        }
    }

    s_cache_count = i;                       /* yayın: count atomik şekilde set */
    s_last_ok_us  = esp_timer_get_time();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "catalog refresh OK — %u apps", (unsigned)i);
    cJSON_Delete(root);
    return ESP_OK;
}

// ─── HTTP fetch task ─────────────────────────────────────────────────────────

typedef struct {
    store_service_refresh_cb_t cb;
    void*                       user_data;
} refresh_arg_t;

/* Helper — fetch + parse, result code döndürür. Caller cleanup + cb yapar. */
static esp_err_t do_fetch_catalog(int* out_count)
{
    *out_count = 0;

    char* buf = (char*)malloc(STORE_CATALOG_BUF_SZ);
    if (!buf) {
        ESP_LOGE(TAG, "buf malloc fail");
        return ESP_ERR_NO_MEM;
    }
    http_ctx_t ctx = { buf, STORE_CATALOG_BUF_SZ, 0 };
    buf[0] = '\0';

    esp_http_client_config_t cfg = {};
    cfg.url           = STORE_CATALOG_URL;
    cfg.timeout_ms    = STORE_HTTP_TIMEOUT_MS;
    cfg.event_handler = http_event_cb;
    cfg.user_data     = &ctx;
    cfg.buffer_size   = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init fail");
        free(buf);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "perform fail: %s", esp_err_to_name(err));
        free(buf);
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ctx.len == 0) {
        ESP_LOGE(TAG, "empty response");
        free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t parse_err = parse_catalog_into_cache(buf);
    free(buf);
    if (parse_err == ESP_OK) {
        *out_count = (int)store_service_remote_count();
    }
    return parse_err;
}

static void refresh_task(void* arg)
{
    refresh_arg_t* ra = (refresh_arg_t*)arg;
    int       count  = 0;
    esp_err_t result = do_fetch_catalog(&count);

    s_busy = false;
    if (ra && ra->cb) {
        ra->cb(result, count, ra->user_data);
    }
    free(ra);
    vTaskDelete(NULL);
}

esp_err_t store_service_refresh_async(store_service_refresh_cb_t cb, void* user_data)
{
    if (!s_lock) {
        ESP_LOGE(TAG, "refresh_async: not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        ESP_LOGW(TAG, "refresh_async: already in-flight, skip");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_service_is_connected()) {
        ESP_LOGW(TAG, "refresh_async: no WiFi, skip");
        return ESP_ERR_INVALID_STATE;   /* WiFi disconnected — generic state err */
    }

    refresh_arg_t* ra = (refresh_arg_t*)malloc(sizeof(*ra));
    if (!ra) return ESP_ERR_NO_MEM;
    ra->cb        = cb;
    ra->user_data = user_data;

    s_busy = true;
    /* v1.4.61: 6144 → 8192. esp_http_client + cJSON parse + recursive
     * cJSON_Delete birlikte 6 KB'a sığıyor ama emniyet payı kalmıyor. */
    BaseType_t r = xTaskCreate(refresh_task, "store_fetch", 8192, ra, 3, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "task spawn fail");
        s_busy = false;
        free(ra);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
