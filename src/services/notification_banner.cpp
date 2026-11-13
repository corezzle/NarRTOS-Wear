/**
 * @file notification_banner.cpp
 * @brief Apple Watch-style notification banner overlay on lv_layer_top().
 *
 * Mimari:
 *   - notification_store'a listener register
 *   - NOTIF_EVENT_ADDED → lv_async_call ile UI thread'e geç
 *   - Banner object'i lv_layer_top()'a yaratılır → her app üstünde görünür
 *   - Slide-in animasyon (200ms ease-out)
 *   - 5sn sonra otomatik dismiss (lv_timer)
 *   - Tap → app_manager_launch(APP_ID_NOTIFY) + dismiss
 *
 * Thread safety: store listener BLE/ANCS task'tan çağrılabilir; lv_async_call
 * her zaman kullanılır. UI ops mutex'siz, LVGL tek thread (UI task) içinde.
 *
 * Banner sadece yeni gelen (ADDED) bildirimlerde gösterilir. MODIFIED ve
 * REMOVED için banner çıkarılmaz (zaten Notifications app'inde güncellenir).
 */

#include "notification_banner.h"
#include "notification_store.h"
#include "power_service.h"
#include "../core/app_manager.h"
#include "../core/os_config.h"
#include <lvgl.h>
#include <esp_log.h>
#include <string.h>

/* v1.4.71: Türkçe karakter destekli özel Montserrat font'ları */
LV_FONT_DECLARE(notif_font_16);
LV_FONT_DECLARE(notif_font_20);

static const char* TAG = "NotifBnr";

#define BANNER_HEIGHT      130
#define BANNER_AUTO_MS    5000
#define BANNER_ANIM_MS     220

static lv_obj_t*   s_banner       = NULL;
static lv_timer_t* s_dismiss_tmr  = NULL;
static uint32_t    s_current_uid  = 0;

/* ─── App icon resolution (Notifications app'tekiyle paralel) ─────────────── */

typedef struct {
    const char* bundle_substr;
    const char* icon;
    uint8_t     cr, cg, cb;
} app_vis_t;

/* LVGL built-in symbol seti sınırlı (WhatsApp logosu yok); en yakın
 * semantik eşleşme + renk kombinasyonuyla ayırt edilebilir kıl. v1.4.72:
 * iOS bundle ID'leri tam string match yerine substring match — "net.whatsapp"
 * yakalanır. Calendar = "mobilecal" (com.apple.mobilecal). */
static const app_vis_t s_visuals[] = {
    { "whatsapp",        LV_SYMBOL_ENVELOPE,0x25, 0xD3, 0x66 },  /* yeşil chat */
    { "mobilesms",       LV_SYMBOL_ENVELOPE,0x0A, 0x84, 0xFF },  /* iMessage mavi */
    { "messages",        LV_SYMBOL_ENVELOPE,0x0A, 0x84, 0xFF },
    { "mobilemail",      LV_SYMBOL_ENVELOPE,0x4F, 0xC3, 0xF7 },
    { "telegram",        LV_SYMBOL_ENVELOPE,0x2A, 0xAB, 0xEE },
    { "instagram",       LV_SYMBOL_IMAGE,   0xE4, 0x40, 0x5F },
    { "facebook",        LV_SYMBOL_HOME,    0x4E, 0x6C, 0xC4 },
    { "mobilephone",     LV_SYMBOL_CALL,    0x4C, 0xD9, 0x64 },  /* telefon */
    { "mobilecal",       LV_SYMBOL_BELL,    0xFF, 0x3B, 0x30 },  /* takvim — kırmızı */
    { "calendar",        LV_SYMBOL_BELL,    0xFF, 0x3B, 0x30 },
    { "reminders",       LV_SYMBOL_LIST,    0xFF, 0x9F, 0x0A },
};

static bool ci_contains(const char* h, const char* n)
{
    if (!h || !n) return false;
    size_t hl = strlen(h), nl = strlen(n);
    if (!nl || nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; i++) {
        bool m = true;
        for (size_t j = 0; j < nl; j++) {
            char a = h[i+j], b = n[j];
            if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
            if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
            if (a != b) { m = false; break; }
        }
        if (m) return true;
    }
    return false;
}

