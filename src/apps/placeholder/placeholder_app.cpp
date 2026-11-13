/**
 * @file placeholder_app.cpp
 * @brief Henüz implemente edilmemiş uygulamalar için "Coming Soon" ekranı.
 *        Dört farklı placeholder app aynı on_start mantığını paylaşır,
 *        sadece isimleri farklıdır.
 */

#include "placeholder_app.h"
#include "../../core/app_manager.h"
#include "../../core/event_bus.h"
#include <lvgl.h>
#include <esp_log.h>

static const char* TAG = "Placeholder";

static void back_hint_clicked_cb(lv_event_t* e)
{
    (void)e;
    app_manager_back();
}

// ─── Generic placeholder screen ───────────────────────────────────────────────

static void placeholder_on_start(lv_obj_t* screen, const char* app_name)
{
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    // Uygulama adı (üst orta)
    lv_obj_t* title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);
    lv_label_set_text(title, app_name);

    // "Coming Soon" yazısı
    lv_obj_t* sub = lv_label_create(screen);
    lv_obj_set_style_text_color(sub, lv_color_make(0x44, 0x44, 0x44), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(sub, "Coming Soon");

    // Geri ipucu — tıklanabilir.
    lv_obj_t* hint = lv_label_create(screen);
    lv_obj_set_style_text_color(hint, lv_color_make(0x33, 0x33, 0x33), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_label_set_text(hint, LV_SYMBOL_LEFT " Back");
    lv_obj_add_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    /* v1.4.64: 24 → 28 (diğer back hedefleriyle tutarlı) */
    lv_obj_set_ext_click_area(hint, 28);
    lv_obj_add_event_cb(hint, back_hint_clicked_cb, LV_EVENT_CLICKED, NULL);
}

static void noop_pause(void)  {}
static void noop_resume(void) {}
static void noop_stop(void)   {}
static bool noop_event(const os_event_t* e) { (void)e; return false; }

// ─── Heart Rate ───────────────────────────────────────────────────────────────

static void heartrate_on_start(lv_obj_t* s) { placeholder_on_start(s, "Heart Rate"); }
static const app_interface_t s_heartrate_iface = {
    .on_start  = heartrate_on_start,
    .on_resume = noop_resume,
    .on_pause  = noop_pause,
    .on_stop   = noop_stop,
    .on_event  = noop_event,
    .name      = "Heart Rate",
};
const app_interface_t* heartrate_app_get_interface(void) { return &s_heartrate_iface; }

// (Weather: gerçek implementasyonu src/apps/weather/weather_app.cpp'de.)

// ─── Timer ────────────────────────────────────────────────────────────────────

static void timer_on_start(lv_obj_t* s) { placeholder_on_start(s, "Timer"); }
static const app_interface_t s_timer_iface = {
    .on_start  = timer_on_start,
    .on_resume = noop_resume,
    .on_pause  = noop_pause,
    .on_stop   = noop_stop,
    .on_event  = noop_event,
    .name      = "Timer",
};
const app_interface_t* timer_app_get_interface(void) { return &s_timer_iface; }

// Notifications app placeholder kaldırıldı (v1.2.8) — gerçek ANCS implementation:
//   src/apps/notifications/notifications_app.cpp
