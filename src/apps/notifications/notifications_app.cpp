/**
 * @file notifications_app.cpp
 * @brief Notifications app — ANCS-backed list + detail UI.
 *
 * v1 scope:
 *   - List view: app icon + sender + preview, en yenisi yukarıda
 *   - Detail view: tam mesaj, dismiss butonu
 *   - Empty state: BLE bağlantı durumuna göre yönlendirme
 *   - Live update: notification_store listener tetiklerse liste yenilenir
 *
 * v1 dışı:
 *   - Notification arrival animation (banner)
 *   - Per-app filter UI
 *   - Quick reply
 *   - Action buttons (Answer/Decline call)
 */

#include "notifications_app.h"
#include "../../core/app_manager.h"
#include "../../services/notification_store.h"
#include "../../services/ble_service.h"
#include "../../services/ancs_service.h"
#include <lvgl.h>
#include <esp_log.h>
#include <string.h>
#include <stdio.h>

/* v1.4.75 — Türkçe font'ları geçici kaldırdık. notif_font_* dosyaları
 * (kullanıcı şüphesi: display sorununa sebep olabilir, test edilecek) — şu
 * an LVGL'in stock montserrat font'larına geri dönüyoruz. Display çalışırsa
 * font integration sorunlu, çalışmazsa başka sebep. */
/* LV_FONT_DECLARE(notif_font_16); */
/* LV_FONT_DECLARE(notif_font_20); */
/* LV_FONT_DECLARE(notif_font_24); */

static const char* TAG = "Notif";

static lv_obj_t*  s_screen        = NULL;
static lv_obj_t*  s_list_view     = NULL;
static lv_obj_t*  s_detail_view   = NULL;
static lv_obj_t*  s_empty_view    = NULL;
static uint32_t   s_detail_uid    = 0;

/* ─── App icon / color mapping ─────────────────────────────────────────────── */

typedef struct {
    const char* bundle_substr;   /* e.g. "whatsapp" — case-insensitive substr match */
    const char* icon;            /* LV_SYMBOL glyph veya UTF-8 emoji */
    uint8_t     cr, cg, cb;
} app_visual_t;

/* v1.4.72 — Bundle ID substring match, iOS spec ile aligned:
 *   WhatsApp     net.whatsapp.WhatsApp     → "whatsapp"
 *   Calendar     com.apple.mobilecal       → "mobilecal"
 *   Mail         com.apple.mobilemail      → "mobilemail"
 *   Messages     com.apple.MobileSMS       → "mobilesms"
 *   Phone        com.apple.mobilephone     → "mobilephone"
 *   Reminders    com.apple.reminders       → "reminders"
 * LVGL'in built-in sembol seti sınırlı — en yakın anlamsal eşleşme + renk
 * kombinasyonuyla app ayırt edilir. */
static const app_visual_t s_app_visuals[] = {
    { "whatsapp",        LV_SYMBOL_ENVELOPE,0x25, 0xD3, 0x66 },  /* yeşil */
    { "mobilesms",       LV_SYMBOL_ENVELOPE,0x0A, 0x84, 0xFF },  /* iMessage mavi */
    { "messages",        LV_SYMBOL_ENVELOPE,0x0A, 0x84, 0xFF },
    { "mobilemail",      LV_SYMBOL_ENVELOPE,0x4F, 0xC3, 0xF7 },
    { "telegram",        LV_SYMBOL_ENVELOPE,0x2A, 0xAB, 0xEE },
    { "instagram",       LV_SYMBOL_IMAGE,   0xE4, 0x40, 0x5F },
    { "twitter",         LV_SYMBOL_BELL,    0x1D, 0xA1, 0xF2 },
    { "facebook",        LV_SYMBOL_HOME,    0x4E, 0x6C, 0xC4 },
    { "mobilephone",     LV_SYMBOL_CALL,    0x4C, 0xD9, 0x64 },
    { "mobilecal",       LV_SYMBOL_BELL,    0xFF, 0x3B, 0x30 },  /* takvim kırmızı */
    { "calendar",        LV_SYMBOL_BELL,    0xFF, 0x3B, 0x30 },
    { "reminders",       LV_SYMBOL_LIST,    0xFF, 0x9F, 0x0A },
};