static void resolve_visual(const notification_entry_t* e,
                            const char** icon, uint8_t* r, uint8_t* g, uint8_t* b)
{
    for (size_t i = 0; i < sizeof(s_visuals)/sizeof(s_visuals[0]); i++) {
        if (ci_contains(e->app_id, s_visuals[i].bundle_substr)) {
            *icon = s_visuals[i].icon;
            *r = s_visuals[i].cr; *g = s_visuals[i].cg; *b = s_visuals[i].cb;
            return;
        }
    }
    /* Kategori fallback */
    switch (e->category) {
    case NOTIF_CAT_SOCIAL:        *icon = LV_SYMBOL_CALL;     *r=0x25;*g=0xD3;*b=0x66; break;
    case NOTIF_CAT_EMAIL:         *icon = LV_SYMBOL_ENVELOPE; *r=0x4F;*g=0xC3;*b=0xF7; break;
    case NOTIF_CAT_INCOMING_CALL: *icon = LV_SYMBOL_CALL;     *r=0x4C;*g=0xD9;*b=0x64; break;
    case NOTIF_CAT_SCHEDULE:      *icon = LV_SYMBOL_LIST;     *r=0xFF;*g=0x3B;*b=0x30; break;
    default:                       *icon = LV_SYMBOL_BELL;     *r=0x8E;*g=0x8E;*b=0x93; break;
    }
}

/* ─── Banner UI ────────────────────────────────────────────────────────────── */

static void banner_remove(void)
{
    if (s_dismiss_tmr) {
        lv_timer_delete(s_dismiss_tmr);
        s_dismiss_tmr = NULL;
    }
    if (s_banner) {
        lv_obj_delete(s_banner);
        s_banner = NULL;
    }
    s_current_uid = 0;
}

static void dismiss_timer_cb(lv_timer_t* t)
{
    (void)t;
    banner_remove();
}

static void banner_click_cb(lv_event_t* e)
{
    uint32_t uid = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "tap → open Notifications, uid=%lu", (unsigned long)uid);
    banner_remove();
    /* Notifications app'i aç. Detail göstermek için app launch sonrası
     * mesaj zaten store'da, app on_start rebuild_list ile render eder.
     * Spesifik detail page'e gitmek için app event'i veya global state
     * lazım — şimdilik liste yeterli (kullanıcı tap'leyebilir). */
    app_manager_launch(APP_ID_NOTIFY);
}

