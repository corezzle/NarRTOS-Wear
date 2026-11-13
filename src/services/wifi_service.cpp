#include "wifi_service.h"

#include "../core/os_config.h"
#include "wifi_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs.h>

#include <string.h>
#include <atomic>             /* v1.4.55 audit #2b: scan generation guard */

/* NAR-ESP32S3 (Session 46j): Wi-Fi TX gücü sınırı.
 * Ölçüm: 14 dBm'e kadar sorunsuz; 17 dBm ve üstünde gönderim anındaki akım
 * sıçraması kartın beslemesini çökertiyor (PMIC dahil tam reset, iki denemede
 * aynı eşik). Donanım güç yolu düzeltilene kadar 13 dBm ile sınırla.
 * esp_wifi_start()'tan SONRA çağrılmalı (öncesinde ESP_ERR_WIFI_NOT_STARTED). */
static inline void wifi_apply_board_tx_limit(void)
{
#ifdef BOARD_VARIANT_NAR
    esp_err_t e = esp_wifi_set_max_tx_power(52);   /* birim 0.25 dBm → 13 dBm */
    if (e != ESP_OK) ESP_LOGW("WiFiSvc", "TX guc siniri ayarlanamadi: %s", esp_err_to_name(e));
#endif
}

static const char* TAG = "WiFiSvc";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     5

#define WIFI_NVS_NS        "wifi_svc"
#define WIFI_NVS_KEY_PREF  "user_pref"   /* uint8_t: 1=enabled, 0=disabled */

static EventGroupHandle_t s_wifi_event_group = NULL;
static bool s_initialized = false;
static int s_retry_count = 0;
static volatile bool s_scan_only = false;

static esp_event_handler_instance_t s_instance_any = NULL;
static esp_event_handler_instance_t s_instance_ip  = NULL;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_scan_only) esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Sebep kodu teşhis için: 15/204 = 4-yollu el sıkışma zaman aşımı (şifre uyuşmazlığı ya da
         * AP'nin reddi), 2 = auth expire, 201 = ağ bulunamadı, 205 = bağlantı kesildi, 8 = AP kapattı */
        const wifi_event_sta_disconnected_t* d = (const wifi_event_sta_disconnected_t*)event_data;
        const int reason = d ? d->reason : -1;
        const char* why = reason == 15 || reason == 204 ? "el sikisma zaman asimi (sifre?)" :
                          reason == 201 ? "ag bulunamadi" : reason == 2 ? "auth expire" :
                          reason == 205 ? "baglanti kesildi" : reason == 8 ? "AP ayrildi" : "";
        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected (reason %d %s), retry %d/%d", reason, why, s_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "WiFi connect failed");
            if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        if (s_wifi_event_group) xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "WiFi connected (got IP)");
    }
}

void wifi_service_init(void)
{
    if (s_initialized) return;

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(ret));
    }

    esp_netif_create_default_wifi_sta();

    /* v1.4.55 (2026-05-19) — audit orta öncelik #4: ESP_ERROR_CHECK → soft fail.
     * esp_wifi_init / set_storage / set_mode / event_handler_register hepsi
     * recoverable error döndürebilir (NVS bozuk, partition fail, vb.). abort()
     * boot crash yaratırdı; WiFi olmadan da saat boot etmeli (RTC, watchface,
     * alarm vb. WiFi'sız çalışır). Fail durumunda s_initialized=false bırakılır
     * → sonraki wifi_service_connect/scan çağrıları no-op (zaten init'i kontrol
     * ediyorlar). */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s — WiFi devre dışı kalıyor, saat WiFi'sız boot ediyor",
                 esp_err_to_name(ret));
        return;   /* s_initialized = false; sessiz devam */
    }
    // Creds'i NVS'e kalıcı yaz — kullanıcı Settings'ten bir kere bağlandıktan
    // sonra sonraki boot'larda aynı AP'ye otomatik bağlansın.
    ret = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_storage failed: %s (continuing)", esp_err_to_name(ret));
    }
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s — WiFi devre dışı", esp_err_to_name(ret));
        esp_wifi_deinit();
        return;
    }

    s_wifi_event_group = xEventGroupCreate();

    ret = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_instance_any);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WIFI_EVENT handler register failed: %s — WiFi devre dışı", esp_err_to_name(ret));
        esp_wifi_deinit();
        return;
    }
    ret = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_instance_ip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "IP_EVENT handler register failed: %s — WiFi devre dışı", esp_err_to_name(ret));
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_instance_any);
        esp_wifi_deinit();
        return;
    }

    s_initialized = true;
}

