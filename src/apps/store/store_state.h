#pragma once
/**
 * @file store_state.h
 * @brief Store catalog + install state — v1.2.6 (2026-05-21).
 *
 * Catalog: 2 kategori app
 *   1. System (is_system=true)        — silinemez, saatte built-in
 *      Alarm, Weather, Gallery, Notifications (4 app)
 *   2. Downloadable (is_system=false) — "mağazadan indirilir gibi" gösterilen
 *      app'ler. Binary saatte zaten compile edilmiş ama kullanıcı için NVS
 *      flag ile install/uninstall yönetilir. Backend gelmeden Apple-style
 *      UX denemek için mock pattern.
 *      Şu an: Step Counter (1 app)
 *
 * NVS state: sadece downloadable app'ler için anlamlı.
 *   namespace: "store"
 *   key: "inst_<app_id>" → uint8_t (0/1)
 *
 * System app'ler her zaman "installed" döner.
 */

#include "../../core/app_manager.h"
#include "../../core/os_config.h"
#include <lvgl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    app_id_t      id;
    const char*   name;
    const char*   subtitle;
    const char*   description;
    const char*   icon_symbol;
    uint8_t       cr, cg, cb;
    bool          is_system;    /* true = silinemez built-in, false = downloadable */
} store_app_entry_t;

/* Catalog accessors — store_app.cpp ve launcher_app.cpp tarafından kullanılır. */
size_t                    store_catalog_count(void);
const store_app_entry_t*  store_catalog_at(size_t idx);

/* Install state — NVS-backed.
 *   System app'ler için her zaman true döner.
 *   Downloadable app'ler için NVS'ten okur. */
bool  store_is_installed(app_id_t id);
void  store_set_installed(app_id_t id, bool installed);

/* v1.2.7 (2026-05-22): Backend integration helpers — store_service ve
 * store_app tarafından paylaşılır.
 *
 * Backend `app_id` string'lerini (örn "step_counter") yayınlar. Saatte
 * bu string'i `APP_ID_*` enum'una map etmek gerek (launch için). Eğer
 * mapping yoksa saatte o binary yok demektir → "Requires firmware update". */

/* app_id string → APP_ID_* enum. Bilinmeyen string için APP_ID_COUNT döner
 * ("saatte binary yok" işareti). */
app_id_t  store_appid_from_string(const char* app_id_str);

/* LV_SYMBOL string isimlerini gerçek LVGL glyph string'ine map et.
 * Bilinmeyen sembol için "?" döner (fallback glyph). Backend "LV_SYMBOL_LIST"
 * gibi string yayınlar; LVGL ise gerçek bir glyph macro tanımlar (UTF-8). */
const char* store_icon_from_string(const char* icon_symbol_str);

/* Hex string ("#RRGGBB") → 3 byte RGB. Başarılıysa true. */
bool  store_color_from_hex(const char* hex, uint8_t* r, uint8_t* g, uint8_t* b);

#ifdef __cplusplus
}
#endif