static void show_banner_for(uint32_t uid)
{
    const notification_entry_t* n = notification_store_get(uid);
    if (!n) return;

    /* v1.4.72 — title henüz dolmamışsa (stub state) banner gösterme.
     * Birazdan MODIFIED ile gerçek içerik geldiğinde tekrar tetiklenecek. */
    if (n->title[0] == '\0') {
        ESP_LOGD(TAG, "skip stub uid=%lu (title empty)", (unsigned long)uid);
        return;
    }

    /* Varsa eski banner'ı değiştir (yeni gelene yer aç) */
    banner_remove();

    /* Layer top → her app üstünde görünür, app switch'i etkilemez */
    lv_obj_t* top = lv_layer_top();
    s_banner = lv_obj_create(top);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_size(s_banner, lv_pct(94), BANNER_HEIGHT);
    /* Slide-in: önce ekran üstünde başla, sonra y=10'a kay */
    lv_obj_set_pos(s_banner, 12, -BANNER_HEIGHT);
    lv_obj_set_style_bg_color(s_banner, lv_color_make(0x2C, 0x2C, 0x2E), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_banner, 22, 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_style_pad_all(s_banner, 0, 0);
    lv_obj_set_style_shadow_width(s_banner, 16, 0);
    lv_obj_set_style_shadow_color(s_banner, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(s_banner, LV_OPA_60, 0);
    lv_obj_clear_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_banner, 12);
    lv_obj_add_event_cb(s_banner, banner_click_cb, LV_EVENT_CLICKED,
                         (void*)(uintptr_t)uid);

    /* App icon */
    const char* icon = NULL;
    uint8_t r=0x8E, g=0x8E, b=0x93;
    resolve_visual(n, &icon, &r, &g, &b);

    lv_obj_t* icon_bg = lv_obj_create(s_banner);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, 54, 54);
    lv_obj_set_pos(icon_bg, 16, 16);
    lv_obj_set_style_bg_color(icon_bg, lv_color_make(r, g, b), 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_bg, 14, 0);
    lv_obj_clear_flag(icon_bg, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t* icon_lbl = lv_label_create(icon_bg);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_center(icon_lbl);

    /* Title (sender) — Türkçe karakter destekli custom font (v1.4.71) */
    lv_obj_t* title = lv_label_create(s_banner);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, n->title[0] ? n->title : "Notification");
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 280);
    lv_obj_set_pos(title, 84, 18);

    /* Preview (message, single line) */
    lv_obj_t* prev = lv_label_create(s_banner);
    lv_obj_set_style_text_font(prev, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(prev, lv_color_make(0xCC, 0xCC, 0xCE), 0);
    lv_label_set_text(prev, n->message[0] ? n->message : "");
    lv_label_set_long_mode(prev, LV_LABEL_LONG_DOT);
    lv_obj_set_width(prev, 280);
    lv_obj_set_pos(prev, 84, 50);

    /* "Tap to open" hint, küçük */
    lv_obj_t* hint = lv_label_create(s_banner);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_make(0x6E, 0x6E, 0x73), 0);
    lv_label_set_text(hint, "Tap to open");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -16, -8);

    /* Slide-down animasyon */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_banner);
    lv_anim_set_values(&a, -BANNER_HEIGHT, 10);
    lv_anim_set_time(&a, BANNER_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        lv_obj_set_y((lv_obj_t*)obj, v);
    });
    lv_anim_start(&a);

    s_current_uid = uid;

    /* Auto-dismiss timer */
    s_dismiss_tmr = lv_timer_create(dismiss_timer_cb, BANNER_AUTO_MS, NULL);
    lv_timer_set_repeat_count(s_dismiss_tmr, 1);

    /* Ekran wake — kullanıcı kilitliyse yansa, dim'deyse açılsın */
    power_service_user_activity(POWER_WAKE_SRC_TOUCH_DIM);

    ESP_LOGI(TAG, "show uid=%lu app=%s title=\"%s\"",
             (unsigned long)uid, n->app_id, n->title);
}

/* ─── Async dispatch (from BLE task → UI thread) ──────────────────────────── */

static void async_show(void* arg)
{
    uint32_t uid = (uint32_t)(uintptr_t)arg;
    show_banner_for(uid);
}

static void store_listener(notif_event_type_t evt, uint32_t uid, void* /*ud*/)
{
    /* v1.4.72 — ADDED (NS event, title boş) ve MODIFIED (DS response, title
     * dolu) ikisine de tepki ver. ANCS akışı şöyle:
     *   1. NS event → stub eklenir (title="") → ADDED tetiklenir
     *   2. DS attributes response → title/message dolar → MODIFIED tetiklenir
     * Banner async_show içinde title boşsa skip eder → MODIFIED'da gerçek
     * içerikle çıkar. */
    if (evt != NOTIF_EVENT_ADDED && evt != NOTIF_EVENT_MODIFIED) return;
    lv_async_call(async_show, (void*)(uintptr_t)uid);
}

/* ─── Public API ───────────────────────────────────────────────────────────── */

esp_err_t notification_banner_init(void)
{
    notification_store_add_listener(store_listener, NULL);
    ESP_LOGI(TAG, "init OK — listening for new notifications");
    return ESP_OK;
}

bool notification_banner_is_showing(void)
{
    return s_banner != NULL;
}

void notification_banner_dismiss(void)
{
    /* UI thread'den çağrılabilir; lv_async_call kullanmaya gerek yok ama
     * defansif olarak yine de async at — caller power task da olabilir. */
    lv_async_call([](void* /*a*/) { banner_remove(); }, NULL);
}
