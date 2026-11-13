#pragma once
/**
 * @file store_service.h
 * @brief Backend catalog fetcher — v1.2.7 (2026-05-22).
 *
 * `corezzle-imager-backend` wear-os modülüne HTTP GET ile bağlanır,
 * yayınlanmış app metadata'sını çeker, in-memory cache'te tutar.
 * Saat tarafında compile-time `store_state` catalog'undan ayrı bir
 * "remote catalog" listesi yönetilir.
 *
 * Backend endpoint:
 *   GET http://<host>/api/wear-os/store/apps
 *   Response: { "apps": [...], "server_time": "..." }
 *
 * Saat tarafında her remote entry için:
 *   - `app_id` string → `APP_ID_*` enum (store_appid_from_string)
 *   - Eğer mapping yoksa: saatte binary yok → UI "Requires firmware update"
 *   - Eğer `min_firmware_version_code` > OS_VERSION_CODE: aynı şekilde
 *     "Requires firmware update" (saatte daha eski firmware var)
 *
 * Cache stratejisi:
 *   - İlk açılışta: WiFi varsa fetch, sonuçlar in-memory
 *   - Refresh: ext_refresh çağrısı (örn Store ekranı açıldığında)
 *   - WiFi yoksa: önceki cache (varsa) kullanılır; yoksa compile-time fallback
 *
 * Thread-safety: WiFi worker task'tan refresh + UI task'tan read.
 * Read sırasında list invalidate olmasın diye `store_service_remote_count` ve
 * `store_service_remote_at` snapshot pointer döndürür — read-only kullanım.
 */

#include "store_state.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Backend'den çekilen tek bir app entry'si — runtime memory cache. */
typedef struct {
    char     app_id[64];               /* backend string: "step_counter", vb. */
    char     name[64];
    char     subtitle[128];
    char     description[512];
    char     icon_symbol[32];          /* "LV_SYMBOL_LIST" gibi backend string */
    uint8_t  cr, cg, cb;               /* color_hex parse edilmiş RGB */
    int      version;
    int      min_firmware_version_code;
    int      display_order;

    /* Saat tarafında runtime'da hesaplanır (mapping sonucu) */
    app_id_t mapped_id;                /* APP_ID_COUNT = saatte binary yok */
    bool     binary_available;         /* mapping + firmware version uygun mu */
} store_remote_app_t;

/* Refresh callback — async fetch tamamlanınca tetiklenir.
 * err == ESP_OK: success, count = remote app sayısı
 * err != ESP_OK: fail, cache değişmedi (varsa eski cache geçerli) */
typedef void (*store_service_refresh_cb_t)(esp_err_t err, int count, void* user_data);

/* Module init — boot'ta veya store_app on_start'ta çağrılır. */
void store_service_init(void);

/* Asenkron catalog refresh.
 *   - Yeni task spawn eder (HTTP fetch)
 *   - WiFi check yapar; yoksa hemen ESP_ERR_WIFI_NOT_CONNECT döner
 *   - cb optional; null geçilebilir */
esp_err_t store_service_refresh_async(store_service_refresh_cb_t cb, void* user_data);

/* Cache okuma — UI tarafından kullanılır */
size_t                     store_service_remote_count(void);
const store_remote_app_t*  store_service_remote_at(size_t idx);

/* Cache "fresh" mi? Son başarılı refresh'ten bu yana geçen süre belli
 * eşiğin altındaysa true. Boot sonrası ilk gösterimde false. */
bool  store_service_has_cache(void);

#ifdef __cplusplus
}
#endif
