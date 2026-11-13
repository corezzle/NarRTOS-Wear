/**
 * @file weather_service.cpp
 * @brief IP geolocation (ip-api.com) + Open-Meteo, çoklu şehir.
 *
 * Tasarım notları (RAM'i az kullan, donmaları engelle):
 *   - Tek görev, Core 1, 6KB stack. UI/sensor task'larına dokunmaz.
 *   - HTTP stream event_handler içinde 2KB stack buffer; dinamik alloc yok.
 *   - JSON "parse" yerine hedef anahtarların etrafındaki substring'i strtof/
 *     strtol ile çıkarıyoruz. cJSON bağımlılığı yok.
 *   - İlk başarılı fetch sonrası lat/lon/city NVS'e yazılır → sonraki tüm
 *     çekimler ip-api.com'u atlar, yalnızca Open-Meteo'ya gider.
 *
 * Çoklu şehir (2026-05-05):
 *   - s_cities[] dizisi: Auto + Ankara + Izmir. Auto'nun lat/lon'u IP'den
 *     gelir; diğerleri hardcoded.
 *   - Her şehir kendi weather_data_t snapshot'ına ve NVS blob cache'ine sahip.
 *   - "Primary" idx (kullanıcının seçtiği) NVS'te kalıcı; UI bunu sorar.
 *   - Fetch loop her tick'te 3 şehri sırayla çeker.
 */

#include "weather_service.h"
#include "wifi_service.h"
#include "time_service.h"
#include "../core/event_bus.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

static const char* TAG = "WeatherSvc";

#define WEATHER_REFRESH_S   (15 * 60)   // 15 dakika
#define HTTP_BUF_SZ         3072      /* daily bloğu eklendi → ~2.4 KB cevap */
#define URL_BUF_SZ          384       /* current+daily parametreleri ile ~290 byte */
#define NVS_NAMESPACE       "weather"
/* Auto şehir lokasyonu (legacy key isimleri korunuyor → eski cihazlar boot'ta
 * cache'lerini kaybetmesin). */
#define NVS_KEY_LAT         "lat"
#define NVS_KEY_LON         "lon"
#define NVS_KEY_CITY        "city"
/* Per-city weather snapshot blob — index'li ("data0", "data1", "data2"). Eski
 * "data" key'i de Auto için fallback olarak okunuyor (boot uyumluluğu). */
#define NVS_KEY_DATA_LEGACY "data"
#define NVS_KEY_DATA_FMT    "data%d"
/* Aktif (primary) şehir idx'i — kullanıcının liste menüsünden seçtiği. */
#define NVS_KEY_PRIMARY     "primary"

// ─── Çoklu şehir state ────────────────────────────────────────────────────────

typedef struct {
    char            name[32];   /* Görünür ad (UI). Auto için IP detect; sabit cities için sabit. */
    float           lat;
    float           lon;
    bool            valid_loc;  /* Auto için: ip-api başarılı mı? Sabit cities için: hep true. */
    weather_data_t  snapshot;   /* Son başarılı fetch sonucu */
} weather_city_t;

static bool              s_initialized = false;
static SemaphoreHandle_t s_mutex       = nullptr;
static TaskHandle_t      s_task_handle = nullptr;
static weather_city_t    s_cities[WEATHER_CITY_COUNT];
static int               s_primary_idx = WEATHER_CITY_AUTO;

