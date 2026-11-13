/**
 * @file step_counter_app.cpp
 * @brief Adım Sayar — günlük hedef ilerleme halkası.
 *
 * Layout:
 *   Merkezi dairesel arc (cyan/mavi) günlük 10.000 adım hedefini gösterir.
 *   Arc içinde mevcut adım sayısı, altında hedef bilgisi yer alır.
 *
 * Navigasyon:
 *   TOP butonu (kısa) → geri (app_manager varsayılanı)
 *   TOP butonu (uzun) → WatchFace
 *
 * Event dispatch:
 *   on_event, app_manager'ın merkezi dispatch timer'ı üzerinden çağrılır.
 *   Uygulama artık event_bus'a doğrudan abone olmaz.
 */

#include "step_counter_app.h"
#include "../../core/app_manager.h"
#include "../../core/event_bus.h"
#include <lvgl.h>
#include <esp_log.h>

static const char* TAG = "StepApp";

static lv_obj_t* s_lbl_steps = NULL;
static lv_obj_t* s_arc_goal  = NULL;
static lv_obj_t* s_lbl_pct   = NULL;

#define DAILY_GOAL  10000

static void back_hint_clicked_cb(lv_event_t* e)
{
    (void)e;
    app_manager_back();
}

// ─── App lifecycle ─────────────────────────────────────────────────────────────