esp_err_t wifi_service_connect(uint32_t timeout_ms)
{
    wifi_service_init();

    // ── Bağlantı kaynağı seçimi ─────────────────────────────────────────
    // 1) wifi_config.h'de hardcoded creds varsa onu kullan (dev kolaylığı).
    // 2) Yoksa NVS'te kayıtlı creds'i ara (kullanıcı Settings'ten bağlandıysa).
    // 3) Hiçbiri yoksa early return — Settings ekranı kullanıcıdan ister.
    bool have_creds = false;

    if (strlen(WIFI_SSID) > 0) {
        wifi_config_t wifi_config = {};
        strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
        wifi_config.sta.pmf_cfg.capable  = true;
        wifi_config.sta.pmf_cfg.required = false;
        wifi_config.sta.threshold.authmode =
            (strlen(WIFI_PASS) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_LOGI(TAG, "Using hardcoded credentials for SSID '%s'", WIFI_SSID);
        have_creds = true;
    } else {
        // NVS'teki kayıtlı config'i oku. esp_wifi_get_config önce WiFi'nin
        // en azından init edilmiş olmasını ister; init zaten yukarıda yapıldı.
        wifi_config_t stored = {};
        esp_err_t ret = esp_wifi_get_config(WIFI_IF_STA, &stored);
        if (ret == ESP_OK && stored.sta.ssid[0] != 0) {
            ESP_LOGI(TAG, "Auto-connecting to saved SSID '%s'",
                     (const char*)stored.sta.ssid);
            have_creds = true;
        } else {
            ESP_LOGW(TAG, "No saved WiFi credentials — user must connect via Settings");
            return ESP_ERR_INVALID_ARG;
        }
    }
    (void)have_creds;

    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    wifi_apply_board_tx_limit();

    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        return ESP_FAIL;
    }

    return ESP_ERR_TIMEOUT;
}

void wifi_service_disconnect(void)
{
    if (!s_initialized) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
}