static void cities_init_defaults(void)
{
    /* Auto şehir — lat/lon NVS'ten veya IP detect'ten dolacak */
    strncpy(s_cities[WEATHER_CITY_AUTO].name, "Sehir",
            sizeof(s_cities[WEATHER_CITY_AUTO].name) - 1);
    s_cities[WEATHER_CITY_AUTO].lat = NAN;
    s_cities[WEATHER_CITY_AUTO].lon = NAN;
    s_cities[WEATHER_CITY_AUTO].valid_loc = false;
    memset(&s_cities[WEATHER_CITY_AUTO].snapshot, 0, sizeof(weather_data_t));

    /* Ankara — center coordinates */
    strncpy(s_cities[WEATHER_CITY_ANKARA].name, "Ankara",
            sizeof(s_cities[WEATHER_CITY_ANKARA].name) - 1);
    s_cities[WEATHER_CITY_ANKARA].lat = 39.9334f;
    s_cities[WEATHER_CITY_ANKARA].lon = 32.8597f;
    s_cities[WEATHER_CITY_ANKARA].valid_loc = true;
    memset(&s_cities[WEATHER_CITY_ANKARA].snapshot, 0, sizeof(weather_data_t));
    strncpy(s_cities[WEATHER_CITY_ANKARA].snapshot.city, "Ankara",
            sizeof(s_cities[WEATHER_CITY_ANKARA].snapshot.city) - 1);

    /* Izmir — center coordinates */
    strncpy(s_cities[WEATHER_CITY_IZMIR].name, "Izmir",
            sizeof(s_cities[WEATHER_CITY_IZMIR].name) - 1);
    s_cities[WEATHER_CITY_IZMIR].lat = 38.4192f;
    s_cities[WEATHER_CITY_IZMIR].lon = 27.1287f;
    s_cities[WEATHER_CITY_IZMIR].valid_loc = true;
    memset(&s_cities[WEATHER_CITY_IZMIR].snapshot, 0, sizeof(weather_data_t));
    strncpy(s_cities[WEATHER_CITY_IZMIR].snapshot.city, "Izmir",
            sizeof(s_cities[WEATHER_CITY_IZMIR].snapshot.city) - 1);
}

// ─── Small helpers ───────────────────────────────────────────────────────────

/** Extract a float that follows `key` in @p json. Returns NAN on failure.
 *  '[' de skiplanır → daily array değerleri ("temperature_2m_max":[16.5]) için. */
static float extract_float(const char* json, const char* key)
{
    const char* p = strstr(json, key);
    if (!p) return NAN;
    p += strlen(key);
    while (*p && (*p == ':' || *p == ' ' || *p == '"' ||
                  *p == '\t' || *p == '[')) ++p;
    return strtof(p, NULL);
}

/** Extract an integer that follows `key` in @p json. Returns @p def on failure. */
static int extract_int(const char* json, const char* key, int def)
{
    const char* p = strstr(json, key);
    if (!p) return def;
    p += strlen(key);
    while (*p && (*p == ':' || *p == ' ' || *p == '"' ||
                  *p == '\t' || *p == '[')) ++p;
    return (int)strtol(p, NULL, 10);
}

/** Extract a quoted string that follows `key`. Copies up to outsz-1 bytes. */
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
    while (*p && *p != '"' && i + 1 < outsz) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

// ─── NVS cache ───────────────────────────────────────────────────────────────