static void on_start(lv_obj_t* screen)
{
    ESP_LOGI(TAG, "StepCounter starting");
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    // ── Başlık ────────────────────────────────────────────────────────────────
    lv_obj_t* back_hint = lv_label_create(screen);
    lv_obj_set_style_text_color(back_hint, lv_color_make(0x44, 0x44, 0x44), 0);
#ifdef BOARD_VARIANT_LCD128
    /* Session 38: yuvarlak ekranda sol-üst köşe bezelin altında kalıyor
     * (y=18'de görünür kiriş x=45..195). Metin atılıp yalnız ok bırakıldı
     * ve daireye girecek şekilde içeri alındı. */
    lv_obj_align(back_hint, LV_ALIGN_TOP_LEFT, 54, 22);
    lv_label_set_text(back_hint, LV_SYMBOL_LEFT);
#else
    lv_obj_align(back_hint, LV_ALIGN_TOP_LEFT, 20, 18);
    lv_label_set_text(back_hint, LV_SYMBOL_LEFT " Back");
#endif
    lv_obj_add_flag(back_hint, LV_OBJ_FLAG_CLICKABLE);
    /* v1.4.64: 20 → 28 (Apple HIG min 44×44, ekran sol-üst curve uzaklığı) */
    lv_obj_set_ext_click_area(back_hint, 28);
    lv_obj_add_event_cb(back_hint, back_hint_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xE5, 0xFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
    lv_label_set_text(title, "Steps");

    // ── Hedef ilerleme halkası (cyan) ──────────────────────────────────────────
    s_arc_goal = lv_arc_create(screen);
#ifdef BOARD_VARIANT_LCD128
    /* Session 38: 290x290 halka 240 ekranda yarıçapı 145 px — görünür
     * dairenin (r=120) DIŞINDA kalıyordu, ilerleme halkası neredeyse hiç
     * görünmüyordu. 200x200 → yarıçap 100, kenarla 20 px pay. */
    lv_obj_set_size(s_arc_goal, 200, 200);
    lv_obj_align(s_arc_goal, LV_ALIGN_CENTER, 0, 6);
#else
    lv_obj_set_size(s_arc_goal, 290, 290);
    lv_obj_align(s_arc_goal, LV_ALIGN_CENTER, 0, 10);
#endif

    // Arka plan halkası: koyu
    lv_obj_set_style_arc_color(s_arc_goal, lv_color_make(0x1A, 0x1A, 0x1A), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_goal, 14, LV_PART_MAIN);

    // İlerleme halkası: cyan gradient hissi için açık mavi
    lv_obj_set_style_arc_color(s_arc_goal,
        lv_color_make(0x00, 0xBF, 0xFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc_goal, 14, LV_PART_INDICATOR);

    // Topu gizle
    lv_obj_set_style_bg_opa(s_arc_goal, LV_OPA_0, LV_PART_KNOB);
    lv_obj_set_style_border_opa(s_arc_goal, LV_OPA_0, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc_goal, LV_OBJ_FLAG_CLICKABLE);

    lv_arc_set_range(s_arc_goal, 0, DAILY_GOAL);
    lv_arc_set_value(s_arc_goal, 0);
    lv_arc_set_rotation(s_arc_goal, 135);          // klasik arc başlangıcı

    // ── Adım sayısı etiketi (arc içi) ─────────────────────────────────────────
    s_lbl_steps = lv_label_create(screen);
    lv_obj_set_style_text_color(s_lbl_steps, lv_color_white(), 0);
    lv_obj_align(s_lbl_steps, LV_ALIGN_CENTER, 0, -5);
    lv_obj_set_style_text_align(s_lbl_steps, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_steps, "0\nsteps");

    // ── Yüzde etiketi (arc içi, küçük) ───────────────────────────────────────
    s_lbl_pct = lv_label_create(screen);
    lv_obj_set_style_text_color(s_lbl_pct, lv_color_make(0x00, 0xE5, 0xFF), 0);
    lv_obj_align(s_lbl_pct, LV_ALIGN_CENTER, 0, 45);
    lv_label_set_text(s_lbl_pct, "0%");

    // ── Hedef etiketi (alt) ───────────────────────────────────────────────────
    lv_obj_t* lbl_goal = lv_label_create(screen);
    lv_obj_set_style_text_color(lbl_goal, lv_color_make(0x55, 0x55, 0x55), 0);
#ifdef BOARD_VARIANT_LCD128
    /* Session 38: tam metin 162 px, y=204..220'de görünür kiriş x=54..186
     * (132 px) → iki ucu bezelin altında kalıyordu. Kısa metin + yukarı. */
    lv_obj_set_style_text_font(lbl_goal, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_goal, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_label_set_text_fmt(lbl_goal, "Goal %d", DAILY_GOAL);
#else
    lv_obj_align(lbl_goal, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_label_set_text_fmt(lbl_goal, "Daily goal: %d steps", DAILY_GOAL);
#endif
}

static void on_resume(void) { ESP_LOGD(TAG, "StepCounter resumed"); }
static void on_pause(void)  { ESP_LOGD(TAG, "StepCounter paused"); }

static void on_stop(void)
{
    s_lbl_steps = NULL;
    s_arc_goal  = NULL;
    s_lbl_pct   = NULL;
    ESP_LOGD(TAG, "StepCounter stopped");
}

static bool on_event(const os_event_t* event)
{
    if (event->type == EVENT_STEP_COUNT) {
        uint32_t steps = event->data;
        uint32_t clamped = (steps > DAILY_GOAL) ? DAILY_GOAL : steps;
        uint32_t pct = (clamped * 100) / DAILY_GOAL;

        if (s_lbl_steps) {
            lv_label_set_text_fmt(s_lbl_steps, "%lu\nsteps", (unsigned long)steps);
        }
        if (s_arc_goal) {
            lv_arc_set_value(s_arc_goal, (int32_t)clamped);
        }
        if (s_lbl_pct) {
            lv_label_set_text_fmt(s_lbl_pct, "%lu%%", (unsigned long)pct);
        }
        return true;
    }
    return false;
}

// ─── Interface export ─────────────────────────────────────────────────────────

static const app_interface_t s_iface = {
    .on_start  = on_start,
    .on_resume = on_resume,
    .on_pause  = on_pause,
    .on_stop   = on_stop,
    .on_event  = on_event,
    .name      = "StepCounter",
};

const app_interface_t* step_counter_app_get_interface(void) { return &s_iface; }