bool wifi_service_is_connected(void)
{
    if (!s_wifi_event_group) return false;
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ── Async scan ─────────────────────────────────────────────────────────── */

static volatile wifi_scan_cb_t s_scan_cb     = NULL;
static volatile void*          s_scan_ud     = NULL;
static volatile bool           s_scan_cancel = false;
static TaskHandle_t            s_scan_task   = NULL;
/* v1.4.55 (2026-05-19) — audit orta öncelik #2b: scan generation guard.
 * Settings WiFi sayfasından hızlı çıkış sırasında race window: scan_task
 * cb'yi local'e kopyaladıktan sonra cancel gelirse callback yine çağrılır.
 * Mevcut callback chain (lv_async_call → wifi_list_update → s_wifi_list NULL
 * check) defansif AMA bu defansiflik settings tarafında. Wifi_service kendi
 * içinde de defense-in-depth garanti edebilsin: her scan_async/cancel'de
 * generation artırılır, callback'i çağırmadan önce karşılaştırılır → eski
 * scan'in callback'i sessizce drop edilir. */
static std::atomic<uint32_t>   s_scan_generation{0};

static void wifi_scan_task(void* arg)
{
    /* arg = scan_async sırasında yakalanan generation snapshot */
    uint32_t my_gen = (uint32_t)(uintptr_t)arg;

    wifi_service_init();
    s_scan_only = true;
    esp_wifi_start();   /* no-op if already started */
    wifi_apply_board_tx_limit();

    wifi_scan_config_t cfg = {};
    cfg.scan_type              = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min   = 50;
    cfg.scan_time.active.max   = 300;

    if (!s_scan_cancel) {
        esp_err_t err = esp_wifi_scan_start(&cfg, true /* blocking */);

        /* v1.4.55 audit #2b: generation kontrolü cancel kontrolü ile beraber.
         * Generation mismatch = scan_cancel veya yeni scan_async geldi →
         * eski callback artık geçersiz, drop et. */
        bool gen_valid = (s_scan_generation.load(std::memory_order_acquire) == my_gen);

        if (err == ESP_OK && !s_scan_cancel && gen_valid) {
            uint16_t       n       = WIFI_SCAN_MAX_APS;
            wifi_ap_record_t records[WIFI_SCAN_MAX_APS];
            esp_wifi_scan_get_ap_records(&n, records);

            /* determine currently connected AP (best-effort) */
            wifi_ap_record_t conn = {};
            bool is_conn = (esp_wifi_sta_get_ap_info(&conn) == ESP_OK);

            wifi_ap_t aps[WIFI_SCAN_MAX_APS];
            for (uint16_t i = 0; i < n; i++) {
                strlcpy(aps[i].ssid, (const char*)records[i].ssid,
                        sizeof(aps[i].ssid));
                aps[i].rssi      = records[i].rssi;
                aps[i].secured   = (records[i].authmode != WIFI_AUTH_OPEN);
                aps[i].connected = is_conn &&
                    (strcmp(aps[i].ssid, (const char*)conn.ssid) == 0);
            }

            /* Son bir generation kontrolü — uzun esp_wifi_scan_get_ap_records
             * sırasında cancel gelmiş olabilir. */
            if (s_scan_generation.load(std::memory_order_acquire) == my_gen) {
                wifi_scan_cb_t cb = s_scan_cb;
                void*          ud = (void*)s_scan_ud;
                s_scan_cb = NULL;
                s_scan_ud = NULL;
                if (cb) cb(aps, (uint8_t)n, ud);
            } else {
                ESP_LOGD(TAG, "scan: generation mismatch on final check, dropping callback");
            }
        } else if (!gen_valid) {
            ESP_LOGD(TAG, "scan: generation mismatch (cancelled/superseded), dropping");
        }
    }

    s_scan_only = false;

    s_scan_cancel = false;
    s_scan_cb     = NULL;
    s_scan_ud     = NULL;
    s_scan_task   = NULL;
    vTaskDelete(NULL);
}

void wifi_service_scan_async(wifi_scan_cb_t cb, void* user_data)
{
    /* cancel any in-flight scan before starting a new one */
    if (s_scan_task != NULL)
    {
        ESP_LOGW(TAG, "Scan already in progress, ignoring new request");
        return;
    }

    s_scan_cancel = false;
    s_scan_cb = cb;
    s_scan_ud = user_data;
    /* v1.4.55 audit #2b: yeni generation snapshot — task buna göre filtrelenir */
    uint32_t my_gen = s_scan_generation.fetch_add(1, std::memory_order_acq_rel) + 1;

    // Görev oluşturulurken Stack boyutunu kontrol edelim (4096 yeterli)
    BaseType_t ret = xTaskCreate(wifi_scan_task, "wifi_scan", 4096,
                                  (void*)(uintptr_t)my_gen, 3, &s_scan_task);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create scan task");
        s_scan_task = NULL;
    }
}

void wifi_service_scan_cancel(void)
{
    /* v1.4.55 audit #2b: generation'ı artır → scan_task'ın final
     * callback kontrolünde mismatch → callback drop edilir. */
    s_scan_generation.fetch_add(1, std::memory_order_acq_rel);
    s_scan_cancel = true;
    s_scan_cb     = NULL;
    s_scan_ud     = NULL;
    s_scan_only   = false;
}

/* ── Synchronous BSSID scan (WiFi geolocation, telemetry) ───────────────────
 * Faz 2: GPS'siz konumlandırma girdisi. Bloklayıcı; kendi task'ından çağrılır.
 * Async (UI) scan çalışıyorsa çakışmayı önlemek için bu turu atlar. */
esp_err_t wifi_service_scan_bssids(wifi_bssid_ap_t* out, uint8_t max, uint8_t* out_count)
{
    if (out_count) *out_count = 0;
    if (!out || max == 0) return ESP_ERR_INVALID_ARG;

    wifi_service_init();
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    /* Async (UI) scan çalışıyorsa aynı anda ikinci esp_wifi_scan_start açma. */
    if (s_scan_task != NULL) return ESP_ERR_INVALID_STATE;

    s_scan_only = true;               /* tarama sırasında auto-reconnect churn'ü bastır */
    esp_wifi_start();                 /* no-op if already started */
    wifi_apply_board_tx_limit();

    /* BT+WiFi coexistence: aktif tarama süresi override edilirse sürücü hata
     * verir ("use default active scan time parameter … when Bluetooth is
     * enabled"). scan_time'ı 0 bırak → sürücü coex-güvenli varsayılanı seçer. */
    wifi_scan_config_t cfg = {};
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    esp_err_t err = esp_wifi_scan_start(&cfg, true /* blocking */);
    if (err != ESP_OK) {
        s_scan_only = false;
        return err;
    }

    uint16_t n = (max > WIFI_SCAN_MAX_APS) ? WIFI_SCAN_MAX_APS : max;
    wifi_ap_record_t records[WIFI_SCAN_MAX_APS];
    err = esp_wifi_scan_get_ap_records(&n, records);
    s_scan_only = false;              /* connected'sak bağlantı tarama sonrası korunur */
    if (err != ESP_OK) return err;

    uint8_t count = 0;
    for (uint16_t i = 0; i < n && count < max; i++) {
        memcpy(out[count].bssid, records[i].bssid, 6);
        out[count].rssi    = records[i].rssi;
        out[count].channel = records[i].primary;
        count++;
    }
    if (out_count) *out_count = count;
    return ESP_OK;
}