static void cache_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    /* Auto şehir lokasyonu (legacy keys) */
    size_t sz = sizeof(s_cities[WEATHER_CITY_AUTO].lat);
    nvs_get_blob(h, NVS_KEY_LAT, &s_cities[WEATHER_CITY_AUTO].lat, &sz);
    sz = sizeof(s_cities[WEATHER_CITY_AUTO].lon);
    nvs_get_blob(h, NVS_KEY_LON, &s_cities[WEATHER_CITY_AUTO].lon, &sz);
    sz = sizeof(s_cities[WEATHER_CITY_AUTO].name);
    if (nvs_get_str(h, NVS_KEY_CITY, s_cities[WEATHER_CITY_AUTO].name, &sz) == ESP_OK) {
        /* Auto için kullanıcıya gösterilen ad da snapshot.city'ye yansımalı */
        strncpy(s_cities[WEATHER_CITY_AUTO].snapshot.city,
                s_cities[WEATHER_CITY_AUTO].name,
                sizeof(s_cities[WEATHER_CITY_AUTO].snapshot.city) - 1);
    }
    if (!isnan(s_cities[WEATHER_CITY_AUTO].lat) &&
        !isnan(s_cities[WEATHER_CITY_AUTO].lon)) {
        s_cities[WEATHER_CITY_AUTO].valid_loc = true;
    }

    /* Per-city snapshot blobları. Auto için ek fallback: legacy "data" key'i. */
    for (int i = 0; i < WEATHER_CITY_COUNT; i++) {
        char keybuf[16];
        snprintf(keybuf, sizeof(keybuf), NVS_KEY_DATA_FMT, i);
        weather_data_t cached = {};
        sz = sizeof(cached);
        esp_err_t err = nvs_get_blob(h, keybuf, &cached, &sz);
        if (err != ESP_OK && i == WEATHER_CITY_AUTO) {
            /* Legacy key fallback (sadece auto için) */
            sz = sizeof(cached);
            err = nvs_get_blob(h, NVS_KEY_DATA_LEGACY, &cached, &sz);
        }
        if (err == ESP_OK && sz == sizeof(cached) && cached.valid) {
            s_cities[i].snapshot = cached;
            ESP_LOGI(TAG, "Cached snapshot[%d] %s loaded: %.1f°C cond=%d updated=%u",
                     i, s_cities[i].snapshot.city, s_cities[i].snapshot.temperature,
                     (int)s_cities[i].snapshot.condition,
                     (unsigned)s_cities[i].snapshot.updated_at_s);
        }
    }

    /* Primary idx — kullanıcının seçtiği */
    int8_t pri = 0;
    sz = sizeof(pri);
    if (nvs_get_i8(h, NVS_KEY_PRIMARY, &pri) == ESP_OK) {
        if (pri >= 0 && pri < WEATHER_CITY_COUNT) s_primary_idx = pri;
    }

    nvs_close(h);

    if (s_cities[WEATHER_CITY_AUTO].valid_loc) {
        ESP_LOGI(TAG, "Auto location: %.3f,%.3f (%s)",
                 s_cities[WEATHER_CITY_AUTO].lat,
                 s_cities[WEATHER_CITY_AUTO].lon,
                 s_cities[WEATHER_CITY_AUTO].name);
    }
    ESP_LOGI(TAG, "Primary city idx: %d (%s)",
             s_primary_idx, s_cities[s_primary_idx].name);
}

static void cache_save_location_auto(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_LAT, &s_cities[WEATHER_CITY_AUTO].lat,
                 sizeof(s_cities[WEATHER_CITY_AUTO].lat));
    nvs_set_blob(h, NVS_KEY_LON, &s_cities[WEATHER_CITY_AUTO].lon,
                 sizeof(s_cities[WEATHER_CITY_AUTO].lon));
    nvs_set_str(h, NVS_KEY_CITY, s_cities[WEATHER_CITY_AUTO].name);
    nvs_commit(h);
    nvs_close(h);
}

/* Bir şehrin tam snapshot'ını NVS'e yaz. Her başarılı fetch sonrası çağrılır.
 * Endurance: 15 dakikada bir × 3 şehir = 12 yazma/watch → günde 288 → yılda
 * ~105K. NVS partition'ında wear-leveling ile 600K toplam → ~5.7 yıl/key,
 * 3 key birlikte ~1.9 yıl key başına. Kabul edilebilir. */
static void cache_save_snapshot(int idx)
{
    if (idx < 0 || idx >= WEATHER_CITY_COUNT) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    char keybuf[16];
    snprintf(keybuf, sizeof(keybuf), NVS_KEY_DATA_FMT, idx);
    nvs_set_blob(h, keybuf, &s_cities[idx].snapshot, sizeof(weather_data_t));
    nvs_commit(h);
    nvs_close(h);
}

static void cache_save_primary(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    int8_t pri = (int8_t)s_primary_idx;
    nvs_set_i8(h, NVS_KEY_PRIMARY, pri);
    nvs_commit(h);
    nvs_close(h);
}

// ─── HTTP GET into caller-provided buffer ────────────────────────────────────