/* Case-insensitive substring match. NULL safe. */
static bool ci_contains(const char* haystack, const char* needle)
{
    if (!haystack || !needle) return false;
    size_t hl = strlen(haystack);
    size_t nl = strlen(needle);
    if (nl == 0 || nl > hl) return false;
    for (size_t i = 0; i + nl <= hl; i++) {
        bool match = true;
        for (size_t j = 0; j < nl; j++) {
            char a = haystack[i+j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
            if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static const app_visual_t* resolve_visual(const notification_entry_t* e)
{
    if (!e) return NULL;
    for (size_t i = 0; i < sizeof(s_app_visuals)/sizeof(s_app_visuals[0]); i++) {
        if (ci_contains(e->app_id, s_app_visuals[i].bundle_substr)) {
            return &s_app_visuals[i];
        }
    }
    return NULL;
}

/* Default visual'lar (mapping yoksa kategoriden tahmin) */
static void category_fallback(const notification_entry_t* e,
                               const char** icon_out, uint8_t* r, uint8_t* g, uint8_t* b)
{
    switch (e->category) {
    case NOTIF_CAT_SOCIAL:        *icon_out = LV_SYMBOL_CALL;     *r=0x25;*g=0xD3;*b=0x66; break;
    case NOTIF_CAT_EMAIL:         *icon_out = LV_SYMBOL_ENVELOPE; *r=0x4F;*g=0xC3;*b=0xF7; break;
    case NOTIF_CAT_INCOMING_CALL: *icon_out = LV_SYMBOL_CALL;     *r=0x4C;*g=0xD9;*b=0x64; break;
    case NOTIF_CAT_SCHEDULE:      *icon_out = LV_SYMBOL_LIST;     *r=0xFF;*g=0x3B;*b=0x30; break;
    case NOTIF_CAT_NEWS:          *icon_out = LV_SYMBOL_FILE;     *r=0xFF;*g=0x9F;*b=0x0A; break;
    default:                       *icon_out = LV_SYMBOL_BELL;     *r=0x8E;*g=0x8E;*b=0x93; break;
    }
}

/* ─── Forward decls ────────────────────────────────────────────────────────── */

static void rebuild_list(void);
static void show_detail(uint32_t uid);
static void hide_detail(void);
static void show_empty(void);
static void hide_empty(void);

/* ─── Empty state ──────────────────────────────────────────────────────────── */

static void show_empty(void)
{
    if (s_empty_view) lv_obj_clear_flag(s_empty_view, LV_OBJ_FLAG_HIDDEN);
    if (s_list_view)  lv_obj_add_flag  (s_list_view,  LV_OBJ_FLAG_HIDDEN);
}

static void hide_empty(void)
{
    if (s_empty_view) lv_obj_add_flag(s_empty_view, LV_OBJ_FLAG_HIDDEN);
    if (s_list_view)  lv_obj_clear_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
}

static void build_empty(lv_obj_t* parent)
{
    s_empty_view = lv_obj_create(parent);
    lv_obj_remove_style_all(s_empty_view);
    lv_obj_set_size(s_empty_view, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(s_empty_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon = lv_label_create(s_empty_view);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(icon, lv_color_make(0x3A, 0x3A, 0x3C), 0);
    lv_label_set_text(icon, LV_SYMBOL_BELL);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t* title = lv_label_create(s_empty_view);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "No Notifications");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t* sub = lv_label_create(s_empty_view);
    lv_obj_set_style_text_color(sub, lv_color_make(0x8E, 0x8E, 0x93), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
#ifdef BOARD_VARIANT_LCD128
    /* Session 38: 340 px genişlik AMOLED içindi — 240 ekranda "Pair your
     * iPhone in Bluetooth settings" (tw=308) iki yandan kırpılıyordu.
     * Dar genişlik + küçük font → metin satırlara sarıyor. */
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_width(sub, 170);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
#else
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_set_width(sub, 340);
#endif
    ble_state_t bs = ble_service_get_state();
    if (bs == BLE_STATE_BONDED) {
        lv_label_set_text(sub, "You're all caught up");
    } else if (bs == BLE_STATE_CONNECTED) {
        lv_label_set_text(sub, "Pairing with iPhone...");
    } else {
        lv_label_set_text(sub, "Pair your iPhone in Bluetooth settings");
    }
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 50);
}

/* ─── List view ────────────────────────────────────────────────────────────── */

static void list_row_click_cb(lv_event_t* e)
{
    uint32_t uid = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    show_detail(uid);
}

static void build_list_row(lv_obj_t* parent, const notification_entry_t* n, int32_t y)
{
    const app_visual_t* vis = resolve_visual(n);
    const char* icon_glyph = NULL;
    uint8_t r=0x8E, g=0x8E, b=0x93;
    if (vis) {
        icon_glyph = vis->icon;
        r = vis->cr; g = vis->cg; b = vis->cb;
    } else {
        category_fallback(n, &icon_glyph, &r, &g, &b);
    }

    /* Row container */
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(96), 84);
    lv_obj_set_pos(row, 8, y);
    lv_obj_set_style_bg_color(row, lv_color_make(0x1C, 0x1C, 0x1E), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(row, (void*)(uintptr_t)n->uid);
    lv_obj_add_event_cb(row, list_row_click_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)n->uid);

    /* App icon */
    lv_obj_t* icon_bg = lv_obj_create(row);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, 50, 50);
    lv_obj_set_pos(icon_bg, 16, 17);
    lv_obj_set_style_bg_color(icon_bg, lv_color_make(r, g, b), 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_bg, 12, 0);
    lv_obj_clear_flag(icon_bg, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t* icon_lbl = lv_label_create(icon_bg);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
    lv_label_set_text(icon_lbl, icon_glyph);
    lv_obj_center(icon_lbl);

    /* Title (sender) — Türkçe karakter destekli custom font (v1.4.71) */
    lv_obj_t* title = lv_label_create(row);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, n->title[0] ? n->title : "(no title)");
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 280);
    lv_obj_set_pos(title, 78, 14);

    /* Preview (message, single line) */
    lv_obj_t* prev = lv_label_create(row);
    lv_obj_set_style_text_font(prev, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(prev, lv_color_make(0xAE, 0xAE, 0xB2), 0);
    lv_label_set_text(prev, n->message[0] ? n->message : "");
    lv_label_set_long_mode(prev, LV_LABEL_LONG_DOT);
    lv_obj_set_width(prev, 280);
    lv_obj_set_pos(prev, 78, 46);
}

static void rebuild_list(void)
{
    if (!s_list_view) return;
    lv_obj_clean(s_list_view);

    size_t count = notification_store_count();
    if (count == 0) {
        show_empty();
        return;
    }
    hide_empty();

    /* Top spacer for "Notifications" header */
    lv_obj_t* header = lv_label_create(s_list_view);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(header, lv_color_white(), 0);
    lv_label_set_text(header, "Notifications");
    lv_obj_set_pos(header, 24, 16);

    int32_t y = 64;
    for (size_t i = 0; i < count; i++) {
        const notification_entry_t* n = notification_store_at(i);
        if (!n) continue;
        build_list_row(s_list_view, n, y);
        y += 92;
    }
}

/* ─── Detail view ──────────────────────────────────────────────────────────── */

static void detail_back_cb(lv_event_t* e)
{
    (void)e;
    hide_detail();
}

static void detail_dismiss_cb(lv_event_t* e)
{
    (void)e;
    if (s_detail_uid != 0) {
        ancs_service_dismiss(s_detail_uid);
    }
    hide_detail();
}

static void show_detail(uint32_t uid)
{
    const notification_entry_t* n = notification_store_get(uid);
    if (!n) return;
    s_detail_uid = uid;

    if (!s_detail_view) {
        s_detail_view = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_detail_view);
        lv_obj_set_size(s_detail_view, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(s_detail_view, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_detail_view, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_detail_view, LV_DIR_VER);
    } else {
        lv_obj_clean(s_detail_view);
        lv_obj_clear_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    }

    /* Back */
    lv_obj_t* back = lv_label_create(s_detail_view);
    lv_obj_set_style_text_color(back, lv_color_make(0x0A, 0x84, 0xFF), 0);
    lv_obj_set_style_text_font(back, &lv_font_montserrat_20, 0);
    lv_label_set_text(back, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_pos(back, 24, 14);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 28);
    lv_obj_add_event_cb(back, detail_back_cb, LV_EVENT_CLICKED, NULL);

    /* App icon */
    const app_visual_t* vis = resolve_visual(n);
    const char* icon_glyph = NULL;
    uint8_t r=0x8E, g=0x8E, b=0x93;
    if (vis) { icon_glyph = vis->icon; r=vis->cr; g=vis->cg; b=vis->cb; }
    else     { category_fallback(n, &icon_glyph, &r, &g, &b); }

    lv_obj_t* icon_bg = lv_obj_create(s_detail_view);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, 80, 80);
    lv_obj_align(icon_bg, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(icon_bg, lv_color_make(r, g, b), 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_bg, 18, 0);
    lv_obj_clear_flag(icon_bg, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t* icon_lbl = lv_label_create(icon_bg);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
    lv_label_set_text(icon_lbl, icon_glyph);
    lv_obj_center(icon_lbl);

    /* Title — Türkçe karakter destekli custom font (v1.4.71) */
    lv_obj_t* title = lv_label_create(s_detail_view);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, 360);
    lv_label_set_text(title, n->title[0] ? n->title : "(no title)");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 148);

    /* Subtitle (group) */
    if (n->subtitle[0]) {
        lv_obj_t* sub = lv_label_create(s_detail_view);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(sub, lv_color_make(0x8E, 0x8E, 0x93), 0);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(sub, 360);
        lv_label_set_text(sub, n->subtitle);
        lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 182);
    }

    /* Message body */
    lv_obj_t* msg = lv_label_create(s_detail_view);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_obj_set_width(msg, 360);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(msg, n->message[0] ? n->message : "(no content)");
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 220);

    /* Dismiss button */
    lv_obj_t* dismiss = lv_label_create(s_detail_view);
    lv_obj_set_style_text_color(dismiss, lv_color_make(0xFF, 0x3B, 0x30), 0);
    lv_obj_set_style_text_font(dismiss, &lv_font_montserrat_22, 0);
    lv_label_set_text(dismiss, "Dismiss");
    lv_obj_align(dismiss, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_obj_add_flag(dismiss, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(dismiss, 28);
    lv_obj_add_event_cb(dismiss, detail_dismiss_cb, LV_EVENT_CLICKED, NULL);
}

static void hide_detail(void)
{
    s_detail_uid = 0;
    if (s_detail_view) {
        lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ─── Store listener (called from BLE task — must lv_async_call) ──────────── */

static void async_refresh(void* /*arg*/)
{
    /* If detail view is open and target removed, close it */
    if (s_detail_uid != 0 && notification_store_get(s_detail_uid) == NULL) {
        hide_detail();
    }
    rebuild_list();
}

static void store_listener(notif_event_type_t evt, uint32_t uid, void* /*user_data*/)
{
    (void)evt; (void)uid;
    /* BLE task'tan tetiklenir — LVGL'i doğrudan dokunma, async ile UI thread'e at */
    lv_async_call(async_refresh, NULL);
}

/* ─── App lifecycle ────────────────────────────────────────────────────────── */

static void on_start(lv_obj_t* screen)
{
    ESP_LOGI(TAG, "Notifications app start");
    s_screen = screen;
    s_detail_uid = 0;
    s_detail_view = NULL;

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* Container: list view (scrollable) */
    s_list_view = lv_obj_create(screen);
    lv_obj_remove_style_all(s_list_view);
    lv_obj_set_size(s_list_view, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_list_view, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(s_list_view, LV_DIR_VER);

    /* Empty state overlay */
    build_empty(screen);

    app_manager_enable_gestures(screen);

    /* Listener subscribe + initial render. v1.4.70: add_listener (multi). */
    notification_store_add_listener(store_listener, NULL);
    rebuild_list();
}

static void on_resume(void)
{
    /* BLE state değişmiş olabilir, listeyi tazele */
    rebuild_list();
}

static void on_pause(void)  {}
static void on_stop(void)   {
    notification_store_remove_listener(store_listener, NULL);
    s_screen      = NULL;
    s_list_view   = NULL;
    s_detail_view = NULL;
    s_empty_view  = NULL;
    s_detail_uid  = 0;
}

static bool on_event(const os_event_t* /*e*/) { return false; }

static const app_interface_t s_iface = {
    .on_start  = on_start,
    .on_resume = on_resume,
    .on_pause  = on_pause,
    .on_stop   = on_stop,
    .on_event  = on_event,
    .name      = "Notifications",
};

const app_interface_t* notifications_app_get_interface(void) { return &s_iface; }