/* ── Connect to a specific AP ───────────────────────────────────────────── */

typedef struct {
    char                     ssid[33];
    char                     password[65];
    wifi_connect_result_cb_t cb;
    void*                    user_data;
} wifi_connect_args_t;

static void wifi_connect_ap_task(void* arg)
{
    wifi_connect_args_t* a = (wifi_connect_args_t*)arg;

    wifi_service_init();

    /* Hard-stop any current connection first */
    esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(300));

    /* Build config */
    wifi_config_t cfg = {};
    strlcpy((char*)cfg.sta.ssid,     a->ssid,     sizeof(cfg.sta.ssid));
    strlcpy((char*)cfg.sta.password, a->password, sizeof(cfg.sta.password));
    cfg.sta.pmf_cfg.capable  = true;
    cfg.sta.pmf_cfg.required = false;
    cfg.sta.threshold.authmode =
        (strlen(a->password) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    /* Reset retry state + event bits */
    s_retry_count = 0;
    if (s_wifi_event_group)
        xEventGroupClearBits(s_wifi_event_group,
                             WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    esp_err_t ret = esp_wifi_start();
    wifi_apply_board_tx_limit();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
        ESP_LOGE("WiFiSvc", "start failed: %s", esp_err_to_name(ret));
        if (a->cb) a->cb(false, a->user_data);
        free(a);
        vTaskDelete(NULL);
        return;
    }

    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    bool ok = (bits & WIFI_CONNECTED_BIT) != 0;
    ESP_LOGI("WiFiSvc", "connect_to_ap '%s' → %s", a->ssid, ok ? "OK" : "FAIL");

    if (a->cb) a->cb(ok, a->user_data);
    free(a);
    vTaskDelete(NULL);
}

void wifi_service_connect_to_ap(const char* ssid, const char* password,
                                 wifi_connect_result_cb_t cb, void* user_data)
{
    wifi_connect_args_t* a =
        (wifi_connect_args_t*)malloc(sizeof(wifi_connect_args_t));
    if (!a) { if (cb) cb(false, user_data); return; }

    strlcpy(a->ssid,     ssid,              sizeof(a->ssid));
    strlcpy(a->password, password ? password : "", sizeof(a->password));
    a->cb        = cb;
    a->user_data = user_data;

    // Spawn the connect task — if creation fails (e.g. heap exhausted) we must
    // free the args ourselves and notify the caller, otherwise the UI hangs
    // forever waiting on a callback that will never come.
    BaseType_t r = xTaskCreate(wifi_connect_ap_task, "wifi_conn_ap",
                               4096, a, 5, NULL);
    if (r != pdPASS) {
        ESP_LOGE("WiFiSvc", "xTaskCreate(wifi_conn_ap) failed");
        if (cb) cb(false, user_data);
        free(a);
    }
}

/* ─── User preference (NVS-persisted) ────────────────────────────────────── */

bool wifi_service_get_user_preference(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        /* Key yoksa default ON (ilk boot kullanıcı WiFi'nin çalışmasını bekler) */
        return true;
    }
    uint8_t v = 1;
    if (nvs_get_u8(h, WIFI_NVS_KEY_PREF, &v) != ESP_OK) v = 1;
    nvs_close(h);
    return v != 0;
}

void wifi_service_set_user_preference(bool enabled)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, WIFI_NVS_KEY_PREF, enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "User preference saved: WiFi=%s", enabled ? "ON" : "OFF");
}