typedef struct {
    char*   buf;
    size_t  cap;
    size_t  len;
} http_ctx_t;

static esp_err_t http_event_cb(esp_http_client_event_t* evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    http_ctx_t* ctx = (http_ctx_t*)evt->user_data;
    if (!ctx || !ctx->buf) return ESP_OK;
    size_t rem = (ctx->cap > ctx->len + 1) ? (ctx->cap - ctx->len - 1) : 0;
    if (rem == 0) return ESP_OK;   // buffer dolu, gerisini drop et
    size_t copy = (size_t)evt->data_len;
    if (copy > rem) copy = rem;
    memcpy(ctx->buf + ctx->len, evt->data, copy);
    ctx->len += copy;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

/** Blocking HTTP GET. @p buf doldurulur, NUL-terminated. */
static esp_err_t http_get(const char* url, char* buf, size_t bufsz,
                          int timeout_ms)
{
    http_ctx_t ctx = { buf, bufsz, 0 };
    buf[0] = '\0';

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.event_handler = http_event_cb;
    cfg.user_data = &ctx;
    cfg.disable_auto_redirect = false;
    cfg.buffer_size = 1024;

    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP fail: %s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP status %d for %s", status, url);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ─── ip-api.com geolocation (yalnız Auto için) ───────────────────────────────

static esp_err_t fetch_location_auto(char* buf, size_t bufsz)
{
    // Ücretsiz, kayıt gerektirmeyen endpoint. JSON kısaltılmış fields ile
    // paketi küçük tutuyoruz.
    const char* url = "http://ip-api.com/json/?fields=status,city,lat,lon";
    esp_err_t err = http_get(url, buf, bufsz, 6000);
    if (err != ESP_OK) return err;

    // status "success" mu?
    if (!strstr(buf, "\"success\"")) {
        ESP_LOGW(TAG, "ip-api status not success");
        return ESP_FAIL;
    }
    float lat = extract_float(buf, "\"lat\"");
    float lon = extract_float(buf, "\"lon\"");
    if (isnan(lat) || isnan(lon)) return ESP_FAIL;

    weather_city_t* a = &s_cities[WEATHER_CITY_AUTO];
    a->lat = lat;
    a->lon = lon;
    a->valid_loc = true;
    extract_string(buf, "\"city\"", a->name, sizeof(a->name));
    /* Snapshot.city'yi de güncelle ki eski snapshot'taki ad değişmiş olsun */
    strncpy(a->snapshot.city, a->name, sizeof(a->snapshot.city) - 1);
    a->snapshot.city[sizeof(a->snapshot.city) - 1] = '\0';
    cache_save_location_auto();
    ESP_LOGI(TAG, "Geolocation: %.3f,%.3f (%s)", a->lat, a->lon, a->name);
    return ESP_OK;
}

// ─── Open-Meteo current weather (per city) ──────────────────────────────────

static esp_err_t fetch_weather_for(int idx, char* buf, size_t bufsz)
{
    if (idx < 0 || idx >= WEATHER_CITY_COUNT) return ESP_FAIL;
    weather_city_t* c = &s_cities[idx];
    if (!c->valid_loc || isnan(c->lat) || isnan(c->lon)) return ESP_FAIL;

    char url[URL_BUF_SZ];
    /* Tek istekle hem `current` (anlık) hem de `daily` (gün hi/lo + UV)
     * çekiyoruz. URL_BUF_SZ = 384 — şu an ~290 byte, sınır içi. */
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&current=weather_code,temperature_2m,relative_humidity_2m,"
             "is_day,wind_speed_10m,wind_direction_10m,wind_gusts_10m,"
             "precipitation"
             "&daily=temperature_2m_max,temperature_2m_min,uv_index_max"
             "&forecast_days=1&timezone=auto",
             c->lat, c->lon);

    esp_err_t err = http_get(url, buf, bufsz, 8000);
    if (err != ESP_OK) return err;

    // DİKKAT: Open-Meteo cevabında "current_units" bloğu "current"'dan önce
    // geliyor ve aynı alan adlarını string değerlerle içeriyor
    // (ör: "weather_code":"wmo code", "is_day":""). strstr ilk eşleşmeyi
    // yakalıyor → yanlış sayısal değerler okunuyordu. Çözüm: parse'ı
    // "current":{ bloğunun başlangıcından itibaren yap.
    const char* scan = strstr(buf, "\"current\":{");
    if (!scan) {
        scan = buf;
        ESP_LOGW(TAG, "\"current\":{ not found — fallback parse");
    }

    int   wmo        = extract_int(scan, "\"weather_code\"", -1);
    float temp       = extract_float(scan, "\"temperature_2m\"");
    int   humidity   = extract_int(scan, "\"relative_humidity_2m\"", -1);
    int   is_day     = extract_int(scan, "\"is_day\"", 1);
    float wind_speed = extract_float(scan, "\"wind_speed_10m\"");      /* km/h */
    int   wind_dir   = extract_int(scan, "\"wind_direction_10m\"", 0); /* deg  */
    float wind_gusts = extract_float(scan, "\"wind_gusts_10m\"");      /* km/h */
    float precip     = extract_float(scan, "\"precipitation\"");       /* mm   */

    /* Daily (hi/lo + UV) — "daily":{ bloğunun başından parse'la */
    const char* dscan = strstr(buf, "\"daily\":{");
    if (!dscan) dscan = buf;
    float t_max = extract_float(dscan, "\"temperature_2m_max\"");
    float t_min = extract_float(dscan, "\"temperature_2m_min\"");
    float uv    = extract_float(dscan, "\"uv_index_max\"");

    if (wmo < 0) {
        ESP_LOGW(TAG, "Open-Meteo parse failed for %s (first 160B): %.160s",
                 c->name, buf);
        return ESP_FAIL;
    }

    bool night = (is_day == 0);
    weather_condition_t cond = weather_icon_from_wmo(wmo, night);

    /* Apple/IBM "fırtına" ikonunu taklit et — eski mantık aynen korundu. */
    bool has_precip = !isnan(precip) && precip >= 0.3f;
    bool has_storm  = (!isnan(wind_gusts) && wind_gusts >= 40.0f) ||
                      (!isnan(precip) && precip >= 1.0f &&
                       !isnan(wind_gusts) && wind_gusts >= 25.0f);

    if (has_storm && cond != WEATHER_SNOW) {
        cond = WEATHER_THUNDER;
    } else if (has_precip &&
               (cond == WEATHER_CLEAR || cond == WEATHER_NIGHT_CLEAR ||
                cond == WEATHER_PARTLY_CLOUDY ||
                cond == WEATHER_NIGHT_CLOUDY ||
                cond == WEATHER_CLOUDY || cond == WEATHER_FOG)) {
        cond = WEATHER_RAIN;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    weather_data_t* d = &c->snapshot;
    d->condition    = cond;
    d->wmo_code     = wmo;
    d->temperature  = isnan(temp)   ? 0.0f : temp;
    d->temp_max     = isnan(t_max)  ? 0.0f : t_max;
    d->temp_min     = isnan(t_min)  ? 0.0f : t_min;
    d->humidity     = (humidity < 0 || humidity > 100) ? 0 : humidity;
    d->wind_speed   = isnan(wind_speed) ? 0.0f : wind_speed;
    d->wind_dir     = ((wind_dir % 360) + 360) % 360;
    d->uv_index     = isnan(uv)     ? 0.0f : uv;
    d->is_night     = night;
    d->valid        = true;
    d->updated_at_s = (uint32_t)time(NULL);
    strncpy(d->city, c->name, sizeof(d->city) - 1);
    d->city[sizeof(d->city) - 1] = '\0';
    xSemaphoreGive(s_mutex);

    static const char* const COND_NAME[] = {
        "UNKNOWN", "CLEAR", "PARTLY_CLOUDY", "CLOUDY", "FOG",
        "RAIN", "SNOW", "THUNDER", "NIGHT_CLEAR", "NIGHT_CLOUDY"
    };
    const char* cname = ((int)cond < (int)(sizeof(COND_NAME)/sizeof(COND_NAME[0])))
                            ? COND_NAME[(int)cond] : "?";
    ESP_LOGI(TAG, "Weather %s: wmo=%d temp=%.1f°C → %s",
             c->name, wmo, temp, cname);

    cache_save_snapshot(idx);

    /* Her şehir fetch'inde EVENT_WEATHER_UPDATE publish — overlay açıkken
     * card'lar reaktif güncellensin. Payload: low byte = condition, high byte =
     * city idx. Eski dinleyiciler (update_ui_from_snapshot) low byte'a bakıp
     * primary olmayan event'leri de dinleseler bile sadece primary verisini
     * yeniden çiziyorlar (idempotent), problem değil. */
    uint32_t payload = ((uint32_t)idx << 16) | (uint32_t)cond;
    event_bus_publish(EVENT_WEATHER_UPDATE, payload);
    return ESP_OK;
}

// ─── Main task ───────────────────────────────────────────────────────────────

static void weather_task(void* /*arg*/)
{
    ESP_LOGI(TAG, "Weather task started");

    cache_load();
    /* Primary'nin cached snapshot'ı varsa UI'ye haber ver — boot'ta widget
     * "Loading"de takılmasın. */
    if (s_cities[s_primary_idx].snapshot.valid) {
        event_bus_publish(EVENT_WEATHER_UPDATE,
                          (uint32_t)s_cities[s_primary_idx].snapshot.condition);
    }

    char* buf = (char*)malloc(HTTP_BUF_SZ);
    if (!buf) {
        ESP_LOGE(TAG, "Out of memory for HTTP buffer");
        vTaskDelete(NULL);
        return;
    }

    while (!wifi_service_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (true) {
        bool any_failed = false;
        bool all_have_data = true;

        if (wifi_service_is_connected()) {
            /* Auto'nun lokasyonu yoksa önce ip-api */
            if (!s_cities[WEATHER_CITY_AUTO].valid_loc) {
                if (fetch_location_auto(buf, HTTP_BUF_SZ) != ESP_OK) {
                    ESP_LOGW(TAG, "Geolocation failed — retry 30s");
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
                    continue;
                }
            }
            /* Tüm şehirleri sırayla çek. Primary önce — kullanıcı en hızlı
             * şekilde aktif şehrin tazesini görsün. */
            int order[WEATHER_CITY_COUNT];
            order[0] = s_primary_idx;
            int k = 1;
            for (int i = 0; i < WEATHER_CITY_COUNT; i++) {
                if (i == s_primary_idx) continue;
                order[k++] = i;
            }
            for (int j = 0; j < WEATHER_CITY_COUNT; j++) {
                int idx = order[j];
                esp_err_t e = fetch_weather_for(idx, buf, HTTP_BUF_SZ);
                if (e != ESP_OK) {
                    ESP_LOGW(TAG, "Fetch fail for %s — will retry",
                             s_cities[idx].name);
                    any_failed = true;
                }
                /* Open-Meteo free tier: nazik davran, kısa bir buffer */
                vTaskDelay(pdMS_TO_TICKS(200));
            }

            for (int i = 0; i < WEATHER_CITY_COUNT; i++) {
                if (!s_cities[i].snapshot.valid) all_have_data = false;
            }
        } else {
            ESP_LOGD(TAG, "Wi-Fi down, waiting...");
            all_have_data = false;
        }

        /* Adaptive sleep:
         *   - Tüm şehirler valid: 15 dakika uyu (sebepsiz fetch yapma).
         *   - Bazı şehirler fail veya hiç fetch edilmemiş: 30 saniye uyu, retry.
         * refresh_now / set_primary_idx istediği an erken uyandırır. */
        uint32_t sleep_ms = (all_have_data && !any_failed)
                                ? (WEATHER_REFRESH_S * 1000)
                                : 30000;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(sleep_ms));
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

void weather_service_init(void)
{
    if (s_initialized) return;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Mutex create failed");
        return;
    }

    cities_init_defaults();

    BaseType_t ok = xTaskCreatePinnedToCore(
        weather_task,
        "weather",
        6144,                // 6KB stack (HTTP+TLS dışı ama malloc ile güvenli)
        nullptr,
        2,                   // UI/sensor/power task'larından düşük
        &s_task_handle,
        1                    // Core 1
    );

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Task create failed");
        return;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "Weather service initialized");
}

bool weather_service_get(weather_data_t* out)
{
    if (!out || !s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cities[s_primary_idx].snapshot;
    bool valid = out->valid;
    xSemaphoreGive(s_mutex);
    return valid;
}

bool weather_service_get_city(int idx, weather_data_t* out)
{
    if (!out || !s_mutex) return false;
    if (idx < 0 || idx >= WEATHER_CITY_COUNT) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cities[idx].snapshot;
    bool valid = out->valid;
    xSemaphoreGive(s_mutex);
    return valid;
}

const char* weather_service_get_city_name(int idx)
{
    if (idx < 0 || idx >= WEATHER_CITY_COUNT) return "";
    return s_cities[idx].name;
}

int weather_service_get_primary_idx(void)
{
    return s_primary_idx;
}

void weather_service_set_primary_idx(int idx)
{
    if (idx < 0 || idx >= WEATHER_CITY_COUNT) return;
    if (idx == s_primary_idx) return;
    s_primary_idx = idx;
    cache_save_primary();
    ESP_LOGI(TAG, "Primary city -> %d (%s)", idx, s_cities[idx].name);
    /* UI'ye anında yeni şehrin snapshot'ı yansısın — cached varsa */
    if (s_cities[idx].snapshot.valid) {
        event_bus_publish(EVENT_WEATHER_UPDATE,
                          (uint32_t)s_cities[idx].snapshot.condition);
    }
    /* Fetch task'i uyandır → seçilen şehir hemen tazelensin */
    if (s_task_handle) xTaskNotifyGive(s_task_handle);
}

void weather_service_refresh_now(void)
{
    if (s_task_handle) xTaskNotifyGive(s_task_handle);
}

// ─── WMO → kısa Türkçe açıklama (ASCII) ──────────────────────────────────────
const char* weather_desc_from_wmo(int wmo, bool is_night)
{
    switch (wmo) {
        case 0:  return is_night ? "Acik (Gece)"      : "Acik";
        case 1:  return is_night ? "Az Bulutlu"        : "Az Bulutlu";
        case 2:  return "Parcali Bulutlu";
        case 3:  return "Cogunlukla Bulutlu";
        case 45: return "Sisli";
        case 48: return "Buz Sisi";
        case 51: return "Hafif Cisenti";
        case 53: return "Cisenti";
        case 55: return "Yogun Cisenti";
        case 56: return "Donan Cisenti";
        case 57: return "Donan Cisenti";
        case 61: return "Hafif Yagmur";
        case 63: return "Yagmurlu";
        case 65: return "Siddetli Yagmur";
        case 66: return "Donan Yagmur";
        case 67: return "Donan Yagmur";
        case 71: return "Hafif Kar";
        case 73: return "Karli";
        case 75: return "Yogun Kar";
        case 77: return "Kar Taneleri";
        case 80: return "Saganak Yagis";
        case 81: return "Saganak Yagis";
        case 82: return "Siddetli Saganak";
        case 85: return "Hafif Kar Saganagi";
        case 86: return "Siddetli Kar Saganagi";
        case 95: return "Gok Gurultulu Firtina";
        case 96: return "Firtina ve Dolu";
        case 99: return "Siddetli Firtina";
        default: return "Hava Durumu";
    }
}
