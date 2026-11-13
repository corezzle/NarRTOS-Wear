/**
 * @file watchface_app.cpp
 * @brief Analog Watch Face — Apple Watch style, screen-adaptive
 *
 * Tick marks follow the ROUNDED-RECT PERIMETER of the screen, not a circle.
 * This fills the entire rectangular display edge-to-edge, matching Apple Watch.
 *
 * Geometry:
 *   - Ticks placed along a rounded-rect path (corner radius = CORNER_R px)
 *     at TICK_MARGIN px inside the screen edge.
 *   - 12 tick positions evenly spaced around that perimeter.
 *   - Clock hands still rotate from center (CX, CY) using standard angles.
 *   - Second hand is 2 px wide (thin, like the reference).
 */

#include "watchface_app.h"
#include "../../core/app_manager.h"
#include "../../services/ui_service.h"
#include "../../services/face_upload_service.h"
#include "../../services/gallery_service.h"
#include "../../../include/wear_motion.h"
#include <lvgl.h>
#include <esp_log.h>
#include <math.h>
#include <esp_timer.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <esp_heap_caps.h>

#include "../../assets/corezzle_logo_a8_140.h"
#include "../../assets/partly_cloud_icon.h"
#include "../../assets/running_man_icon.h"
#include "../../assets/weather_icon.h"
#include "../../services/weather_service.h"

#ifndef LV_SYMBOL_CLOUD
#define LV_SYMBOL_CLOUD "\xef\x83\x82"
#endif
#ifndef LV_SYMBOL_PLAY
#define LV_SYMBOL_PLAY "\xef\x81\x8b"
#endif

static const char* TAG = "WatchFace";

LV_FONT_DECLARE(lv_font_nunito_extrabold_60);
LV_FONT_DECLARE(lv_font_nunito_extrabold_70);
LV_FONT_DECLARE(lv_font_rubik_black_280);
LV_FONT_DECLARE(lv_font_rubik_black_160);
LV_FONT_DECLARE(lv_font_rubik_black_220);
LV_FONT_DECLARE(lv_font_rubik_black_180);

/* Digital kadran font takma adları (v1.2.9b). 240×240 LCD'de 160-280px
 * fontlar taşıyor (cihaz testi 2026-07-13: selector thumb'ında dev rakamlar).
 * Bu kart için Rubik-Black.ttf'den rakam-only 100px/64px üretildi
 * (npx lv_font_conv, sadece 0-9 → ~200KB flash). AMOLED dalı orijinal
 * fontların birebir aynısını kullanır. */
#ifdef BOARD_VARIANT_LCD128
LV_FONT_DECLARE(lv_font_rubik_black_100);
LV_FONT_DECLARE(lv_font_rubik_black_64);
LV_FONT_DECLARE(lv_font_nunito_extrabold_30);
#define DGT_FONT_MAIN   (&lv_font_rubik_black_100)   /* 280 karşılığı */
#define DGT_FONT_MID    (&lv_font_rubik_black_100)   /* 220 karşılığı */
#define DGT_FONT_SMALL  (&lv_font_rubik_black_64)    /* 180 karşılığı */
#define DGT_FONT_SEL    (&lv_font_rubik_black_64)    /* 160 (selector) karşılığı */
#define DGT_THUMB_FONT  (&lv_font_rubik_black_64)    /* thumb çizimi */
#else
#define DGT_FONT_MAIN   (&lv_font_rubik_black_280)
#define DGT_FONT_MID    (&lv_font_rubik_black_220)
#define DGT_FONT_SMALL  (&lv_font_rubik_black_180)
#define DGT_FONT_SEL    (&lv_font_rubik_black_160)
#define DGT_THUMB_FONT  (&lv_font_rubik_black_160)
#endif

/* Selector UI fontu (başlık + Edit). 240 ekranda 24px oransal büyük kalıyor. */
#ifdef BOARD_VARIANT_LCD128
#define SEL_UI_FONT   (&lv_font_montserrat_16)
#else
#define SEL_UI_FONT   (&lv_font_montserrat_24)
#endif

typedef enum {
    FACE_ANALOG     = 0,
    FACE_DIGITAL    = 1,
    FACE_ACTIVITY   = 2,
    FACE_IO_CONTROL = 3,   // v1.2.1: 2 mock toggle face (IO15/IO14 control panel)
    FACE_CUSTOM_0   = 4,   // (shifted from 3) uploaded custom image, slot 0
    FACE_CUSTOM_1   = 5,   // (shifted from 4) uploaded custom image, slot 1
    FACE_NEW        = 6,   // (shifted from 5) virtual "add new" card
    FACE_SLOT_COUNT        // total array capacity (7)
} face_index_t;

#define FACE_COUNT         4   // built-in: analog, digital, activity, io_control
#define FACE_BUILTIN_COUNT 4

typedef enum {
    MODE_NORMAL = 0,
    MODE_SELECTOR = 1
} face_mode_t;

typedef struct
{
    float x_off;
    float left_d;
    float right_d;
    face_index_t left_face;
    face_index_t right_face;
    face_index_t center_face;
    float visual_pos;
} carousel_frame_t;

static carousel_frame_t s_locked_frame;

static void update_digital_labels(const struct tm *ti);
static void update_activity_labels(const struct tm *ti);
static void update_io_control_labels(const struct tm *ti);   /* v1.2.1 */
static void build_io_control_layer(void);
static void io_toggle_clicked_cb(lv_event_t* e);
static void draw_io_control_thumb(lv_layer_t*, int32_t, int32_t, int32_t, int32_t, float);
static void set_face_index(face_index_t idx);
static void selector_apply_pose(void);
static inline bool face_is_custom(int idx);
static inline float wrap_face_pos_(float pos);
static inline float clampf_(float v, float lo, float hi);
static inline float shortest_delta_(float target, float from);
static void build_analog_base(void);
static void build_face_snapshot(face_index_t face);
static void free_face_snapshots(void);
static void draw_analog_thumb(lv_layer_t*, int32_t, int32_t, int32_t, int32_t, float);
static void draw_digital_thumb(lv_layer_t*, int32_t, int32_t, int32_t, int32_t, float);
static void draw_activity_thumb(lv_layer_t*, int32_t, int32_t, int32_t, int32_t, float);
static void draw_new_thumb(lv_layer_t*, int32_t, int32_t, int32_t, int32_t, float);
static void freeze_frame(void);
static void cc_open(void);
static void cc_close(void);
static void*    s_snap_raw_ptr[FACE_SLOT_COUNT] = {};

// ─── Layout constants ─────────────────────────────────────────────────────────
#ifdef BOARD_VARIANT_LCD128
/* 1.28" 240×240 yuvarlak LCD (GC9A01) için ölçeklenmiş kadran geometrisi.
 * Oranlar 410×502 orijinalinden ~0.55-0.58 ölçekle türetildi; el uzunlukları
 * yarıçap (120) içinde kalacak şekilde kırpıldı. AMOLED build'ı bu bloğu
 * hiç görmez (#else dalı birebir orijinal). */
#define SCR_W  240
#define SCR_H  240
#define SAFE_INSET 4

// Yuvarlak panel → tick ring tam daire
#define CORNER_R   120

#define TICK_MARGIN  2
#define TICK_LEN_HR  16
#define TICK_LEN_MIN 7
#define TICK_W_HR    6
#define TICK_W_MIN   2

#define H_TIP    70
#define H_TAIL   10
#define H_OW      6

#define M_TIP   100
#define M_TAIL    8
#define M_OW      4

#define S_TIP    92
#define S_TAIL   18
#define S_W       1

#define CAP_R_OUT  6
#define CAP_R_IN   3

#define CENTER_Y_ADJ 0
#define LOGO_Y_GAP   10

/* Logo blit ölçeği (256 = %100). 140px asset → %62 ≈ 87px genişlik.
 * Cihaz testi: %50 küçük, %100 taşkın — %62 dengeli. */
#define LOGO_SCALE_256 160

#define NOTIF_R    4
#define NOTIF_Y_OFF (TICK_MARGIN + TICK_LEN_HR + NOTIF_R + 4)
#else
#define SCR_W  410
#define SCR_H  502
#define SAFE_INSET 6  /* visible safe area inset */

// Rounded-rect corner radius for the tick ring
// (should roughly match the physical display glass corner radius)
#define CORNER_R   72

// Tick geometry
#define TICK_MARGIN  4     /* px inside screen edge to outer tick end */
#define TICK_LEN_HR  28    /* hour tick length inward */
#define TICK_LEN_MIN 12    /* minute tick length inward */
#define TICK_W_HR    10     /* hour tick stroke width */
#define TICK_W_MIN   2     /* minute tick stroke width */

// Hour hand: outlined pill
#define H_TIP   125
#define H_TAIL    16
#define H_OW     10

// Minute hand: outlined pill
#define M_TIP   190
#define M_TAIL    12
#define M_OW      7

// Second hand — thin red line
#define S_TIP   165
#define S_TAIL   30
#define S_W       1    /* 1 px: thinner */

// Center cap
#define CAP_R_OUT  9
#define CAP_R_IN   5

#define CENTER_Y_ADJ 0
#define LOGO_Y_GAP   18

// Notification dot (12 o'clock, just below inner tick edge)
#define NOTIF_R    5
#define NOTIF_Y_OFF (TICK_MARGIN + TICK_LEN_HR + NOTIF_R + 5)
#endif /* BOARD_VARIANT_LCD128 */

// ─── State ───────────────────────────────────────────────────────────────────
static lv_obj_t*   s_face  = NULL;
static lv_timer_t* s_timer = NULL;          /* 30 ms — anim_timer_cb (hand motion, selector) */
static lv_timer_t* s_minute_timer = NULL;   /* v1.2.2: 500 ms — minute-gated invalidate (digital/io_control) */
static face_index_t s_face_index = FACE_ANALOG;
static face_mode_t  s_face_mode  = MODE_NORMAL;
static lv_obj_t*    s_digital_layer = NULL;
static lv_obj_t*    s_digital_top = NULL;
static lv_obj_t*    s_digital_bottom = NULL;
static lv_obj_t*    s_digital_hour_tens_top = NULL;
static lv_obj_t*    s_digital_hour_ones_top = NULL;
static lv_obj_t*    s_digital_min_tens_top  = NULL;
static lv_obj_t*    s_digital_min_ones_top  = NULL;
static lv_obj_t*    s_digital_hour_tens_bot = NULL;
static lv_obj_t*    s_digital_hour_ones_bot = NULL;
static lv_obj_t*    s_digital_min_tens_bot  = NULL;
static lv_obj_t*    s_digital_min_ones_bot  = NULL;
static lv_obj_t*    s_activity_layer = NULL;
static lv_obj_t*    s_activity_time = NULL;
static lv_obj_t*    s_activity_goal = NULL;
static lv_obj_t*    s_activity_cal = NULL;
static lv_obj_t*    s_activity_hr = NULL;
static lv_obj_t*    s_activity_date = NULL;
static lv_obj_t*    s_activity_icon_cloud_bg = NULL;
static lv_obj_t*    s_activity_icon_run_bg = NULL;
static lv_obj_t*    s_activity_icon_cloud = NULL;
static lv_obj_t*    s_activity_icon_run = NULL;
static lv_obj_t*    s_activity_gauge_val = NULL;
static lv_obj_t*    s_activity_gauge_min = NULL;
static lv_obj_t*    s_activity_gauge_max = NULL;
static lv_obj_t*    s_activity_msg_badge = NULL;
static lv_obj_t*    s_activity_msg_label = NULL;
static lv_obj_t*    s_activity_notif_dot = NULL;
static lv_obj_t*    s_activity_touch_layer = NULL;

/* v1.2.1 — FACE_IO_CONTROL widget'ları + RTC_DATA_ATTR mock state.
 *
 * Mock toggle: HW'ye yazma yok (GPIO 14/15 I²C bus, kullanılamaz).
 * Kullanıcı ileride bağlantı yapılınca io_toggle_clicked_cb içinden
 * gerçek gpio_set_level çağrılarını eklenecek. Şimdilik sadece state +
 * UI feedback.
 *
 * RTC_DATA_ATTR: deep sleep cycle'ında state korunur (RAM kaybolmaz —
 * RTC slow memory). Reboot/power-cycle'da default OFF (safe state, RTC
 * memory power kesilince temizlenir). NVS persist yok, kullanıcı tercihi. */
static RTC_DATA_ATTR bool s_io_state[2] = {false, false};

static lv_obj_t*    s_io_layer        = NULL;
static lv_obj_t*    s_io_time_label   = NULL;
static lv_obj_t*    s_io_date_label   = NULL;
static lv_obj_t*    s_io_toggle_outer[2] = {NULL, NULL};   /* 0=Çıkış 1, 1=Çıkış 2 */
static lv_obj_t*    s_io_toggle_knob[2]  = {NULL, NULL};
static lv_obj_t*    s_io_toggle_card[2]  = {NULL, NULL};   /* tıklanabilir kart (parent) */
static lv_obj_t*    s_io_toggle_title[2] = {NULL, NULL};

static lv_obj_t*    s_selector_border = NULL;
static lv_obj_t*    s_selector_title = NULL;
static lv_obj_t*    s_selector_edit_btn = NULL;
static lv_obj_t*    s_selector_edit_label = NULL;
static lv_obj_t*    s_selector_side_left = NULL;
static lv_obj_t*    s_selector_side_right = NULL;
static lv_obj_t*    s_cc_overlay = NULL;
static lv_obj_t*    s_cc_panel = NULL;
static bool         s_cc_open = false;
/* v1.4.55 (2026-05-19) — audit orta öncelik #5a: s_cc_swipe_zone tamamen
 * kaldırıldı. Geçmişte 1:1 finger tracking için top-edge 60px overlay'di;
 * ESP32-S3 60 FPS tutamadığı için iptal edildi ama widget yaratımı + 4
 * event callback (LV_OBJ_FLAG_CLICKABLE ile) kaldı → callback no-op olduğu
 * için ekranın üst 60 px'lik bandı kullanıcının tap'lerini YUTUYORDU.
 * Tamamen kaldırıldı: static decl + cb fonksiyonu + create bloğu. */
static bool         s_cc_drag_active = false;
static int16_t      s_cc_drag_start_y = 0;
static int64_t      s_cc_drag_suppress_until_us = 0;
static int32_t      s_logo_center_x = 0;
static int32_t      s_logo_min_y = 0;
static bool         s_logo_metrics_ready = false;
static bool         s_sel_down = false;
static bool         s_sel_swiped_recent = false;
static bool         s_sel_swipe_just_changed = false;
static bool         s_sel_dragged = false;
static int64_t      s_sel_swipe_block_until_us = 0;
static int64_t      s_sel_press_start_us = 0;
static bool         s_sel_ignore_release_once = false;
static int16_t      s_sel_x0 = -1;
static int16_t      s_sel_y0 = -1;
static int16_t      s_sel_last_x = -1;
static int16_t      s_sel_last_y = -1;
static int64_t      s_sel_ignore_click_until_us = 0;
static bool         s_ignore_next_selector_click = false;
static bool         s_hold_down = false;
static bool         s_hold_fired = false;
static int64_t      s_hold_start_us = 0;
static int16_t      s_norm_x0 = -1;
static int16_t      s_norm_y0 = -1;
static int16_t      s_norm_last_x = -1;
static int16_t      s_norm_last_y = -1;
static bool         s_norm_swiped = false;
static bool         s_norm_dragging = false;
static float        s_draw_scale = 1.0f;
static float        s_draw_y_off = 0.0f;
static float        s_draw_x_off = 0.0f;
static int          s_pending_face_index = -1;
static float        s_sel_pos = 0.0f;
static float        s_sel_visual_pos = 0.0f;
static float        s_sel_vel = 0.0f;
static int64_t      s_sel_last_sample_us = 0;
static bool         s_sel_release_pending = false;
static int64_t      s_sel_release_due_us = 0;
static bool         s_sel_snap_active = false;
static float        s_sel_snap_target = 0.0f;
static float        s_sel_snap_start = 0.0f;
static float        s_sel_snap_t = 1.0f;
static bool         s_sel_delete_mode = false;
static float        s_sel_delete_reveal = 0.0f; // 0..1 (card lift progress)
static int64_t      s_sel_last_drag_us = 0;
static bool         s_sel_click_armed = false;
static int64_t      s_sel_click_armed_until_us = 0;

static constexpr int64_t SEL_RELEASE_DEBOUNCE_US = 80000;   // was 120ms — faster release detection
/* Apple Watch tarzı raylı kayma: 140 ms snap çok hızlı bitiyordu, kullanıcı
 * "şipşak yerine oturdu" hissi alıyordu. 280 ms ease-out-quint ile mesafenin
 * %50'si ilk %25 zamanda alınır (snappy başlangıç) → kalan %50 sonraki %75
 * zamanda yumuşakça glide eder = "raylı kayma" hissi. */
static constexpr float SEL_SNAP_DURATION_S = 0.28f;

// ── Selector carousel geometry ────────────────────────────────────────────────
// CARD_SCALE: all cards same size (no squeeze/deform)
// CARD_W = 410*0.68 = 279, GAP = 10  → adjacent peek = (205 - 279/2 - 10) = 56px = 20%
static constexpr float SEL_CARD_SCALE   = 0.68f;
#ifdef BOARD_VARIANT_LCD128
static constexpr float SEL_CARD_SPACING = 178.0f;  // CARD_W(240*0.68=163) + GAP(15)
#else
static constexpr float SEL_CARD_SPACING = 305.0f;  // CARD_W(279) + GAP(26) — ~13px each side
#endif
static constexpr float SEL_SCALE_MIN    = SEL_CARD_SCALE;
static constexpr float SEL_SCALE_MAX    = SEL_CARD_SCALE;

// Side card thumbnail state (updated in update_selector_ui, read in face_draw_cb)
static face_index_t s_left_card_face  = FACE_ACTIVITY;
static face_index_t s_right_card_face = FACE_DIGITAL;
static float        s_left_card_d     = -99.0f;
static float        s_right_card_d    = +99.0f;

// ── Render-frozen frame values ────────────────────────────────────────────────
// Captured once at invalidation, held constant across all partial-render strips.
// Prevents mid-frame tearing when touch updates s_draw_x_off between strips.
static float   s_frame_x_off       = 0.0f;
static float   s_frame_left_d      = -99.0f;
static float   s_frame_right_d     = +99.0f;
static face_index_t s_frame_left_face  = FACE_ACTIVITY;
static face_index_t s_frame_right_face = FACE_DIGITAL;
static face_index_t s_frame_center     = FACE_ANALOG;

// ── Thumbnail snapshot cache ───────────────────────────────────────────────────
// Pre-rendered once per selector entry; freed on on_stop.
// Stored in PSRAM (MALLOC_CAP_SPIRAM) — ~190KB each, 3 total = ~570KB.
#define SNAP_W  ((int32_t)(SCR_W * SEL_CARD_SCALE))   // 279
#define SNAP_H  ((int32_t)(SCR_H * SEL_CARD_SCALE))   // 341

static lv_draw_buf_t s_snap_buf_obj[FACE_SLOT_COUNT];
static bool          s_snap_valid[FACE_SLOT_COUNT] = {};

// ─── Custom image faces (PSRAM, not persistent) ───────────────────────────────
static int           s_custom_face_count = 0;    // 0, 1, or 2
static int           s_custom_slot_map[FACE_UPLOAD_MAX_SLOTS] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};
static lv_image_dsc_t s_custom_buf_obj[FACE_UPLOAD_MAX_SLOTS];
static lv_image_dsc_t s_custom_thumb_obj[FACE_UPLOAD_MAX_SLOTS];
static void*         s_custom_thumb_raw[FACE_UPLOAD_MAX_SLOTS] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
static bool          s_custom_hidden_slot[FACE_UPLOAD_MAX_SLOTS] = {
    false, false, false, false, false, false, false, false, false, false, false, false
};

static void ensure_custom_thumb(int slot, const uint16_t* src565)
{
    if (slot < 0 || slot >= FACE_UPLOAD_MAX_SLOTS || !src565) return;
    if (s_custom_thumb_raw[slot]) return;

    const int tw  = SNAP_W;
    const int th  = SNAP_H;
    // Corner radius matching the selector card at SEL_CARD_SCALE — rounded corners are masked
    // to black (AMOLED bg) so the photo never overflows the card frame.
    const int32_t rad = (int32_t)roundf((float)CORNER_R * SEL_CARD_SCALE);  // ~49 px

    size_t bytes = (size_t)tw * (size_t)th * 2u;
    uint16_t* dst = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst) dst = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!dst) return;

    // Nearest-neighbor downscale from 410×502 → SNAP_W×SNAP_H, with rounded corner masking.
    // Pixels outside the card radius become pure black so they blend with the AMOLED background.
    for (int y = 0; y < th; y++) {
        int sy = (y * FACE_UPLOAD_H) / th;
        const uint16_t* srow = src565 + sy * FACE_UPLOAD_W;
        uint16_t* drow = dst + y * tw;
        for (int x = 0; x < tw; x++) {
            // Check whether this pixel falls outside any of the four rounded corners.
            bool outside = false;
            if (x < rad && y < rad) {
                int dx = rad - x, dy = rad - y;
                outside = ((int32_t)dx*dx + (int32_t)dy*dy) > (int32_t)rad*rad;
            } else if (x >= tw - rad && y < rad) {
                int dx = x - (tw - 1 - rad), dy = rad - y;
                outside = ((int32_t)dx*dx + (int32_t)dy*dy) > (int32_t)rad*rad;
            } else if (x < rad && y >= th - rad) {
                int dx = rad - x, dy = y - (th - 1 - rad);
                outside = ((int32_t)dx*dx + (int32_t)dy*dy) > (int32_t)rad*rad;
            } else if (x >= tw - rad && y >= th - rad) {
                int dx = x - (tw - 1 - rad), dy = y - (th - 1 - rad);
                outside = ((int32_t)dx*dx + (int32_t)dy*dy) > (int32_t)rad*rad;
            }
            if (outside) {
                drow[x] = 0x0000;   // pure black — invisible on AMOLED
            } else {
                int sx = (x * FACE_UPLOAD_W) / tw;
                drow[x] = srow[sx];
            }
        }
    }

    s_custom_thumb_raw[slot] = dst;
    uint32_t stride = lv_draw_buf_width_to_stride(tw, LV_COLOR_FORMAT_RGB565);
    lv_image_dsc_t* d = &s_custom_thumb_obj[slot];
    d->header.magic     = LV_IMAGE_HEADER_MAGIC;
    d->header.cf        = LV_COLOR_FORMAT_RGB565;
    d->header.flags     = 0;
    d->header.w         = (uint16_t)tw;
    d->header.h         = (uint16_t)th;
    d->header.stride    = (uint16_t)stride;
    d->header.reserved_2 = 0;
    d->data_size        = (uint32_t)bytes;
    d->data             = (const uint8_t*)dst;
    d->reserved         = NULL;
}

// Dynamic carousel helpers
static inline int carousel_size(void)  { return FACE_BUILTIN_COUNT + s_custom_face_count + 1; }
static inline int face_new_idx(void)   { return FACE_BUILTIN_COUNT + s_custom_face_count; }

static bool         s_gal_first_sync_done = false;   /* Session 13 */
static lv_timer_t*  s_gal_sync_timer      = NULL;

static void refresh_custom_faces_from_gallery(void);

/* Hafif kısım — ağır yükleme bittikten sonra UI thread'inde çalışır. */
static void gal_sync_finish_cb(void* arg)
{
    (void)arg;
    refresh_custom_faces_from_gallery();     /* store yüklü → ~0 ms */
    s_gal_first_sync_done = true;
    ESP_LOGI(TAG, "gallery sync tamam (UI bloklanmadı)");
}

/* Ağır kısım — depolama mount + kadran dosyalarının okunması (~470 ms).
 * UI task'ında yapılırsa boot'tan sonra yarım saniyelik donma oluyordu.
 * Kısa ömürlü işçi görev: boot'un ilk saniyesinde face_upload deposuna
 * başka dokunan yok (Galeri uygulamasına ulaşmak birkaç tık sürer). */
static void gal_load_task(void* arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    (void)face_upload_slot_count();          /* load_slots_from_store tetikler */
    ESP_LOGI(TAG, "gallery store yüklendi %lld ms (arka plan)",
             (long long)((esp_timer_get_time() - t0) / 1000));
    ui_service_lock();
    lv_async_call(gal_sync_finish_cb, NULL);
    ui_service_unlock();
    vTaskDelete(NULL);
}

static void gal_first_sync_cb(lv_timer_t* t)
{
    (void)t;
    s_gal_sync_timer = NULL;                 /* repeat_count=1 → LVGL kendi siler */
    if (xTaskCreate(gal_load_task, "gal_load", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "gal_load task spawn failed — senkron yükleniyor");
        refresh_custom_faces_from_gallery();
        s_gal_first_sync_done = true;
    }
}

static void refresh_custom_faces_from_gallery(void)
{
    // Sync the gallery list from the upload service (reads s_slots[] state).
    gallery_service_sync_uploaded();

    // Explicitly picked from Gallery -> allow it again in selector.
    int picked = gallery_service_consume_pending_pick();
    if (picked >= 0 && picked < FACE_UPLOAD_MAX_SLOTS) {
        s_custom_hidden_slot[picked] = false;
    }

    // Custom cards are added to selector only when explicitly picked from Gallery.
    if (picked >= 0 && picked < FACE_UPLOAD_MAX_SLOTS && s_custom_face_count < FACE_UPLOAD_MAX_SLOTS) {
        bool found = false;
        for (int j = 0; j < s_custom_face_count; j++) {
            if (s_custom_slot_map[j] == picked) { found = true; break; }
        }

        if (!found) {
            const face_upload_slot_t* s = face_upload_get_slot(picked);
            if (s && s->valid && s->buf && !gallery_service_slot_hidden(picked) && !s_custom_hidden_slot[picked]) {
                s_custom_slot_map[s_custom_face_count] = picked;
                uint32_t stride = lv_draw_buf_width_to_stride(FACE_UPLOAD_W, LV_COLOR_FORMAT_RGB565);
                lv_image_dsc_t* d = &s_custom_buf_obj[s_custom_face_count];
                d->header.magic = LV_IMAGE_HEADER_MAGIC;
                d->header.cf = LV_COLOR_FORMAT_RGB565;
                d->header.flags = 0;
                d->header.w = FACE_UPLOAD_W;
                d->header.h = FACE_UPLOAD_H;
                d->header.stride = (uint16_t)stride;
                d->header.reserved_2 = 0;
                d->data_size = FACE_UPLOAD_BUF_BYTES;
                d->data = (const uint8_t*)s->buf;
                d->reserved = NULL;
                ensure_custom_thumb(picked, (const uint16_t*)s->buf);
                s_custom_face_count++;
            }
        }
    }

    // If a slot was explicitly picked from gallery, navigate to it.
    if (picked >= 0) {
        for (int i = 0; i < s_custom_face_count; i++) {
            if (s_custom_slot_map[i] == picked) {
                int new_idx = FACE_BUILTIN_COUNT + i;
                s_face_index = (face_index_t)new_idx;
                s_sel_pos    = (float)new_idx;
                s_sel_visual_pos = s_sel_pos;
                break;
            }
        }
    }

    s_sel_pos        = wrap_face_pos_(s_sel_pos);
    s_sel_visual_pos = wrap_face_pos_(s_sel_visual_pos);
}

static inline bool face_is_custom(int idx)
{
    return idx >= FACE_BUILTIN_COUNT && idx < face_new_idx();
}

static int custom_rel_for_face(int idx)
{
    if (!face_is_custom(idx)) return -1;
    int rel = idx - FACE_BUILTIN_COUNT;
    if (rel < 0 || rel >= s_custom_face_count) return -1;
    return rel;
}

static int custom_slot_for_face(int idx)
{
    int rel = custom_rel_for_face(idx);
    if (rel < 0) return -1;
    return s_custom_slot_map[rel];
}

// ── Analog static base (full-size pre-render: bg + ticks + logo, NO hands) ─────
// Built once on on_start(), freed on on_stop(). Reduces per-frame analog cost ~12×.
static lv_draw_buf_t s_base_buf;
static void*         s_base_raw   = NULL;
static bool          s_base_valid = false;

// Freeze current layout into frame-stable copies before every lv_obj_invalidate call.
// Prevents mid-frame tearing caused by partial-render strips seeing different x_off.
static void freeze_frame(void)
{
    s_frame_x_off        = s_draw_x_off;
    s_frame_left_d       = s_left_card_d;
    s_frame_right_d      = s_right_card_d;
    s_frame_left_face    = s_left_card_face;
    s_frame_right_face   = s_right_card_face;
    s_frame_center       = s_face_index;
}
static void lock_current_frame(void)
{
    s_locked_frame.x_off = s_draw_x_off;
    s_locked_frame.left_d = s_left_card_d;
    s_locked_frame.right_d = s_right_card_d;
    s_locked_frame.left_face = s_left_card_face;
    s_locked_frame.right_face = s_right_card_face;
    s_locked_frame.center_face = s_face_index;
    s_locked_frame.visual_pos = s_sel_visual_pos;
}

static void cc_panel_y_anim_cb(void* obj, int32_t v)
{
    if (!obj) return;
    lv_obj_set_y((lv_obj_t*)obj, v);
}

static void cc_close_anim_ready_cb(lv_anim_t* /*a*/)
{
    if (!s_cc_overlay || !s_cc_panel) return;
    lv_obj_add_flag(s_cc_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_cc_panel, -220);
    s_cc_open = false;
}

static void cc_open(void)
{
    if (!s_cc_overlay || !s_cc_panel || s_face_mode == MODE_SELECTOR) return;
    if (s_cc_open) return;

    lv_obj_clear_flag(s_cc_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(s_cc_panel, -220);

    /* CC açılması: 90 ms ease_out_quint. Spring curve overshoot'u kaldırıldı
     * (kullanıcı "yavaş" hissediyordu, overshoot'un settle tail'i geç bitiyor).
     * 90 ms = ~5 frame @ 60 FPS, 30 FPS'e düşse bile 3 frame → snappy. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cc_panel);
    lv_anim_set_values(&a, -220, 0);
    lv_anim_set_time(&a, 90);
    lv_anim_set_path_cb(&a, wear_anim_path_ease_out_quint);
    lv_anim_set_exec_cb(&a, cc_panel_y_anim_cb);
    lv_anim_start(&a);

    s_cc_open = true;
}

static void cc_close(void)
{
    if (!s_cc_overlay || !s_cc_panel) return;
    if (!s_cc_open) return;

    /* CC kapanması: 70 ms — hızlı yukarı snap. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cc_panel);
    lv_anim_set_values(&a, lv_obj_get_y(s_cc_panel), -220);
    lv_anim_set_time(&a, 70);
    lv_anim_set_path_cb(&a, wear_anim_path_ease_out_quint);
    lv_anim_set_exec_cb(&a, cc_panel_y_anim_cb);
    lv_anim_set_completed_cb(&a, cc_close_anim_ready_cb);
    lv_anim_start(&a);
}

/* ─── 1:1 finger tracking for CC swipe-down ──────────────────────────────────
 * Üst kenarda invisible "swipe zone" overlay'i (60 px high). Press+drag DOWN
 * ile CC panel'i parmağı 1:1 takip eder. Release'da pozisyona göre snap:
 *   panel y > -110 (yarıdan fazla açık) → snap-open
 *   panel y < -110                       → snap-close (geri yukarı)
 * Apple Watch'taki "rubber-band" feel: kullanıcı yarıya kadar açıp bırakırsa
 * geri kapanır, yarıdan fazla açarsa kalır.
 *
 * Mevcut gesture-based cc_open() (face_event_cb LV_DIR_BOTTOM) ile çakışmasın
 * diye: zone drag aktive olduğunda s_cc_drag_active=true → gesture handler
 * cc_open()'ı atlar; release'dan sonra 250 ms suppress window.
 */
static void cc_panel_y_anim_cb(void* var, int32_t v);  /* fwd decl, mevcut */

static void cc_snap_open_after_drag(void)
{
    if (!s_cc_panel) return;
    int16_t cur_y = lv_obj_get_y(s_cc_panel);
    lv_anim_delete(s_cc_panel, cc_panel_y_anim_cb);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cc_panel);
    lv_anim_set_values(&a, cur_y, 0);
    /* Snap distance'a göre süre — daha az mesafe = daha kısa anim */
    int16_t dist = -cur_y;            /* 0..220 */
    int32_t dur  = 80 + (dist * 70) / 220;   /* 80-150 ms */
    lv_anim_set_time(&a, dur);
    lv_anim_set_path_cb(&a, wear_anim_path_apple_spring);
    lv_anim_set_exec_cb(&a, cc_panel_y_anim_cb);
    lv_anim_start(&a);
    s_cc_open = true;
}

static void cc_snap_close_after_drag(void)
{
    if (!s_cc_panel) return;
    int16_t cur_y = lv_obj_get_y(s_cc_panel);
    lv_anim_delete(s_cc_panel, cc_panel_y_anim_cb);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cc_panel);
    lv_anim_set_values(&a, cur_y, -220);
    int16_t dist = cur_y - (-220);    /* 0..220 */
    int32_t dur  = 80 + (dist * 70) / 220;
    lv_anim_set_time(&a, dur);
    lv_anim_set_path_cb(&a, wear_anim_path_ease_out_quint);
    lv_anim_set_exec_cb(&a, cc_panel_y_anim_cb);
    lv_anim_set_completed_cb(&a, cc_close_anim_ready_cb);
    lv_anim_start(&a);
}

/* v1.4.55: cc_swipe_zone_cb kaldırıldı — bkz s_cc_drag_active üstündeki
 * yorum (audit orta #5a). 1:1 finger tracking için yaratılmıştı ama
 * ESP32-S3 60 FPS tutamadığı için stub'a düşmüştü; CLICKABLE flag ile
 * ekranın üst 60 px'i ölü zon olmuştu. Tüm cc_swipe_zone yapısı silindi. */
static void cc_overlay_click_cb(lv_event_t* e)
{
    lv_obj_t* t = (lv_obj_t*)lv_event_get_target(e);
    if (t == s_cc_overlay) {
        cc_close();
    }
}

static void cc_overlay_gesture_cb(lv_event_t* /*e*/)
{
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        cc_close();
    }
}

static const char* event_target_name(lv_obj_t* t)
{
    if (!t) return "null";
    if (t == s_face) return "face";
    if (t == s_digital_layer) return "digital_layer";
    if (t == s_digital_top) return "digital_top";
    if (t == s_digital_bottom) return "digital_bottom";
    if (t == s_activity_layer) return "activity_layer";
    if (t == s_activity_touch_layer) return "activity_touch";
    if (t == s_activity_msg_badge) return "activity_badge";
    if (t == s_activity_notif_dot) return "activity_dot";
    return "child";
}

static const char* selector_face_name(int idx)
{
    if (face_is_custom(idx)) return "Photo";
    switch (idx) {
        case FACE_ANALOG:     return "Analog";
        case FACE_DIGITAL:    return "Digital";
        case FACE_ACTIVITY:   return "Activity";
        case FACE_IO_CONTROL: return "Outputs";    /* v1.2.1: yeni built-in face */
        default:
            if (idx == face_new_idx()) return "New";
            return "Face";
    }
}

static void selector_edit_btn_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h)
{
    float scale = s_draw_scale;
    if (scale < SEL_SCALE_MIN) scale = SEL_SCALE_MIN;
    if (scale > SEL_SCALE_MAX) scale = SEL_SCALE_MAX;
    int32_t x_off = (int32_t)roundf(s_draw_x_off);

    int32_t dw = (int32_t)roundf((float)SCR_W * scale);
    int32_t dh = (int32_t)roundf((float)SCR_H * scale);
    int32_t dy = (SCR_H - dh) / 2 + (int32_t)roundf(s_draw_y_off);

    *w = (int32_t)roundf(240.0f * scale);
    *h = (int32_t)roundf(65.0f * scale);
    *x = (SCR_W - *w) / 2 + x_off;
    *y = dy + dh + (int32_t)roundf(35.0f * scale);
    if (*y + *h > SCR_H - 10) *y = SCR_H - 10 - *h;
}

static bool point_in_rect_(int32_t x, int32_t y, int32_t rx, int32_t ry, int32_t rw, int32_t rh)
{
    return x >= rx && x <= (rx + rw) && y >= ry && y <= (ry + rh);
}

static int32_t selector_delete_lift_px(float scale)
{
    int32_t dh = (int32_t)roundf((float)SCR_H * scale);
    int32_t yoff = (int32_t)roundf(s_draw_y_off);
    int32_t need = (SCR_H + dh) / 2 + yoff;
    return need + (int32_t)roundf(26.0f * scale);
}

static void selector_trash_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h)
{
    float scale = s_draw_scale;
    if (scale < SEL_SCALE_MIN) scale = SEL_SCALE_MIN;
    if (scale > SEL_SCALE_MAX) scale = SEL_SCALE_MAX;
    int32_t x_off = (int32_t)roundf(s_draw_x_off);

    int32_t dw = (int32_t)roundf((float)SCR_W * scale);
    int32_t dh = (int32_t)roundf((float)SCR_H * scale);
    int32_t dx = (SCR_W - dw) / 2 + x_off;
    int32_t dy = (SCR_H - dh) / 2 + (int32_t)roundf(s_draw_y_off);

    int32_t lift = (int32_t)roundf((float)selector_delete_lift_px(scale) * (s_sel_delete_mode ? 1.0f : s_sel_delete_reveal));
    int32_t center_x = dx + dw / 2;
    int32_t center_y = SCR_H / 2 + (int32_t)roundf(38.0f * scale);

    *w = (int32_t)roundf(150.0f * scale);
    *h = (int32_t)roundf(110.0f * scale);
    *x = center_x - *w / 2;
    *y = center_y - *h / 2;
}

static void selector_remove_current_custom(void)
{
    if (!face_is_custom((int)s_face_index)) return;

    int rel = (int)s_face_index - FACE_BUILTIN_COUNT;
    if (rel < 0 || rel >= s_custom_face_count) return;

    int removed_slot = s_custom_slot_map[rel];
    if (removed_slot >= 0 && removed_slot < FACE_UPLOAD_MAX_SLOTS) {
        s_custom_hidden_slot[removed_slot] = true;
        gallery_service_hide_slot(removed_slot);
    }

    for (int i = rel; i < s_custom_face_count - 1; i++) {
        s_custom_slot_map[i] = s_custom_slot_map[i + 1];
        s_custom_buf_obj[i] = s_custom_buf_obj[i + 1];
    }
    s_custom_face_count--;
    s_custom_slot_map[s_custom_face_count] = -1;

    int new_idx;
    if (s_custom_face_count <= 0) {
        new_idx = face_new_idx();
    } else if (rel < s_custom_face_count) {
        new_idx = FACE_BUILTIN_COUNT + rel;
    } else {
        new_idx = FACE_BUILTIN_COUNT + s_custom_face_count - 1;
    }

    s_face_index = (face_index_t)new_idx;
    s_sel_pos = (float)new_idx;
    s_sel_visual_pos = s_sel_pos;
    s_sel_delete_mode = false;
}

static void update_selector_ui(void)
{
    if (!s_selector_border || !s_selector_title || !s_selector_edit_btn || !s_selector_edit_label)
        return;

    if (s_face_mode != MODE_SELECTOR)
    {
        lv_obj_add_flag(s_selector_border, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_selector_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_selector_edit_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_selector_side_left)
            lv_obj_add_flag(s_selector_side_left, LV_OBJ_FLAG_HIDDEN);
        if (s_selector_side_right)
            lv_obj_add_flag(s_selector_side_right, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    float scale = s_draw_scale;
    if (scale < SEL_SCALE_MIN)
        scale = SEL_SCALE_MIN;
    if (scale > SEL_SCALE_MAX)
        scale = SEL_SCALE_MAX;
    int32_t x_off = (int32_t)roundf(s_draw_x_off);

    int32_t dw = (int32_t)roundf((float)SCR_W * scale);
    int32_t dh = (int32_t)roundf((float)SCR_H * scale);
    int32_t dx = (SCR_W - dw) / 2 + x_off;
    int32_t dy = (SCR_H - dh) / 2 + (int32_t)roundf(s_draw_y_off);

    if (s_selector_side_left && s_selector_side_right)
    {
        float nearest_left = -99.0f;
        float nearest_right = 99.0f;
        for (int i = 0; i < carousel_size(); i++)
        {
            float d = shortest_delta_((float)i, s_sel_visual_pos);
            if (fabsf(d) < 0.05f)
                continue;
            if (d < 0.0f && d > nearest_left) {
                nearest_left = d;
                s_left_card_face = (face_index_t)i;
            }
            if (d > 0.0f && d < nearest_right) {
                nearest_right = d;
                s_right_card_face = (face_index_t)i;
            }
        }
        s_left_card_d  = nearest_left;
        s_right_card_d = nearest_right;

        auto place_side = [&](lv_obj_t *obj, float d_cards)
        {
            if (!obj)
                return;
            if (fabsf(d_cards) > 1.5f)
            {
                lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            // All cards same scale — no deformation
            float s   = SEL_CARD_SCALE;
            int32_t sw = (int32_t)roundf((float)SCR_W * s);
            int32_t sh = (int32_t)roundf((float)SCR_H * s);
            int32_t cy_card = SCR_H / 2 + (int32_t)roundf(s_draw_y_off);
            int32_t sy = cy_card - sh / 2;

            // Card center on the horizontal track — same formula as face_draw_cb side thumbnails
            int32_t cx = SCR_W / 2 + (int32_t)roundf(d_cards * SEL_CARD_SPACING);
            int32_t sx_full = cx - sw / 2;
            int32_t ex_full = sx_full + sw;

            // No clipping — screen boundary clips naturally
            int32_t x1 = sx_full;
            int32_t x2 = ex_full;

            // Object is transparent — we only use it to track visibility/position for d values
            // Actual rendering is in face_draw_cb via draw_selector_side_thumb
            int32_t pw = x2 - x1;
            if (pw < 4) {
                lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            lv_obj_set_pos(obj, x1, sy);
            lv_obj_set_size(obj, pw, sh);
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_background(obj);
        };

        place_side(s_selector_side_left, nearest_left);
        place_side(s_selector_side_right, nearest_right);
    }

    // Ana kart çerçeve ayarları
    int32_t border_pad = (int32_t)roundf(10.0f * scale);
    lv_obj_set_pos(s_selector_border, dx - border_pad, dy - border_pad);
    lv_obj_set_size(s_selector_border, dw + border_pad * 2, dh + border_pad * 2);
    lv_obj_set_style_radius(s_selector_border, (int32_t)roundf(80.0f * scale), 0);

    // Başlık ve Buton Pozisyonları
    lv_label_set_text(s_selector_title, selector_face_name((int)s_face_index));
    bool delete_ui = s_sel_delete_mode || (s_sel_delete_reveal > 0.01f);
    lv_label_set_text(s_selector_edit_label, delete_ui ? "Cancel" : "Edit");

    // Hide edit button only on FACE_NEW (unless delete mode)
    if (!delete_ui && (int)s_face_index == face_new_idx()) {
        lv_obj_add_flag(s_selector_edit_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_selector_edit_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_update_layout(s_selector_title);
    int32_t title_y = dy - (int32_t)roundf(70.0f * scale);
    if (title_y < 8)
        title_y = 8;
    lv_obj_set_pos(s_selector_title, (SCR_W - lv_obj_get_width(s_selector_title)) / 2 + x_off, title_y);

#ifdef BOARD_VARIANT_LCD128
    /* 240 ekranda 240×65 pill oransal kocaman — küçültüldü. */
    int32_t btn_w = (int32_t)roundf(150.0f * scale);
    int32_t btn_h = (int32_t)roundf(46.0f * scale);
#else
    int32_t btn_w = (int32_t)roundf(240.0f * scale);
    int32_t btn_h = (int32_t)roundf(65.0f * scale);
#endif
    int32_t btn_x = (SCR_W - btn_w) / 2 + x_off;
    int32_t btn_y = dy + dh + (int32_t)roundf(35.0f * scale);
    if (btn_y + btn_h > SCR_H - 10)
        btn_y = SCR_H - 10 - btn_h;

    lv_obj_set_pos(s_selector_edit_btn, btn_x, btn_y);
    lv_obj_set_size(s_selector_edit_btn, btn_w, btn_h);

    lv_obj_update_layout(s_selector_edit_label);
    lv_obj_set_pos(s_selector_edit_label, (btn_w - lv_obj_get_width(s_selector_edit_label)) / 2, (btn_h - lv_obj_get_height(s_selector_edit_label)) / 2);

    lv_obj_move_foreground(s_selector_border);
    lv_obj_move_foreground(s_selector_title);
    lv_obj_move_foreground(s_selector_edit_btn);

    // Center border is drawn in face_draw_cb for perfect sync with card motion.
    // Keep object border hidden to avoid double/stacked outlines.
    lv_obj_add_flag(s_selector_border, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_selector_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_selector_edit_btn, LV_OBJ_FLAG_HIDDEN);
}

static void enable_event_bubble_recursive(lv_obj_t* obj)
{
    if (!obj) return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        enable_event_bubble_recursive(lv_obj_get_child(obj, i));
    }
}

// ─── Polar → Cartesian (screen-adaptive center) ───────────────────────────────
// angle = 0 → 12 o'clock, clockwise positive
static inline int32_t px_(float a, float r, int32_t cx)
{
    return (int32_t)roundf((float)cx + r * sinf(a));
}
static inline int32_t py_(float a, float r, int32_t cy)
{
    return (int32_t)roundf((float)cy - r * cosf(a));
}

// ─── Rounded-rect perimeter positioning ──────────────────────────────────────
/**
 * Given distance `d` (px) clockwise from 12 o'clock along the rounded-rect
 * perimeter at `margin` px inside the screen edge, returns:
 *   (ox, oy)  — the outer tick point
 *   (nx, ny)  — inward unit normal at that point
 *
 * Segments (clockwise from top-center):
 *   0  top-right half  (W/2-R long, right along top)
 *   1  top-right arc   (quarter circle)
 *   2  right edge      (H-2R long, downward)
 *   3  bottom-right arc
 *   4  bottom edge     (W-2R long, leftward)
 *   5  bottom-left arc
 *   6  left edge       (H-2R long, upward)
 *   7  top-left arc
 *   8  top-left half   (W/2-R long, right to center)
 */
static void perimeter_pos(float d, float margin,
                           float w, float h, float corner_r,
                           float* ox, float* oy,
                           float* nx, float* ny)
{
    const float W  = w;
    const float H  = h;
    const float R  = corner_r;
    const float Rm = R - margin;   /* effective arc radius */

    float seg[9];
    seg[0] = W / 2.0f - R;                  /* top-right half  */
    seg[1] = Rm * (float)M_PI / 2.0f;       /* TR arc          */
    seg[2] = H - 2.0f * R;                  /* right edge      */
    seg[3] = Rm * (float)M_PI / 2.0f;       /* BR arc          */
    seg[4] = W - 2.0f * R;                  /* bottom edge     */
    seg[5] = Rm * (float)M_PI / 2.0f;       /* BL arc          */
    seg[6] = H - 2.0f * R;                  /* left edge       */
    seg[7] = Rm * (float)M_PI / 2.0f;       /* TL arc          */
    seg[8] = W / 2.0f - R;                  /* top-left half   */

    float total = 0;
    for (int i = 0; i < 9; i++) total += seg[i];

    d = fmodf(d, total);
    if (d < 0) d += total;

    float cum = 0;
    for (int s = 0; s < 9; s++) {
        float end = cum + seg[s];
        if (d < end + 0.001f) {
            float t = d - cum;
            float f = (seg[s] > 0.001f) ? (t / seg[s]) : 0.0f;
            switch (s) {
                case 0:  /* top-right half, y = margin, going right */
                     *ox = W / 2.0f + t;
                     *oy = margin;
                     *nx = 0; *ny = 1;
                     break;
                case 1: {  /* TR arc, center (W-R, R) */
                    float ang = -(float)M_PI / 2.0f + f * (float)M_PI / 2.0f;
                    float cx = W - R, cy = R;
                    *ox = cx + Rm * cosf(ang);
                    *oy = cy + Rm * sinf(ang);
                    *nx = -(*ox - cx) / Rm;
                    *ny = -(*oy - cy) / Rm;
                    break;
                }
                case 2:  /* right edge, x = W-margin, going down */
                     *ox = W - margin;
                     *oy = R + t;
                     *nx = -1; *ny = 0;
                     break;
                case 3: {  /* BR arc, center (W-R, H-R) */
                    float ang = f * (float)M_PI / 2.0f;
                    float cx = W - R, cy = H - R;
                    *ox = cx + Rm * cosf(ang);
                    *oy = cy + Rm * sinf(ang);
                    *nx = -(*ox - cx) / Rm;
                    *ny = -(*oy - cy) / Rm;
                    break;
                }
                case 4:  /* bottom edge, y = H-margin, going left */
                     *ox = (W - R) - t;
                     *oy = H - margin;
                     *nx = 0; *ny = -1;
                     break;
                case 5: {  /* BL arc, center (R, H-R) */
                    float ang = (float)M_PI / 2.0f + f * (float)M_PI / 2.0f;
                    float cx = R, cy = H - R;
                    *ox = cx + Rm * cosf(ang);
                    *oy = cy + Rm * sinf(ang);
                    *nx = -(*ox - cx) / Rm;
                    *ny = -(*oy - cy) / Rm;
                    break;
                }
                case 6:  /* left edge, x = margin, going up */
                     *ox = margin;
                     *oy = (H - R) - t;
                     *nx = 1; *ny = 0;
                     break;
                case 7: {  /* TL arc, center (R, R) */
                    float ang = (float)M_PI + f * (float)M_PI / 2.0f;
                    float cx = R, cy = R;
                    *ox = cx + Rm * cosf(ang);
                    *oy = cy + Rm * sinf(ang);
                    *nx = -(*ox - cx) / Rm;
                    *ny = -(*oy - cy) / Rm;
                    break;
                }
                case 8:  /* top-left half, y = margin, going right */
                     *ox = R + t;
                     *oy = margin;
                     *nx = 0; *ny = 1;
                     break;
                default:
                    *ox = W / 2.0f; *oy = margin; *nx = 0; *ny = 1;
                    break;
            }
            return;
        }
        cum = end;
    }
    /* fallback: 12 o'clock */
    *ox = W / 2.0f;
    *oy = margin;
    *nx = 0; *ny = 1;
}

static inline float clampf_(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int wrap_face_idx_(int idx)
{
    int n = carousel_size();
    if (idx < 0) return 0;
    if (idx >= n) return n - 1;
    return idx;
}

static inline float wrap_face_pos_(float pos)
{
    float n = (float)carousel_size();
    if (pos < 0.0f) return 0.0f;
    float hi = n - 1.0f;
    if (pos > hi) return hi;
    return pos;
}

static inline float shortest_delta_(float target, float from)
{
    return target - from;
}

static inline int nearest_face_idx_(float pos)
{
    return wrap_face_idx_((int)lroundf(pos));
}

static void selector_apply_pose(void)
{
    if (s_face_mode != MODE_SELECTOR) return;

    s_sel_visual_pos = wrap_face_pos_(s_sel_visual_pos);
    int center_idx = nearest_face_idx_(s_sel_visual_pos);
    if ((int)s_face_index != center_idx) {
        set_face_index((face_index_t)center_idx);
    }

    float center_delta = shortest_delta_((float)center_idx, s_sel_visual_pos);
    s_draw_scale = SEL_CARD_SCALE;   // all cards same scale — no deformation
    s_draw_y_off = -10.0f;           // fixed vertical offset (card centered slightly above mid)
    s_draw_x_off = center_delta * SEL_CARD_SPACING;

    /* Selector mode'da digital_layer / activity_layer LV_OBJ_FLAG_HIDDEN — bu
     * widget'ları her tick güncellemek (60 Hz) tamamen kayıp CPU'ydu (~150 µs/tick
     * gettimeofday + tm_min karşılaştırma + style/text setterler). Her 16 ms tick'te
     * fazladan yapılan iş anim_timer_cb cadence'ını jitterlatıp "dalgalanma" hissi
     * yaratıyordu. Snapshot'lar build_face_snapshot() ile bir kere oluşturuluyor;
     * selector exit'te update_digital_labels/update_activity_labels normal mode için
     * tek seferlik çağrılıyor (exit_selector). */
    update_selector_ui();
}

static void init_logo_metrics(void)
{
    if (s_logo_metrics_ready) return;
    const lv_image_dsc_t* img = &corezzle_logo_a8_140;
    int w = (int)img->header.w;
    int h = (int)img->header.h;
    if (w <= 0 || h <= 0 || img->data == NULL) {
        s_logo_center_x = 0;
        s_logo_min_y = 0;
        s_logo_metrics_ready = true;
        return;
    }

    int min_x = w;
    int max_x = -1;
    int min_y = h;
    int max_y = -1;
    const uint8_t* data = (const uint8_t*)img->data;
    for (int y = 0; y < h; y++) {
        int row = y * w;
        for (int x = 0; x < w; x++) {
            if (data[row + x] != 0) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    if (max_x < 0 || max_y < 0) {
        s_logo_center_x = w / 2;
        s_logo_min_y = 0;
    } else {
        s_logo_center_x = (min_x + max_x) / 2;
        s_logo_min_y = min_y;
    }
    s_logo_metrics_ready = true;
}

/**
 * Manually blit the A8 corezzle logo into an RGB565 buffer.
 *
 * Rationale: LVGL 9's A8 image decoder allocates a full-size RAM copy
 * (24.5 KB for 140×175) which fails when regular heap is fragmented —
 *   "lv_draw_buf_create_ex: No memory ... decode_alpha_only: Out of memory"
 * Since the logo always sits on a black background with white recolor,
 * A8 alpha maps directly to a grayscale RGB565 pixel. We bypass the
 * decoder entirely and write straight into the PSRAM base buffer.
 *
 * @param dst_raw     base-of-buffer byte pointer (RGB565)
 * @param dst_stride  bytes per row (from lv_draw_buf_width_to_stride)
 * @param dst_w/dst_h buffer dimensions in pixels
 * @param dst_x/dst_y top-left destination position (may be negative)
 */
static void blit_corezzle_logo_rgb565(void*   dst_raw,
                                      uint32_t dst_stride,
                                      int32_t  dst_w,
                                      int32_t  dst_h,
                                      int32_t  dst_x,
                                      int32_t  dst_y)
{
    if (!dst_raw) return;
    const lv_image_dsc_t* img = &corezzle_logo_a8_140;
    if (!img->data) return;

    const int32_t  logo_w     = (int32_t)img->header.w;
    const int32_t  logo_h     = (int32_t)img->header.h;
    const uint32_t src_stride = img->header.stride ? img->header.stride
                                                   : (uint32_t)logo_w;
    const uint8_t* src        = (const uint8_t*)img->data;

#ifdef BOARD_VARIANT_LCD128
    /* 240×240 ekranda 140px'lik asset merkeze/tick'lere taşıyor —
     * LOGO_SCALE_256/256 oranında nearest-neighbor küçültme (cihaz testi
     * 2026-07-13: %50 fazla küçük bulundu → %62'ye çıkarıldı). Sadece bu
     * varyantta; AMOLED aşağıdaki orijinal 1:1 döngüyü kullanır. */
    const int32_t SC = LOGO_SCALE_256;
    const int32_t dw = (logo_w * SC) >> 8;
    const int32_t dh = (logo_h * SC) >> 8;
    for (int32_t y2 = 0; y2 < dh; y2++) {
        int32_t dy = dst_y + y2;
        if (dy < 0)       continue;
        if (dy >= dst_h)  break;

        int32_t src_y = (y2 << 8) / SC;
        if (src_y >= logo_h) break;
        const uint8_t* src_row = src + (uint32_t)src_y * src_stride;
        uint16_t*      dst_row = (uint16_t*)((uint8_t*)dst_raw + (uint32_t)dy * dst_stride);

        for (int32_t x2 = 0; x2 < dw; x2++) {
            int32_t src_x = (x2 << 8) / SC;
            if (src_x >= logo_w) break;
            uint8_t a = src_row[src_x];
            if (a == 0) continue;

            int32_t dx = dst_x + x2;
            if (dx < 0)      continue;
            if (dx >= dst_w) break;

            uint16_t r5 = (uint16_t)(a >> 3);
            uint16_t g6 = (uint16_t)(a >> 2);
            uint16_t b5 = (uint16_t)(a >> 3);
            dst_row[dx] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
#else
    for (int32_t y = 0; y < logo_h; y++) {
        int32_t dy = dst_y + y;
        if (dy < 0)       continue;
        if (dy >= dst_h)  break;

        const uint8_t* src_row = src + (uint32_t)y * src_stride;
        uint16_t*      dst_row = (uint16_t*)((uint8_t*)dst_raw + (uint32_t)dy * dst_stride);

        for (int32_t x = 0; x < logo_w; x++) {
            uint8_t a = src_row[x];
            if (a == 0) continue;                 /* transparent — keep base pixel */

            int32_t dx = dst_x + x;
            if (dx < 0)      continue;
            if (dx >= dst_w) break;

            /* White recolor on black BG → grayscale scaled by alpha */
            uint16_t r5 = (uint16_t)(a >> 3);     /* 5-bit */
            uint16_t g6 = (uint16_t)(a >> 2);     /* 6-bit */
            uint16_t b5 = (uint16_t)(a >> 3);     /* 5-bit */
            dst_row[dx] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
#endif /* BOARD_VARIANT_LCD128 */
}

// ─── Perimeter total length ───────────────────────────────────────────────────
static float perim_total(float w, float h, float margin, float corner_r)
{
    const float W  = w;
    const float H  = h;
    const float R  = corner_r;
    const float Rm = R - margin;
    return 2.0f * (W - 2.0f * R) + 2.0f * (H - 2.0f * R)
           + 4.0f * Rm * (float)M_PI / 2.0f;
}

// ─── Draw helpers ─────────────────────────────────────────────────────────────

static void draw_tick(lv_layer_t *layer, int i, float w, float h,
                      float margin, float corner_r,
                      float tick_len_hr, float tick_len_min,
                      int tick_w_hr, int tick_w_min,
                      float ox0, float oy0)
{
    bool is_hr = (i % 10 == 0); // Her 10 çizgide bir saat başı
    float total = perim_total(w, h, margin, corner_r);
    float d = (float)i / 120.0f * total;

    float ox, oy, nx, ny;
    perimeter_pos(d, margin, w, h, corner_r, &ox, &oy, &nx, &ny);

    // --- MERKEZE YÖNLENDİRME (Tüm çizgiler için) ---
    float cx_f = w / 2.0f;
    float cy_f = h / 2.0f;
    bool is_special_hr = (i == 0 || i == 30 || i == 60 || i == 90);

    if (!is_special_hr)
    {
        float dx = cx_f - ox;
        float dy = cy_f - oy;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist > 0.1f)
        {
            nx = dx / dist;
            ny = dy / dist;
        }
    }

    // 1. KATMAN: ARKA PLANDAKİ İNCE GRİ ÇİZGİ (Hepsinde çizilir)
    lv_draw_line_dsc_t thin_dsc;
    lv_draw_line_dsc_init(&thin_dsc);
    thin_dsc.color = lv_color_make(0x55, 0x55, 0x55); // Biraz daha koyu gri
    thin_dsc.opa = 130;
    thin_dsc.width = tick_w_min;
    thin_dsc.round_start = 1;
    thin_dsc.round_end = 1;
    thin_dsc.p1.x = (lv_value_precise_t)(ox + ox0);
    thin_dsc.p1.y = (lv_value_precise_t)(oy + oy0);
    thin_dsc.p2.x = (lv_value_precise_t)(ox + nx * tick_len_min + ox0);
    thin_dsc.p2.y = (lv_value_precise_t)(oy + ny * tick_len_min + oy0);
    lv_draw_line(layer, &thin_dsc);

    // 2. KATMAN: ÖNDEKİ KALIN BEYAZ KAPSÜL (Sadece saat başlarında)
    if (is_hr)
    {
        float start_offset = 0;
        float current_tlen = tick_len_hr;

        if (i == 0)
        { // Saat 12 için özel ayar
            start_offset = 22.0f;
            current_tlen = tick_len_hr * 0.6f;
        }

        lv_draw_line_dsc_t thick_dsc;
        lv_draw_line_dsc_init(&thick_dsc);
        thick_dsc.color = lv_color_make(0xD1, 0xD1, 0xD1);
        thick_dsc.opa = LV_OPA_COVER;
        thick_dsc.width = tick_w_hr; // 10px genişlik
        thick_dsc.round_start = 1;   // Tam yuvarlak başlangıç
        thick_dsc.round_end = 1;     // Tam yuvarlak bitiş (Pill shape)

        thick_dsc.p1.x = (lv_value_precise_t)(ox + (nx * start_offset) + ox0);
        thick_dsc.p1.y = (lv_value_precise_t)(oy + (ny * start_offset) + oy0);
        thick_dsc.p2.x = (lv_value_precise_t)(ox + (nx * (start_offset + current_tlen)) + ox0);
        thick_dsc.p2.y = (lv_value_precise_t)(oy + (ny * (start_offset + current_tlen)) + oy0);
        lv_draw_line(layer, &thick_dsc);
    }
}

/* Outlined pill hand */
static void draw_hand_pill(lv_layer_t *layer,
                           float angle,
                           int32_t tip,
                           int32_t w_out,
                           int32_t w_in,
                           int32_t cx, int32_t cy)
{
    // Sapın uzunluğu (Merkezden kapsülün başlangıcına kadar olan mesafe)
    int32_t stem_len = tip / 4;

    // 1. İNCE SAP (Merkeze bağlanan beyaz çizgi)
    lv_draw_line_dsc_t stem_dsc;
    lv_draw_line_dsc_init(&stem_dsc);
    stem_dsc.color = lv_color_white(); // Parantezler eklendi
    stem_dsc.width = 2;                // Sap her zaman ince kalmalı
    stem_dsc.round_start = 1;
    stem_dsc.p1.x = cx;
    stem_dsc.p1.y = cy;
    stem_dsc.p2.x = (lv_value_precise_t)px_(angle, (float)stem_len, cx);
    stem_dsc.p2.y = (lv_value_precise_t)py_(angle, (float)stem_len, cy);
    lv_draw_line(layer, &stem_dsc);

    // 2. DIŞ KAPSÜL (Beyaz Çerçeve)
    lv_draw_line_dsc_t outer;
    lv_draw_line_dsc_init(&outer);
    outer.color = lv_color_white();
    outer.width = w_out;
    outer.round_start = 1;
    outer.round_end = 1;
    outer.p1.x = (lv_value_precise_t)px_(angle, (float)stem_len, cx);
    outer.p1.y = (lv_value_precise_t)py_(angle, (float)stem_len, cy);
    outer.p2.x = (lv_value_precise_t)px_(angle, (float)tip, cx);
    outer.p2.y = (lv_value_precise_t)py_(angle, (float)tip, cy);
    lv_draw_line(layer, &outer);

    // 3. İÇ BOŞLUK (Siyah Dolgu)
    lv_draw_line_dsc_t inner = outer;
    inner.color = lv_color_black(); // Parantezler eklendi
    inner.width = w_in;
    // İç kısmın uçlarının beyazın içinde kalması için %10 ofset
    float offset = (float)(w_out - w_in) * 0.5f;
    inner.p1.x = (lv_value_precise_t)px_(angle, (float)stem_len + offset, cx);
    inner.p1.y = (lv_value_precise_t)py_(angle, (float)stem_len + offset, cy);
    inner.p2.x = (lv_value_precise_t)px_(angle, (float)tip - offset, cx);
    inner.p2.y = (lv_value_precise_t)py_(angle, (float)tip - offset, cy);
    lv_draw_line(layer, &inner);
}

/* Filled circle */
static void draw_circle(lv_layer_t* layer, int32_t cx, int32_t cy,
                         int32_t r, lv_color_t col)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color     = col;
    dsc.bg_opa       = LV_OPA_COVER;
    dsc.radius       = LV_RADIUS_CIRCLE;
    dsc.border_width = 0;
    dsc.shadow_width = 0;
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };
    lv_draw_rect(layer, &dsc, &a);
}

static void draw_line_segment(lv_layer_t* layer,
                              int32_t x1, int32_t y1,
                              int32_t x2, int32_t y2,
                              int width,
                              lv_color_t color,
                              lv_opa_t opa)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = opa;
    dsc.width = width < 1 ? 1 : width;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

static void draw_arc_segment(lv_layer_t* layer,
                             int32_t cx, int32_t cy,
                             float radius,
                             int width,
                             float start_deg,
                             float end_deg,
                             lv_color_t color,
                             lv_opa_t opa)
{
    if (radius <= 1.0f) return;
    float span = end_deg - start_deg;
    if (span < 0.0f) span += 360.0f;
    if (span <= 0.01f) return;

    int segs = (int)roundf(span / 4.0f);
    if (segs < 8) segs = 8;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = opa;
    dsc.width = width < 1 ? 1 : width;
    dsc.round_start = 1;
    dsc.round_end = 1;

    for (int i = 0; i < segs; i++) {
        float a0 = (start_deg + span * ((float)i / (float)segs)) * (float)M_PI / 180.0f;
        float a1 = (start_deg + span * ((float)(i + 1) / (float)segs)) * (float)M_PI / 180.0f;
        dsc.p1.x = (lv_value_precise_t)px_(a0, radius, cx);
        dsc.p1.y = (lv_value_precise_t)py_(a0, radius, cy);
        dsc.p2.x = (lv_value_precise_t)px_(a1, radius, cx);
        dsc.p2.y = (lv_value_precise_t)py_(a1, radius, cy);
        lv_draw_line(layer, &dsc);
    }
}

static void update_digital_labels(const struct tm *ti)
{
    if (!s_digital_layer || !s_digital_hour_tens_top || !s_digital_hour_ones_top ||
        !s_digital_min_tens_bot || !s_digital_min_ones_bot)
        return;

    int hour = ti->tm_hour;
    int min = ti->tm_min;
    float scale = (s_face_mode == MODE_SELECTOR) ? s_draw_scale : 1.0f;

    /* TITREME FIX (kullanıcı raporu 2026-05-04):
     * Bu fonksiyon anim_timer_cb tarafından her 30 ms çağrılıyor → her tick
     * lv_obj_set_parent + set_style_text_font + set_style_text_color +
     * lv_label_set_text + lv_obj_update_layout × 4 widget = >20 invalidate
     * çağrısı saniyede 33 kere → label antialiasing edge'leri her render'da
     * subtle farklı pikseller üretiyor = visible flicker.
     *
     * Çözüm: state caching. hour/min/face_mode/scale aynıysa → tüm setter'ları
     * skip et. Sadece bir değer değişince re-apply. Layout iş yükü %95+ düşer,
     * widget'lar dirty mark almaz, AA stabil. */
    static int      last_hour = -1, last_min = -1;
    static face_mode_t last_face_mode = (face_mode_t)-1;
    static float    last_scale = -1.0f;
    bool full_refresh = (last_face_mode != s_face_mode) ||
                        (fabsf(last_scale - scale) > 0.001f);
    bool hour_changed = full_refresh || (last_hour != hour);
    bool min_changed  = full_refresh || (last_min  != min);

    if (!hour_changed && !min_changed) {
        return;   /* Hiçbir şey değişmedi → erken çıkış, hiç widget dokunma */
    }

    // --- RENKLER ---
    lv_color_t hour_color = lv_color_hex(0xECEB38); // #eceb38 (Üst Saat)
    lv_color_t min_color = lv_color_hex(0xF6BAC5);  // #f6bac5 (Alt Dakika)

    /* Re-parent + font + color SADECE full_refresh'te (face_mode/scale değişti).
     * Normal tick'lerde (sadece minute değişti) bunlar zaten güncel. */
    if (full_refresh) {
        lv_obj_set_parent(s_digital_hour_tens_top, s_digital_layer);
        lv_obj_set_parent(s_digital_hour_ones_top, s_digital_layer);
        lv_obj_set_parent(s_digital_min_tens_bot, s_digital_layer);
        lv_obj_set_parent(s_digital_min_ones_bot, s_digital_layer);

        const lv_font_t *main_font = (s_face_mode == MODE_SELECTOR)
                                     ? DGT_FONT_SEL
                                     : DGT_FONT_MAIN;
        lv_obj_set_style_text_font(s_digital_hour_tens_top, main_font, 0);
        lv_obj_set_style_text_color(s_digital_hour_tens_top, hour_color, 0);
        lv_obj_set_style_text_font(s_digital_hour_ones_top, main_font, 0);
        lv_obj_set_style_text_color(s_digital_hour_ones_top, hour_color, 0);
        lv_obj_set_style_text_font(s_digital_min_tens_bot, main_font, 0);
        lv_obj_set_style_text_color(s_digital_min_tens_bot, min_color, 0);
        lv_obj_set_style_text_font(s_digital_min_ones_bot, main_font, 0);
        lv_obj_set_style_text_color(s_digital_min_ones_bot, min_color, 0);
    }

    /* Sadece değişen rakam metinlerini güncelle. */
    char buf[2] = {0, 0};
    if (hour_changed) {
        buf[0] = (char)('0' + (hour / 10));
        lv_label_set_text(s_digital_hour_tens_top, buf);
        buf[0] = (char)('0' + (hour % 10));
        lv_label_set_text(s_digital_hour_ones_top, buf);
    }
    if (min_changed) {
        buf[0] = (char)('0' + (min / 10));
        lv_label_set_text(s_digital_min_tens_bot, buf);
        buf[0] = (char)('0' + (min % 10));
        lv_label_set_text(s_digital_min_ones_bot, buf);
    }

    /* Layout: digit text değiştiğinde widget genişliği değişir (1=ince,
     * 8=geniş), positioning code güncel boyutu okumalı. */
    if (hour_changed) {
        lv_obj_update_layout(s_digital_hour_tens_top);
        lv_obj_update_layout(s_digital_hour_ones_top);
    }
    if (min_changed) {
        lv_obj_update_layout(s_digital_min_tens_bot);
        lv_obj_update_layout(s_digital_min_ones_bot);
    }

    last_hour = hour;
    last_min = min;
    last_face_mode = s_face_mode;
    last_scale = scale;

    // --- HİZALAMA MANTIĞI (ölçüm tabanlı, sıfır sabit-offset) ---
    int32_t x_off = (s_face_mode == MODE_SELECTOR) ? (int32_t)roundf(s_frame_x_off) : 0;
    int32_t cx = SCR_W / 2 + x_off;

    // Gerçek font satır yüksekliğini oku (pad_all=0 sayesinde == line_height)
    int32_t dh = lv_obj_get_height(s_digital_hour_tens_top);

    // Satırlar arası boşluk (piksel)
    int32_t row_gap = (int32_t)(10 * scale);

    // Her iki satırı birlikte dikey ortala
    int32_t total_h = dh * 2 + row_gap;
    int32_t row1_y  = (SCR_H - total_h) / 2;
    int32_t row2_y  = row1_y + dh + row_gap;

    // Yatay: her satırın kendi genişliğini ölç, çifti ortala
    int32_t w_htens = lv_obj_get_width(s_digital_hour_tens_top);
    int32_t w_hones = lv_obj_get_width(s_digital_hour_ones_top);
    int32_t w_mtens = lv_obj_get_width(s_digital_min_tens_bot);
    int32_t w_mones = lv_obj_get_width(s_digital_min_ones_bot);
    int32_t col_gap = (int32_t)(14 * scale);  // rakamlar arası boşluk

    // Saat satırı (HH): cx'e göre ortala
    int32_t h_pair_w = w_htens + col_gap + w_hones;
    int32_t h_col1   = cx - h_pair_w / 2;
    int32_t h_col2   = h_col1 + w_htens + col_gap;

    // Dakika satırı (MM): cx'e göre ortala
    int32_t m_pair_w = w_mtens + col_gap + w_mones;
    int32_t m_col1   = cx - m_pair_w / 2;
    int32_t m_col2   = m_col1 + w_mtens + col_gap;

    // Üst Satır (Saat)
    lv_obj_set_pos(s_digital_hour_tens_top, h_col1, row1_y);
    lv_obj_set_pos(s_digital_hour_ones_top, h_col2, row1_y);

    // Alt Satır (Dakika)
    lv_obj_set_pos(s_digital_min_tens_bot, m_col1, row2_y);
    lv_obj_set_pos(s_digital_min_ones_bot, m_col2, row2_y);

    // Gereksizleri Gizle
    lv_obj_add_flag(s_digital_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_digital_bottom, LV_OBJ_FLAG_HIDDEN);
    if (s_digital_hour_tens_bot)
        lv_obj_add_flag(s_digital_hour_tens_bot, LV_OBJ_FLAG_HIDDEN);
    if (s_digital_hour_ones_bot)
        lv_obj_add_flag(s_digital_hour_ones_bot, LV_OBJ_FLAG_HIDDEN);
    if (s_digital_min_tens_top)
        lv_obj_add_flag(s_digital_min_tens_top, LV_OBJ_FLAG_HIDDEN);
    if (s_digital_min_ones_top)
        lv_obj_add_flag(s_digital_min_ones_top, LV_OBJ_FLAG_HIDDEN);
}

static void update_activity_labels(const struct tm *ti)
{
    if (!s_activity_layer || !s_activity_time)
        return;

    float scale = (s_face_mode == MODE_SELECTOR) ? s_draw_scale : 1.0f;
    int32_t dw = (int32_t)(SCR_W * scale), dh = (int32_t)(SCR_H * scale);
    int32_t x_off = (s_face_mode == MODE_SELECTOR) ? (int32_t)roundf(s_frame_x_off) : 0;
    int32_t dx = (SCR_W - dw) / 2 + x_off, dy = (SCR_H - dh) / 2 + (int32_t)s_draw_y_off;

#ifdef BOARD_VARIANT_LCD128
    /* 240 ekranda 70px Nunito "10:09:30"u ekrandan taşırıyor →
     * rakam+iki nokta only 30px Nunito üretildi (lv_font_conv). */
    const lv_font_t *f_time = (s_face_mode == MODE_SELECTOR) ? &lv_font_montserrat_16 : &lv_font_nunito_extrabold_30;
    const lv_font_t *f_stats = (s_face_mode == MODE_SELECTOR) ? &lv_font_montserrat_16 : &lv_font_nunito_extrabold_30;
#else
    const lv_font_t *f_time = (s_face_mode == MODE_SELECTOR) ? &lv_font_montserrat_24 : &lv_font_nunito_extrabold_70;
    const lv_font_t *f_stats = (s_face_mode == MODE_SELECTOR) ? &lv_font_montserrat_24 : &lv_font_nunito_extrabold_70;
#endif

    /* v1.4.55 audit #1b: font'lar sadece mode değişimde set edilir.
     * Renkler build_activity_layer'da bir kez set edildi → buradan kaldırıldı.
     * Önceki kod her label_set_text'in ardından font+color cascade yapıyordu
     * → her seferinde LVGL invalidate. Mode değişimi seyrek → büyük tasarruf. */
    static face_mode_t s_last_activity_font_mode = (face_mode_t)-1;
    if (s_face_mode != s_last_activity_font_mode) {
        lv_obj_set_style_text_font(s_activity_time, f_time, 0);
        lv_obj_set_style_text_font(s_activity_goal, f_stats, 0);
        lv_obj_set_style_text_font(s_activity_cal,  f_stats, 0);
        lv_obj_set_style_text_font(s_activity_hr,   f_stats, 0);
        s_last_activity_font_mode = s_face_mode;
    }

#ifdef BOARD_VARIANT_LCD128
    int32_t right_margin_x = dx + dw - (int32_t)(30 * scale);
#else
    int32_t right_margin_x = dx + dw - (int32_t)(35 * scale);
#endif

    // Saat
    char ts[20];
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d", ti->tm_hour, ti->tm_min, ti->tm_sec);
    lv_label_set_text(s_activity_time, ts);
    lv_obj_update_layout(s_activity_time);
    lv_obj_set_pos(s_activity_time, right_margin_x - lv_obj_get_width(s_activity_time), dy + (int32_t)(dh * 0.22f));

    // Aktivite
    int32_t sy_start = dy + (int32_t)(dh * 0.36f);
#ifdef BOARD_VARIANT_LCD128
    int32_t gap = (int32_t)(36 * scale);   /* 30px font satır aralığı */
#else
    int32_t gap = (int32_t)(62 * scale);
#endif
    lv_label_set_text(s_activity_goal, "170");
    lv_obj_update_layout(s_activity_goal);
    lv_obj_set_pos(s_activity_goal, right_margin_x - lv_obj_get_width(s_activity_goal), sy_start);

    lv_label_set_text(s_activity_cal, "00");
    lv_obj_update_layout(s_activity_cal);
    lv_obj_set_pos(s_activity_cal, right_margin_x - lv_obj_get_width(s_activity_cal), sy_start + gap);

    lv_label_set_text(s_activity_hr, "06");
    lv_obj_update_layout(s_activity_hr);
    lv_obj_set_pos(s_activity_hr, right_margin_x - lv_obj_get_width(s_activity_hr), sy_start + gap * 2);

    // --- İKONLARIN BÜYÜTÜLMESİ VE YERLEŞİMİ ---
    if (s_activity_icon_cloud_bg && s_activity_icon_run_bg)
    {
        if (s_face_index == FACE_ACTIVITY)
        {
            lv_obj_remove_flag(s_activity_icon_cloud_bg, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_activity_icon_run_bg, LV_OBJ_FLAG_HIDDEN);

            // Daireleri daha büyük ve net hale getirdik
#ifdef BOARD_VARIANT_LCD128
            int32_t bg_size = (int32_t)(42 * scale);
#else
            int32_t bg_size = (int32_t)(60 * scale);
#endif
            lv_obj_set_size(s_activity_icon_cloud_bg, bg_size, bg_size);
            lv_obj_set_size(s_activity_icon_run_bg, bg_size, bg_size);

            // Running man ikonu hala bitmap → image scale ile
#ifdef BOARD_VARIANT_LCD128
            int32_t icon_scale = (s_face_mode == MODE_SELECTOR) ? 45 : 90;
#else
            int32_t icon_scale = (s_face_mode == MODE_SELECTOR) ? 60 : 130;
#endif
            lv_image_set_scale(s_activity_icon_run, icon_scale);
            lv_obj_center(s_activity_icon_run);

            // Hava durumu ikonu primitiflerle çiziliyor. Konteyneri bg ile aynı
            // boyuta ayarla, sonra mevcut condition'ı yeniden çiz (boyutlar
            // s değerine göre hesaplanıyor, scaling otomatik oluyor).
            int32_t wx_size = (s_face_mode == MODE_SELECTOR)
                                  ? (int32_t)(bg_size * 0.55f)
                                  : (int32_t)(bg_size * 0.90f);
            lv_obj_set_size(s_activity_icon_cloud, wx_size, wx_size);
            lv_obj_center(s_activity_icon_cloud);
            lv_obj_update_layout(s_activity_icon_cloud);
            weather_data_t _wd = {};
            weather_condition_t _c = weather_service_get(&_wd) ? _wd.condition
                                                               : WEATHER_UNKNOWN;
            weather_icon_render(s_activity_icon_cloud, _c);

#ifdef BOARD_VARIANT_LCD128
            /* Yuvarlak ekranda köşeler kesik → daireleri içeri/aşağı al. */
            lv_obj_set_pos(s_activity_icon_cloud_bg, dx + (int32_t)(34 * scale), dy + (int32_t)(42 * scale));
            lv_obj_set_pos(s_activity_icon_run_bg, dx + dw - (int32_t)(34 * scale) - bg_size, dy + (int32_t)(42 * scale));
#else
            lv_obj_set_pos(s_activity_icon_cloud_bg, dx + (int32_t)(20 * scale), dy + (int32_t)(35 * scale));
            lv_obj_set_pos(s_activity_icon_run_bg, dx + dw - (int32_t)(20 * scale) - bg_size, dy + (int32_t)(35 * scale));
#endif

            lv_obj_move_foreground(s_activity_icon_cloud_bg);
            lv_obj_move_foreground(s_activity_icon_run_bg);
        }
        else
        {
            lv_obj_add_flag(s_activity_icon_cloud_bg, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_activity_icon_run_bg, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Tarih
    char ds[64];
    const char *days[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
    const char *months[] = {"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
    snprintf(ds, sizeof(ds), "%s, %s %d", days[ti->tm_wday], months[ti->tm_mon], ti->tm_mday);

    lv_label_set_text(s_activity_date, ds);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_activity_date, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(s_activity_date, &lv_font_montserrat_24, 0);
#endif
    lv_obj_set_style_text_color(s_activity_date, lv_color_white(), 0);
    lv_obj_update_layout(s_activity_date);
    lv_obj_set_pos(s_activity_date, dx + (dw - lv_obj_get_width(s_activity_date)) / 2, dy + (int32_t)(dh * 0.88f));
}

/* ═══════════════════════════════════════════════════════════════════════════
 * v1.2.1 — FACE_IO_CONTROL (Çıkışlar watchface)
 *
 * Saat + tarih + 2 büyük toggle kartı (Çıkış 1, Çıkış 2). Mock state —
 * gerçek GPIO yazma yok (HW bağlandığında io_toggle_clicked_cb içine
 * gpio_set_level eklenir). State RTC_DATA_ATTR — deep sleep cycle'ında
 * korunur, reboot'ta default OFF (safe state).
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char* IO_TOGGLE_NAMES[2] = { "Cikis 1", "Cikis 2" };

static void io_toggle_apply_visual(int idx)
{
    if (idx < 0 || idx > 1) return;
    bool on = s_io_state[idx];
    if (s_io_toggle_outer[idx]) {
        lv_obj_set_style_bg_color(s_io_toggle_outer[idx],
            on ? lv_color_make(0x30, 0xD1, 0x58)     /* iOS green */
               : lv_color_make(0x39, 0x39, 0x3B),    /* iOS off gray */
            0);
        lv_obj_invalidate(s_io_toggle_outer[idx]);
    }
    if (s_io_toggle_knob[idx]) {
        /* Knob slot: 64 px outer - 30 px knob = 34 / 2 = 2 px her kenardan;
         * OFF x=2, ON x=32 */
        lv_obj_set_x(s_io_toggle_knob[idx], on ? 32 : 2);
    }
}

static void io_toggle_clicked_cb(lv_event_t* e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 1) return;
    s_io_state[idx] = !s_io_state[idx];
    io_toggle_apply_visual(idx);
    ESP_LOGI(TAG, "IO toggle %d (%s) → %s (mock, HW yok)",
             idx, IO_TOGGLE_NAMES[idx], s_io_state[idx] ? "ON" : "OFF");
    /* TODO v1.2.2+: HW bağlandığında gerçek GPIO yazımı buraya:
     *   gpio_set_level(PIN_IO_TOGGLE_15, s_io_state[0]);
     *   gpio_set_level(PIN_IO_TOGGLE_14, s_io_state[1]);
     * UYARI: GPIO 14/15 ŞU AN I²C BUS — başka pinler atanmadan yazılım
     * yapılırsa touch/IMU/RTC/AXP ölür. */
}

static void update_io_control_labels(const struct tm *ti)
{
    if (!s_io_time_label || !s_io_date_label || !ti) return;
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", ti->tm_hour, ti->tm_min);
    lv_label_set_text(s_io_time_label, tbuf);

    /* v1.2.3 (2026-05-19): UI dili İngilizce'ye çevrildi — ay kısaltmaları
     * Türkçe ("Oca","Sub",...) → İngilizce 3-harf ("Jan","Feb",...). */
    static const char* const months_en[12] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    char dbuf[16];
    int m = ti->tm_mon;
    if (m < 0 || m > 11) m = 0;
    snprintf(dbuf, sizeof(dbuf), "%d %s", ti->tm_mday, months_en[m]);
    lv_label_set_text(s_io_date_label, dbuf);
}

static void build_io_control_layer(void)
{
    if (s_io_layer) return;   /* already built */
    if (!s_face)    return;

    /* Tam-ekran layer, siyah arka plan. Hidden default — sadece FACE_IO_CONTROL
     * aktifken görünür. */
    s_io_layer = lv_obj_create(s_face);
    lv_obj_remove_style_all(s_io_layer);
    lv_obj_set_size(s_io_layer, SCR_W, SCR_H);
    lv_obj_set_pos(s_io_layer, 0, 0);
    lv_obj_set_style_bg_color(s_io_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_io_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_io_layer, 0, 0);
    lv_obj_set_style_border_width(s_io_layer, 0, 0);
    lv_obj_clear_flag(s_io_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_io_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_io_layer, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* v1.2.1: EVENT_BUBBLE — LONG_PRESSED event'i s_face'e yükselsin
     * (face_event_cb yakalar → enter_selector). Bu flag olmadan io_layer
     * event'i tüketiyor, long-press selector'a giriş çalışmıyor. */
    lv_obj_add_flag(s_io_layer, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(s_io_layer, LV_OBJ_FLAG_HIDDEN);

    /* 1) Saat label — üstte ortalı */
    s_io_time_label = lv_label_create(s_io_layer);
    lv_obj_set_style_text_color(s_io_time_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_io_time_label, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_io_time_label, "00:00");
    lv_obj_align(s_io_time_label, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_clear_flag(s_io_time_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_io_time_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 2) Tarih label — saat altı */
    s_io_date_label = lv_label_create(s_io_layer);
    lv_obj_set_style_text_color(s_io_date_label, lv_color_make(0x8E, 0x8E, 0x93), 0);
    lv_obj_set_style_text_font(s_io_date_label, &lv_font_montserrat_22, 0);
    lv_label_set_text(s_io_date_label, "01 Oca");
    lv_obj_align(s_io_date_label, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_clear_flag(s_io_date_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_io_date_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 3) İki büyük toggle kartı (dikey, alt alta) */
    const int32_t card_w = SCR_W - 60;
    const int32_t card_h = 100;
    const int32_t card_x = 30;
    const int32_t card_y_start = 150;
    const int32_t card_gap = 24;

    for (int i = 0; i < 2; i++) {
        /* Kart — tıklanabilir tüm yüzey */
        s_io_toggle_card[i] = lv_obj_create(s_io_layer);
        lv_obj_remove_style_all(s_io_toggle_card[i]);
        lv_obj_set_size(s_io_toggle_card[i], card_w, card_h);
        lv_obj_set_pos(s_io_toggle_card[i], card_x, card_y_start + i * (card_h + card_gap));
        lv_obj_set_style_bg_color(s_io_toggle_card[i], lv_color_make(0x1C, 0x1C, 0x1E), 0);
        lv_obj_set_style_bg_opa(s_io_toggle_card[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_io_toggle_card[i], 18, 0);
        lv_obj_set_style_border_width(s_io_toggle_card[i], 1, 0);
        lv_obj_set_style_border_color(s_io_toggle_card[i], lv_color_make(0x2C, 0x2C, 0x2E), 0);
        lv_obj_set_style_pad_all(s_io_toggle_card[i], 0, 0);
        lv_obj_add_flag(s_io_toggle_card[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_io_toggle_card[i], LV_OBJ_FLAG_SCROLLABLE);
        /* v1.2.1: LONG_PRESSED ve GESTURE event'leri s_face'e bubble etsin
         * (selector'a uzun-basışla giriş + swipe gesture nav için). CLICKED
         * event'i de bubble eder ama s_face'in CLICKED handler'ı yok, sorun yok. */
        lv_obj_add_flag(s_io_toggle_card[i], LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(s_io_toggle_card[i], LV_OBJ_FLAG_GESTURE_BUBBLE);
        /* press feedback transform pinning */
        lv_obj_set_style_transform_scale(s_io_toggle_card[i], 256, 0);
        lv_obj_set_style_transform_scale(s_io_toggle_card[i], 256, LV_STATE_PRESSED);
        lv_obj_add_event_cb(s_io_toggle_card[i], io_toggle_clicked_cb,
                            LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        /* Title — sol tarafta */
        s_io_toggle_title[i] = lv_label_create(s_io_toggle_card[i]);
        lv_obj_set_style_text_color(s_io_toggle_title[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(s_io_toggle_title[i], &lv_font_montserrat_24, 0);
        lv_label_set_text(s_io_toggle_title[i], IO_TOGGLE_NAMES[i]);
        lv_obj_align(s_io_toggle_title[i], LV_ALIGN_LEFT_MID, 24, 0);
        lv_obj_clear_flag(s_io_toggle_title[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_io_toggle_title[i], LV_OBJ_FLAG_EVENT_BUBBLE);

        /* Toggle outer — sağ tarafta, 64×34 rounded */
        s_io_toggle_outer[i] = lv_obj_create(s_io_toggle_card[i]);
        lv_obj_remove_style_all(s_io_toggle_outer[i]);
        lv_obj_set_size(s_io_toggle_outer[i], 64, 34);
        lv_obj_align(s_io_toggle_outer[i], LV_ALIGN_RIGHT_MID, -18, 0);
        lv_obj_set_style_radius(s_io_toggle_outer[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_io_toggle_outer[i], 0, 0);
        lv_obj_set_style_bg_opa(s_io_toggle_outer[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_io_toggle_outer[i],
                          (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                                          LV_OBJ_FLAG_CLICK_FOCUSABLE));
        lv_obj_add_flag(s_io_toggle_outer[i], LV_OBJ_FLAG_EVENT_BUBBLE);
        /* Transform pinning */
        lv_obj_set_style_transform_scale(s_io_toggle_outer[i], 256, 0);
        lv_obj_set_style_transform_scale(s_io_toggle_outer[i], 256, LV_STATE_PRESSED);

        /* Knob — 30×30 beyaz daire */
        s_io_toggle_knob[i] = lv_obj_create(s_io_toggle_outer[i]);
        lv_obj_remove_style_all(s_io_toggle_knob[i]);
        lv_obj_set_size(s_io_toggle_knob[i], 30, 30);
        lv_obj_set_y(s_io_toggle_knob[i], 2);
        lv_obj_set_style_bg_color(s_io_toggle_knob[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_io_toggle_knob[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_io_toggle_knob[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_io_toggle_knob[i], 0, 0);
        lv_obj_clear_flag(s_io_toggle_knob[i],
                          (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                                          LV_OBJ_FLAG_CLICK_FOCUSABLE));
        lv_obj_add_flag(s_io_toggle_knob[i], LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_transform_scale(s_io_toggle_knob[i], 256, 0);
        lv_obj_set_style_transform_scale(s_io_toggle_knob[i], 256, LV_STATE_PRESSED);

        /* RTC_DATA_ATTR state'inden başlangıç görseli */
        io_toggle_apply_visual(i);
    }

    /* İlk açılış için watch/tarih label'larını doldur */
    time_t now; time(&now);
    struct tm ti; localtime_r(&now, &ti);
    update_io_control_labels(&ti);
}

/* Selector thumbnail — mini toggle gösterimi.
 * SNAP_W×SNAP_H canvas üzerine küçük 2 toggle çizimi. */
static void draw_io_control_thumb(lv_layer_t* layer,
                                  int32_t ox, int32_t oy,
                                  int32_t ow, int32_t oh,
                                  float scl)
{
    /* Siyah bg */
    {
        lv_draw_rect_dsc_t bg;
        lv_draw_rect_dsc_init(&bg);
        bg.bg_color = lv_color_black();
        bg.bg_opa   = LV_OPA_COVER;
        lv_area_t a = { ox, oy, ox + ow - 1, oy + oh - 1 };
        lv_draw_rect(layer, &bg, &a);
    }

    /* Üstte küçük saat dummy (placeholder dash) */
    {
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = lv_color_make(0x70, 0x70, 0x70);
        dot.bg_opa   = LV_OPA_COVER;
        dot.radius   = (int32_t)(2 * scl);
        int32_t dw = (int32_t)(50 * scl);
        int32_t dh = (int32_t)(4  * scl);
        int32_t dx = ox + (ow - dw) / 2;
        int32_t dy = oy + (int32_t)(20 * scl);
        lv_area_t a = { dx, dy, dx + dw - 1, dy + dh - 1 };
        lv_draw_rect(layer, &dot, &a);
    }

    /* İki mini toggle */
#ifdef BOARD_VARIANT_LCD128
    /* 240 tabanlı kare kartta 502-tasarımı dikeyde taşıyordu (cihaz testi
     * 2026-07-13: ikinci toggle kart dışında). Kompakt dikey yerleşim. */
    int32_t card_w = (int32_t)((SCR_W - 56) * scl);
    int32_t card_h = (int32_t)(56 * scl);
    int32_t card_x = ox + (ow - card_w) / 2;
    int32_t card_y_start = oy + (int32_t)(52 * scl);
    int32_t card_gap = (int32_t)(12 * scl);
#else
    int32_t card_w = (int32_t)((SCR_W - 60) * scl);
    int32_t card_h = (int32_t)(100 * scl);
    int32_t card_x = ox + (ow - card_w) / 2;
    int32_t card_y_start = oy + (int32_t)(80 * scl);
    int32_t card_gap = (int32_t)(20 * scl);
#endif

    for (int i = 0; i < 2; i++) {
        int32_t cy = card_y_start + i * (card_h + card_gap);
        /* Kart bg */
        lv_draw_rect_dsc_t card;
        lv_draw_rect_dsc_init(&card);
        card.bg_color = lv_color_make(0x1C, 0x1C, 0x1E);
        card.bg_opa   = LV_OPA_COVER;
        card.radius   = (int32_t)(14 * scl);
        lv_area_t a = { card_x, cy, card_x + card_w - 1, cy + card_h - 1 };
        lv_draw_rect(layer, &card, &a);

        /* Mini toggle daire (sağda) — state'e göre renk */
        lv_draw_rect_dsc_t tg;
        lv_draw_rect_dsc_init(&tg);
        tg.bg_color = s_io_state[i] ? lv_color_make(0x30, 0xD1, 0x58)
                                     : lv_color_make(0x39, 0x39, 0x3B);
        tg.bg_opa   = LV_OPA_COVER;
        tg.radius   = LV_RADIUS_CIRCLE;
#ifdef BOARD_VARIANT_LCD128
        int32_t tw = (int32_t)(34 * scl);
        int32_t th = (int32_t)(18 * scl);
#else
        int32_t tw = (int32_t)(50 * scl);
        int32_t th = (int32_t)(26 * scl);
#endif
        int32_t tx = card_x + card_w - tw - (int32_t)(12 * scl);
        int32_t ty = cy + (card_h - th) / 2;
        lv_area_t ta = { tx, ty, tx + tw - 1, ty + th - 1 };
        lv_draw_rect(layer, &tg, &ta);

        /* Knob beyaz daire */
        lv_draw_rect_dsc_t kn;
        lv_draw_rect_dsc_init(&kn);
        kn.bg_color = lv_color_white();
        kn.bg_opa   = LV_OPA_COVER;
        kn.radius   = LV_RADIUS_CIRCLE;
#ifdef BOARD_VARIANT_LCD128
        int32_t kr = (int32_t)(14 * scl) / 2;
#else
        int32_t kr = (int32_t)(22 * scl) / 2;
#endif
        int32_t kcx = s_io_state[i]
                      ? (tx + tw - (int32_t)(14 * scl))
                      : (tx + (int32_t)(14 * scl));
        int32_t kcy = ty + th / 2;
        lv_area_t ka = { kcx - kr, kcy - kr, kcx + kr, kcy + kr };
        lv_draw_rect(layer, &kn, &ka);
    }
}

static void set_face_index(face_index_t idx)
    {
        // Clamp: unknown → analog
        if ((int)idx >= carousel_size() || (int)idx < 0)
            idx = FACE_ANALOG;

        s_face_index = idx;

        /* Selector mode'da bu fonksiyon her crossover'da (visual_pos 0.5'i geçince)
         * çağrılıyor → animasyon ortasında ESP_LOGI'nin UART blokunu (~150-300 µs),
         * digital/activity layer flag toggle'larını ve update_selector_ui'yi yapmak
         * tek frame'de spike yaratıyordu = "tık takılma". Selector mode'da:
         *   - layer flag'lar zaten enter_selector'da HIDDEN'a çekildi, set_face_index
         *     burada idempotent
         *   - update_selector_ui caller (selector_apply_pose) zaten devamında çağırıyor
         *   - log normal yön değişikliği için DEBUG'a alındı (default seviyede sessiz) */
        if (s_face_mode == MODE_SELECTOR) {
            ESP_LOGD(TAG, "set_face_index (selector): -> %d", (int)idx);
            return;
        }

        ESP_LOGI(TAG, "set_face_index: %d -> %d (mode=%d)", (int)s_face_index, (int)idx, (int)s_face_mode);

        // LVGL widget layers — only visible for their specific face in normal mode
        if (s_digital_layer) {
            if (s_face_index == FACE_DIGITAL)
                lv_obj_clear_flag(s_digital_layer, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(s_digital_layer, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_activity_layer) {
            if (s_face_index == FACE_ACTIVITY)
                lv_obj_clear_flag(s_activity_layer, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(s_activity_layer, LV_OBJ_FLAG_HIDDEN);
        }
        /* v1.2.1: io_control layer show/hide */
        if (s_io_layer) {
            if (s_face_index == FACE_IO_CONTROL) {
                lv_obj_clear_flag(s_io_layer, LV_OBJ_FLAG_HIDDEN);
                /* İlk activate'da label güncelle */
                time_t now; time(&now);
                struct tm ti; localtime_r(&now, &ti);
                update_io_control_labels(&ti);
            } else {
                lv_obj_add_flag(s_io_layer, LV_OBJ_FLAG_HIDDEN);
            }
        }
        update_selector_ui();
    }

static void request_face_index(face_index_t idx)
{
    face_index_t requested = idx;
    if ((int)idx >= carousel_size() || idx < 0) idx = FACE_ANALOG;

    if (idx == s_face_index) return;
    ESP_LOGI(TAG, "selector request_face_index: cur=%d req=%d final=%d mode=%d", (int)s_face_index, (int)requested, (int)idx, (int)s_face_mode);
    s_pending_face_index = -1;
    set_face_index(idx);

    if (s_face_mode != MODE_SELECTOR) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm ti;
        localtime_r(&tv.tv_sec, &ti);
        update_digital_labels(&ti);
        update_activity_labels(&ti);
        update_io_control_labels(&ti);   /* v1.2.1 */
    }

    if (s_face) { freeze_frame(); lv_obj_invalidate(s_face); }
}

static void enter_selector(void)
{
    if (s_face_mode == MODE_SELECTOR) return;
    refresh_custom_faces_from_gallery();
    s_face_mode = MODE_SELECTOR;
    s_sel_down = false;
    s_sel_swiped_recent = false;
    s_sel_swipe_just_changed = false;
    s_sel_dragged = false;
    s_sel_swipe_block_until_us = 0;
    s_sel_press_start_us = 0;
    s_sel_ignore_release_once = true;
    s_ignore_next_selector_click = true;
    s_sel_pos = (float)s_face_index;
    s_sel_visual_pos = s_sel_pos;
    s_sel_vel = 0.0f;
    s_sel_last_sample_us = 0;
    s_sel_release_pending = false;
    s_sel_release_due_us = 0;
    s_sel_snap_active = false;
    s_sel_snap_target = (float)s_face_index;
    s_sel_snap_start = s_sel_pos;
    s_sel_snap_t = 1.0f;
    s_sel_delete_mode = false;
    s_sel_delete_reveal = 0.0f;
    s_sel_last_drag_us = 0;
    s_sel_click_armed = false;
    s_sel_click_armed_until_us = 0;
    s_draw_scale = SEL_SCALE_MAX;
    s_draw_y_off = -10.0f;
    s_draw_x_off = 0.0f;

    lv_indev_t* indev = lv_indev_get_act();
    if (indev) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        s_sel_down = true;
        s_sel_x0 = (int16_t)p.x;
        s_sel_y0 = (int16_t)p.y;
        s_sel_last_x = (int16_t)p.x;
        s_sel_last_y = (int16_t)p.y;
    }
    s_sel_last_sample_us = esp_timer_get_time();
    if (s_timer) lv_timer_set_period(s_timer, 16);
    ui_service_lock_swipe(true);

    // Hide LVGL widget layers — selector uses snapshots instead of live widgets
    if (s_digital_layer)  lv_obj_add_flag(s_digital_layer,  LV_OBJ_FLAG_HIDDEN);
    if (s_activity_layer) lv_obj_add_flag(s_activity_layer, LV_OBJ_FLAG_HIDDEN);
    if (s_io_layer)       lv_obj_add_flag(s_io_layer,       LV_OBJ_FLAG_HIDDEN);   /* v1.2.1 */

    // Build snapshots for all real faces (built-in + custom). FACE_NEW is drawn live.
    for (int fi = 0; fi < FACE_BUILTIN_COUNT + s_custom_face_count; fi++) {
        build_face_snapshot((face_index_t)fi);
    }

    selector_apply_pose();
    if (s_face) { freeze_frame(); lv_obj_invalidate(s_face); }
}

static void exit_selector(void)
{
    if (s_face_mode != MODE_SELECTOR) return;
    s_face_mode = MODE_NORMAL;
    s_sel_down = false;
    s_sel_swiped_recent = false;
    s_sel_swipe_just_changed = false;
    s_sel_dragged = false;
    s_sel_swipe_block_until_us = 0;
    s_sel_press_start_us = 0;
    s_sel_ignore_release_once = false;
    s_ignore_next_selector_click = false;
    s_sel_vel = 0.0f;
    s_sel_visual_pos = (float)s_face_index;
    s_sel_last_sample_us = 0;
    s_sel_release_pending = false;
    s_sel_release_due_us = 0;
    s_sel_snap_active = false;
    s_sel_snap_target = (float)s_face_index;
    s_sel_snap_start = (float)s_face_index;
    s_sel_snap_t = 1.0f;
    s_sel_delete_mode = false;
    s_sel_delete_reveal = 0.0f;
    s_sel_last_drag_us = 0;
    s_sel_click_armed = false;
    s_sel_click_armed_until_us = 0;
    s_draw_scale = 1.0f;
    s_draw_y_off = 0.0f;
    s_draw_x_off = 0.0f;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm ti;
    localtime_r(&tv.tv_sec, &ti);
    update_digital_labels(&ti);
    update_activity_labels(&ti);
    update_io_control_labels(&ti);   /* v1.2.1 */
    if (s_timer) lv_timer_set_period(s_timer, 30);
    ui_service_lock_swipe(false);
    // Restore LVGL widget layer visibility based on active face
    if (s_digital_layer) {
        if (s_face_index == FACE_DIGITAL)
            lv_obj_clear_flag(s_digital_layer, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_digital_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_activity_layer) {
        if (s_face_index == FACE_ACTIVITY)
            lv_obj_clear_flag(s_activity_layer, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_activity_layer, LV_OBJ_FLAG_HIDDEN);
    }
    /* v1.2.1: io_control layer restore — selector'dan FACE_IO_CONTROL'a çıkarsa
     * görünür olmalı, aksi halde gizli. Bu satır olmadığı için "Cikislar"
     * seçildikten sonra siyah ekran çıkıyordu. */
    if (s_io_layer) {
        if (s_face_index == FACE_IO_CONTROL)
            lv_obj_clear_flag(s_io_layer, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_io_layer, LV_OBJ_FLAG_HIDDEN);
    }
    update_selector_ui();
    if (s_face) { freeze_frame(); lv_obj_invalidate(s_face); }
}

static void face_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t* cur_target = (lv_obj_t*)lv_event_get_current_target(e);

    if (s_face_mode == MODE_SELECTOR &&
        (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_GESTURE ||
         code == LV_EVENT_CLICKED || code == LV_EVENT_LONG_PRESSED || code == LV_EVENT_PRESS_LOST)) {
        ESP_LOGI(TAG, "selector evt=%d face=%d target=%s current=%s",
                 (int)code, (int)s_face_index, event_target_name(target), event_target_name(cur_target));
    }

    if (code == LV_EVENT_GESTURE && s_face_mode != MODE_SELECTOR) {
        lv_indev_t* indev = lv_indev_get_act();
        if (indev) {
            lv_dir_t gdir = lv_indev_get_gesture_dir(indev);
            if (gdir == LV_DIR_BOTTOM) {
                /* Swipe zone DRAG aktive olduysa veya yeni release oldu (250 ms
                 * suppress) → çift cc_open'ı önle. Zone fallback olarak gesture
                 * yine geçerli (zone'un dışında orta-ekrandan swipe için). */
                int64_t now_us = esp_timer_get_time();
                if (s_cc_drag_active) return;
                if (now_us < s_cc_drag_suppress_until_us) return;
                cc_open();
                return;
            }
            if (gdir == LV_DIR_TOP && s_cc_open) {
                cc_close();
                return;
            }
        }
    }

    if (s_cc_open && s_face_mode != MODE_SELECTOR) {
        if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING ||
            code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
            code == LV_EVENT_LONG_PRESSED || code == LV_EVENT_CLICKED) {
            return;
        }
    }

    if (s_face_mode != MODE_SELECTOR) {
        if (code == LV_EVENT_PRESSED) {
            s_hold_down = true;
            s_hold_fired = false;
            s_hold_start_us = esp_timer_get_time();
            s_norm_swiped = false;
            s_norm_dragging = false;
            lv_indev_t* indev = lv_indev_get_act();
            if (indev) {
                lv_point_t p;
                lv_indev_get_point(indev, &p);
                s_norm_x0 = (int16_t)p.x;
                s_norm_y0 = (int16_t)p.y;
                s_norm_last_x = (int16_t)p.x;
                s_norm_last_y = (int16_t)p.y;
            } else {
                s_norm_x0 = -1;
                s_norm_y0 = -1;
                s_norm_last_x = -1;
                s_norm_last_y = -1;
            }
        } else if (code == LV_EVENT_PRESSING) {
            int adx_now = 0;
            int ady_now = 0;
            lv_indev_t* indev = lv_indev_get_act();
            if (indev) {
                lv_point_t p;
                lv_indev_get_point(indev, &p);
                s_norm_last_x = (int16_t)p.x;
                s_norm_last_y = (int16_t)p.y;

                if (s_face_index == FACE_ACTIVITY && !s_norm_swiped && s_norm_x0 >= 0) {
                    int dx = (int)p.x - (int)s_norm_x0;
                    int dy = (int)p.y - (int)s_norm_y0;
                    int adx = dx < 0 ? -dx : dx;
                    int ady = dy < 0 ? -dy : dy;
                    adx_now = adx;
                    ady_now = ady;
                    /* Drag-cancel threshold: 3 → 12 px. FT3168 jitter
                     * 1-5 px envelope'unu aşmayı bekle; aksi halde stationary
                     * hold sırasında jitter hold'u iptal ediyordu (kullanıcı
                     * "sert basmam gerekiyor" raporu). */
                    if (adx >= 12 && adx > ady) {
                        s_norm_dragging = true;
                        s_hold_down = false;
                    }
                    /* Activity face custom swipe-to-launcher KALDIRILDI:
                     * Global app_manager check_swipe (case -1) zaten görsel
                     * sağa swipe'ta launcher'ı açıyor. Custom handler
                     * launcher'ı açıyor, 20 ms sonra global handler aynı
                     * swipe'ı yine işliyor → s_current=LAUNCHER iken
                     * app_manager_back() çağırıyor → activity'ye dönüyor
                     * (kullanıcı raporu 2026-05-05: "launcher'a geçip geri
                     * dönüyor"). s_norm_dragging hâlâ set ediliyor (hold
                     * cancellation için), ama launcher'ı açmıyor. */
                }
            }
            if (s_hold_down && !s_hold_fired) {
                int64_t now = esp_timer_get_time();
                if (s_face_index == FACE_ACTIVITY) {
                    /* Long-press jitter tolerance: 2 → 12 px. FT3168 jitter
                     * 5 px'i aşıyordu → adx_now>2 → long-press hiç fire
                     * etmiyor → "ilk denemede algılamıyor" hissi. */
                    if (adx_now <= 12 && ady_now <= 12 && now - s_hold_start_us >= 420000) {
                        enter_selector();
                        s_hold_fired = true;
                        /* Tight belt-and-suspenders window; release-after-hold is
                         * already covered by s_sel_ignore_release_once +
                         * s_ignore_next_selector_click inside enter_selector(). */
                        s_sel_ignore_click_until_us = now + 120000;
                        return;
                    }
                } else {
                    if (now - s_hold_start_us >= 420000) {
                        enter_selector();
                        s_hold_fired = true;
                        s_sel_ignore_click_until_us = now + 120000;
                        return;
                    }
                }
            }
        } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            if (s_face_index == FACE_ACTIVITY && !s_hold_fired && !s_norm_swiped && s_norm_x0 >= 0) {
                lv_indev_t* indev = lv_indev_get_act();
                int32_t ex = s_norm_last_x;
                int32_t ey = s_norm_last_y;
                if (indev) {
                    lv_point_t p;
                    lv_indev_get_point(indev, &p);
                    ex = p.x;
                    ey = p.y;
                }
                /* Activity face RELEASED swipe-to-launcher KALDIRILDI:
                 * Global app_manager check_swipe yeterli; custom handler
                 * çift-tetikleme bug'ı yapıyordu. (Eski mantık burada
                 * dx/dy'i hesaplayıp app_manager_launch çağırıyordu;
                 * artık global tek başına işliyor.) */
                (void)ex; (void)ey;
            }
            s_hold_down = false;
            s_hold_fired = false;
            s_norm_swiped = false;
            s_norm_dragging = false;
            s_norm_x0 = -1;
            s_norm_y0 = -1;
            s_norm_last_x = -1;
            s_norm_last_y = -1;
        }
    }

    if (code == LV_EVENT_LONG_PRESSED) {
        if (s_face_mode != MODE_SELECTOR && s_face_index == FACE_ACTIVITY) {
            return;
        }
        if (s_face_mode != MODE_SELECTOR) enter_selector();
        /* Short window — just enough to absorb the release that ends the hold.
         * Real per-event guards (s_sel_ignore_release_once,
         * s_ignore_next_selector_click) handle the first release/click. */
        s_sel_ignore_click_until_us = esp_timer_get_time() + 120000;
        return;
    }

    if (code == LV_EVENT_GESTURE && s_face_mode != MODE_SELECTOR && s_face_index == FACE_ACTIVITY) {
        /* Activity face LVGL gesture handler KALDIRILDI:
         * Global app_manager check_swipe görsel sağa swipe'ta launcher'ı
         * açıyor. Custom LVGL gesture aynı swipe'ı tekrar işleyip race
         * condition yaratıyordu. Sessizce konsume et — global yeterli. */
        (void)0;
        if (false) {
            s_norm_swiped = true;
            s_hold_down = false;
            app_manager_launch(APP_ID_LAUNCHER);
            return;
        }
    }

    if (s_face_mode == MODE_SELECTOR) {
        if (code == LV_EVENT_GESTURE) {
            return;
        }

        if (code == LV_EVENT_CLICKED) {
            int64_t now = esp_timer_get_time();
            if (!s_sel_click_armed || now > s_sel_click_armed_until_us) return;
            s_sel_click_armed = false;
            if (s_sel_release_pending) return;
            if ((now - s_sel_last_drag_us) < 260000) return;
            if (fabsf(s_sel_vel) > 0.25f) return;
            if (s_ignore_next_selector_click) {
                s_ignore_next_selector_click = false;
                return;
            }
            if (now < s_sel_ignore_click_until_us) return;
            if (s_sel_dragged) {
                s_sel_dragged = false;
                return;
            }

            if (s_sel_delete_mode) {
                return;
            }

            // "Yeni" card: open Gallery app to manage uploaded photos and selection.
            if ((int)s_face_index == face_new_idx()) {
                gallery_service_request_pick_mode();
                app_manager_launch(APP_ID_GALLERY);
                s_sel_ignore_click_until_us = now + 250000;
                return;
            }

            exit_selector();
            return;
        }

        if (!(code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING ||
              code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)) {
            return;
        }

        lv_indev_t* indev = lv_indev_get_act();
        lv_point_t p = { s_sel_last_x, s_sel_last_y };
        if (indev) {
            lv_indev_get_point(indev, &p);
        }

        if (code == LV_EVENT_PRESSED) {
            if (!indev) return;
            /* CLICK ARMED — selector mode'da PRESSED ile arm edilir, RELEASED
             * sonrası 500 ms boyunca CLICKED event handler'ı (line 1902)
             * çalışmasına izin verir. Önceden hiçbir yerde true'ya set
             * edilmediği için CLICKED handler her zaman early-return ediyordu
             * → kullanıcı "selector'da tap fullscreen'e geçmiyor, 2-3 kez
             * dokunmam gerekiyor" raporu (2026-05-04). Inline tap fallback
             * (line 2106) jitter'a hassas; CLICKED yolu daha güvenilir. */
            int64_t press_now = esp_timer_get_time();
            s_sel_click_armed = true;
            s_sel_click_armed_until_us = press_now + 500000;   /* 500 ms */
            s_sel_release_pending = false;
            s_sel_release_due_us = 0;
            s_sel_snap_active = false;
            s_sel_snap_target = s_sel_pos;
            s_sel_snap_start = s_sel_pos;
            s_sel_snap_t = 1.0f;
            s_sel_down = true;
            s_sel_dragged = false;
            s_sel_press_start_us = press_now;
            s_sel_x0 = (int16_t)p.x;
            s_sel_y0 = (int16_t)p.y;
            s_sel_last_x = (int16_t)p.x;
            s_sel_last_y = (int16_t)p.y;
            s_sel_last_sample_us = s_sel_press_start_us;
            s_sel_vel = 0.0f;
            /* Fresh press: clear carry-over drag timestamp from a previous
             * interaction so the CLICKED gate (now - s_sel_last_drag_us < 260ms)
             * doesn't reject an unrelated later tap. */
            s_sel_last_drag_us = 0;
            return;
        }

        if (code == LV_EVENT_PRESSING) {
            if (!indev) return;
            if (s_sel_release_pending) {
                s_sel_release_pending = false;
                s_sel_release_due_us = 0;
                s_sel_snap_active = false;
                s_sel_snap_target = s_sel_pos;
                s_sel_snap_start = s_sel_pos;
                s_sel_snap_t = 1.0f;
            }
            if (!s_sel_down) {
                s_sel_down = true;
                s_sel_dragged = false;
                s_sel_press_start_us = esp_timer_get_time();
                s_sel_x0 = (int16_t)p.x;
                s_sel_y0 = (int16_t)p.y;
                s_sel_last_x = (int16_t)p.x;
                s_sel_last_y = (int16_t)p.y;
                s_sel_last_sample_us = s_sel_press_start_us;
                s_sel_vel = 0.0f;
                return;
            }
            int prev_x = (int)s_sel_last_x;
            s_sel_last_x = (int16_t)p.x;
            s_sel_last_y = (int16_t)p.y;
            int dx = (int)s_sel_last_x - (int)s_sel_x0;
            int dy = (int)s_sel_last_y - (int)s_sel_y0;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;

            if (face_is_custom((int)s_face_index)) {
                if (dy < 0 && ady > (int)(adx * 1.10f)) {
                    s_sel_delete_reveal = clampf_((-dy) / 200.0f, 0.0f, 1.0f);
                    s_sel_last_drag_us = esp_timer_get_time();
                    s_sel_dragged = true;
                    if (s_face) { freeze_frame(); lv_obj_invalidate(s_face); }
                    return;
                }
            }

            if (s_sel_delete_mode) {
                return;
            }

            /* Drag threshold: 6 → 4 px. Daha dar threshold = daha hassas drag
             * detection → kullanıcının küçük sürüklemeleri "tap" olarak yanlış
             * yorumlanmaz. (FT3168 jitter ~1-3 px; 4 px üstü gerçek niyettir.) */
            if (adx >= 4 && adx > ady) s_sel_dragged = true;

            int delta_x = (int)s_sel_last_x - prev_x;
            if (delta_x > -2 && delta_x < 2) delta_x = 0;  // tightened dead zone (was ±4)
            if (delta_x != 0) {
                // 1.6x sensitivity: 190px drag ≈ 1 card (was 305px)
                s_sel_pos = wrap_face_pos_(s_sel_pos - ((float)delta_x * 1.75f / SEL_CARD_SPACING));
                s_sel_last_drag_us = esp_timer_get_time();
            }

            // During drag: visual position tracks finger 1:1 — no lag, no snap-jump.
            // Spring snap only fires after release (in anim_timer_cb).
            s_sel_visual_pos = s_sel_pos;

            int64_t now_us = esp_timer_get_time();
            int64_t dt_us = now_us - s_sel_last_sample_us;
            if (dt_us >= 5000) {
                float dt = (float)dt_us / 1000000.0f;
                float inst_v = -((float)delta_x * 1.75f / SEL_CARD_SPACING) / dt;
                inst_v = clampf_(inst_v, -8.0f, 8.0f);
                /* Velocity low-pass: önceki 0.62/0.38 → 0.78/0.22. Touch sample
                 * cadence (10 ms) ile anim tick (16 ms) faz aliasingi velocity'ye
                 * de yansıyordu; daha ağır filtre = daha sakin glide projection. */
                s_sel_vel = s_sel_vel * 0.78f + inst_v * 0.22f;
                s_sel_last_sample_us = now_us;
            }

            /* Drag phase render-sync: PRESSING ~100 Hz cadence ama anim_timer_cb
             * 60 Hz, → her 16 ms aralığa 1-2 PRESSING tick düşüyor. Eğer
             * invalidate'i yalnız anim_timer'a bırakırsak bir frame'de 2 PRESSING'in
             * sadece son'unu, sonraki frame'de hiçbirini gösteriyorduk → "tık dalgalanma".
             * Burada delta != 0 olduğunda direkt selector_apply_pose + freeze_frame +
             * invalidate çağırarak parmak hareketini bir sonraki LVGL refresh sınırına
             * (en fazla 16 ms) gecikmeyle ekrana yansıtıyoruz. update_digital_labels
             * çağrıları selector_apply_pose'dan kaldırıldı → bu yol artık ucuz. */
            if (delta_x != 0 && s_face) {
                selector_apply_pose();
                freeze_frame();
                lv_obj_invalidate(s_face);
            }
            return;
        }

        if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            if (s_sel_ignore_release_once) {
                s_sel_ignore_release_once = false;
                s_sel_down = false;
                s_sel_x0 = -1;
                s_sel_y0 = -1;
                s_sel_last_x = -1;
                s_sel_last_y = -1;
                return;
            }
            if (s_sel_down && s_sel_x0 >= 0) {
                int64_t now = esp_timer_get_time();
                int ex = (indev ? (int)p.x : (int)s_sel_last_x);
                int ey = (indev ? (int)p.y : (int)s_sel_last_y);
                int dx = ex - (int)s_sel_x0;
                int dy = ey - (int)s_sel_y0;
                int adx = dx < 0 ? -dx : dx;
                int ady = dy < 0 ? -dy : dy;

                if (s_sel_delete_mode || s_sel_delete_reveal > 0.01f) {
                    if (!s_sel_delete_mode) {
                        s_sel_delete_mode = (s_sel_delete_reveal >= 0.45f) && face_is_custom((int)s_face_index);
                    }
                    if (!s_sel_delete_mode) {
                        update_selector_ui();
                        if (s_face) { freeze_frame(); lv_obj_invalidate(s_face); }
                    }

                    int32_t bx, by, bw, bh;
                    selector_edit_btn_rect(&bx, &by, &bw, &bh);
                    if (point_in_rect_(ex, ey, bx, by, bw, bh)) {
                        s_sel_delete_mode = false;
                        update_selector_ui();
                        if (s_face) { freeze_frame(); lv_obj_invalidate(s_face); }
                    } else {
                        int32_t tx, ty, tw, th;
                        selector_trash_rect(&tx, &ty, &tw, &th);
                        if (s_sel_delete_mode && point_in_rect_(ex, ey, tx, ty, tw, th) && face_is_custom((int)s_face_index)) {
                        selector_remove_current_custom();
                        update_selector_ui();
                        if (s_face) { freeze_frame(); lv_obj_invalidate(s_face); }
                        }
                    }
                    s_sel_down = false;
                    s_sel_press_start_us = 0;
                    s_sel_x0 = -1;
                    s_sel_y0 = -1;
                    s_sel_last_x = -1;
                    s_sel_last_y = -1;
                    s_sel_last_sample_us = 0;
                    s_sel_release_pending = false;
                    s_sel_release_due_us = 0;
                    // Prevent the synthetic CLICKED right after release from
                    // triggering normal selector exit/fullscreen.
                    s_ignore_next_selector_click = true;
                    s_sel_ignore_click_until_us = now + 220000;
                    return;
                }

                // IMPORTANT:
                // Process tap INLINE here — do NOT wait for LV_EVENT_CLICKED.
                // LVGL 9's indev scroll detection can suppress CLICKED if the
                // finger wobbles even 3–5 px during the press, which made taps
                // feel unresponsive (user had to tap 2–3× for fullscreen).
                // We apply the same guards the CLICKED handler used.
                //
                // Drag detection: 3 katmanlı (kullanıcı raporu 2026-05-04 —
                // "selector'da elimi kaldırdığım an bir kadrana fullscreen
                // yapıyor, halbuki sürükledim"):
                //   1. adx/ady < 4 (eski 6'dan dar — hassas tap detection)
                //   2. s_sel_dragged false (PRESSING içinde >=6 px set'lendiyse)
                //   3. s_sel_last_drag_us recent değil — drag'in delta_x>=2
                //      sample'larından update edilmiş timestamp; recent ise
                //      drag oldu demek, click suppressed.
                bool no_drag_recent = (s_sel_last_drag_us == 0) ||
                                       ((now - s_sel_last_drag_us) >= 200000);
                if (adx < 4 && ady < 4 && !s_sel_dragged && no_drag_recent) {
                    // Disarm — any synthetic CLICKED that follows will no-op.
                    s_sel_click_armed = false;
                    s_sel_click_armed_until_us = 0;

                    // Cleanup tap state before branching (both branches need this).
                    s_sel_down = false;
                    s_sel_press_start_us = 0;
                    s_sel_x0 = -1;
                    s_sel_y0 = -1;
                    s_sel_last_x = -1;
                    s_sel_last_y = -1;
                    s_sel_last_sample_us = 0;
                    s_sel_release_pending = false;
                    s_sel_release_due_us = 0;
                    s_sel_vel = 0.0f;
                    s_sel_last_drag_us = 0;

                    // Ignore-window guards (long-press release, recent gesture).
                    if (s_ignore_next_selector_click) {
                        s_ignore_next_selector_click = false;
                        // Make sure the follow-up synthetic CLICKED is also eaten.
                        s_sel_ignore_click_until_us = now + 220000;
                        return;
                    }
                    if (now < s_sel_ignore_click_until_us) {
                        return;
                    }
                    if (s_sel_delete_mode) {
                        // Delete-mode taps are handled in their own release branch above.
                        return;
                    }

                    // "Yeni" card: open Gallery app to pick uploaded photos.
                    if ((int)s_face_index == face_new_idx()) {
                        gallery_service_request_pick_mode();
                        app_manager_launch(APP_ID_GALLERY);
                        // Swallow the synthetic CLICKED that will follow.
                        s_sel_ignore_click_until_us = now + 250000;
                        return;
                    }

                    // Regular face selection — exit selector mode into fullscreen.
                    s_sel_ignore_click_until_us = now + 250000;
                    exit_selector();
                    return;
                } else {
                    s_sel_click_armed = false;
                    s_sel_click_armed_until_us = 0;
                    s_sel_ignore_click_until_us = now + 120000;
                }

                s_sel_release_pending = true;
                s_sel_release_due_us = now + SEL_RELEASE_DEBOUNCE_US;
                return;
            }
            s_sel_down = false;
            s_sel_press_start_us = 0;
            s_sel_x0 = -1;
            s_sel_y0 = -1;
            s_sel_last_x = -1;
            s_sel_last_y = -1;
            // Keep s_sel_last_sample_us — timer uses it to suppress premature spring for 180ms
            return;
        }
    }
}

// ─── Side-card thumbnail renderers ───────────────────────────────────────────
// Each draws a scaled-down real preview of a face at (ox,oy,ow,oh) on the layer.
// All coordinates are in screen-space (absolute pixels).

static void draw_analog_thumb(lv_layer_t* layer,
                               int32_t ox, int32_t oy, int32_t ow, int32_t oh,
                               float scl)
{
    // Static reference time: 10:09:30
    float sec_a  = 30.0f/60.0f  * 2.0f*(float)M_PI;
    float min_a  = 9.5f/60.0f   * 2.0f*(float)M_PI;
    float hour_a = (10.0f+9.5f/60.0f)/12.0f * 2.0f*(float)M_PI;

    float w_in = (float)(SCR_W - 2*SAFE_INSET) * scl;
    float h_in = (float)(SCR_H - 2*SAFE_INSET) * scl;
    float ox0  = (float)ox + ((float)ow - w_in) * 0.5f;
    float oy0  = (float)oy + ((float)oh - h_in) * 0.5f;
    int32_t cx = ox + ow/2;
    int32_t cy = oy + oh/2;

    // 60 ticks (skip every other for performance)
    for (int i = 0; i < 120; i += 2) {
        draw_tick(layer, i, w_in, h_in,
                  (TICK_MARGIN+4)*scl, CORNER_R*scl,
                  28*scl, 12*scl,
                  (int)roundf(10*scl), (int)roundf(2*scl),
                  ox0, oy0);
    }
    draw_hand_pill(layer, hour_a,
                   (int32_t)(H_TIP*scl), (int32_t)(16*scl), (int32_t)(10*scl), cx, cy);
    draw_hand_pill(layer, min_a,
                   (int32_t)(M_TIP*scl), (int32_t)(12*scl), (int32_t)(7*scl),  cx, cy);

    lv_draw_line_dsc_t sd;
    lv_draw_line_dsc_init(&sd);
    sd.color = lv_color_make(0xFF, 0x3B, 0x30);
    sd.width = 1; sd.round_start = sd.round_end = 1;
    sd.p1.x = (lv_value_precise_t)px_(sec_a, -(float)S_TAIL*scl, cx);
    sd.p1.y = (lv_value_precise_t)py_(sec_a, -(float)S_TAIL*scl, cy);
    sd.p2.x = (lv_value_precise_t)px_(sec_a, 165.0f*scl, cx);
    sd.p2.y = (lv_value_precise_t)py_(sec_a, 165.0f*scl, cy);
    lv_draw_line(layer, &sd);

    draw_circle(layer, cx, cy, (int32_t)(5.0f*scl), lv_color_white());
    draw_circle(layer, cx, cy, (int32_t)(2.0f*scl), lv_color_black());
}

static void draw_digital_thumb(lv_layer_t* layer,
                                int32_t ox, int32_t oy, int32_t ow, int32_t oh,
                                float scl)
{
    const lv_font_t *font = DGT_THUMB_FONT;

    // ── Dikey ortalama: font'un gerçek line_height'ına göre hesapla ──────────
    int32_t lh      = (int32_t)font->line_height;   // gerçek piksel yüksekliği
    int32_t row_gap = 10;                            // watch/dakika arası boşluk (canvas piksel)
    int32_t total_h = lh * 2 + row_gap;
    int32_t row1_y  = oy + (oh - total_h) / 2;      // dikey olarak tam ortalı
    int32_t row2_y  = row1_y + lh + row_gap;

    // ── Yatay ortalama: her rakamın advance genişliğine göre hesapla ─────────
    // "10" için: "1" ve "0" genişlikleri
    lv_font_glyph_dsc_t gdsc;
    lv_font_get_glyph_dsc(font, &gdsc, '1', '0');
    int32_t w1 = (int32_t)gdsc.adv_w;

    lv_font_get_glyph_dsc(font, &gdsc, '0', '\0');
    int32_t w0 = (int32_t)gdsc.adv_w;

    lv_font_get_glyph_dsc(font, &gdsc, '9', '\0');
    int32_t w9 = (int32_t)gdsc.adv_w;

    int32_t col_gap = 8;   // rakamlar arası boşluk (canvas piksel)

    // "10" satırını ortala
    int32_t p10_w  = w1 + col_gap + w0;
    int32_t p10_c1 = ox + (ow - p10_w) / 2;
    int32_t p10_c2 = p10_c1 + w1 + col_gap;

    // "09" satırını ortala
    int32_t p09_w  = w0 + col_gap + w9;
    int32_t p09_c1 = ox + (ow - p09_w) / 2;
    int32_t p09_c2 = p09_c1 + w0 + col_gap;

    // ── Çizim ────────────────────────────────────────────────────────────────
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font       = font;
    dsc.opa        = LV_OPA_COVER;
    dsc.text_local = 0;

    // Saat "10" (sarı)
    dsc.color = lv_color_hex(0xECEB38);

    dsc.text = "1";
    lv_area_t a = {p10_c1, row1_y, p10_c1 + w1 + 4, row1_y + lh + 4};
    lv_draw_label(layer, &dsc, &a);

    dsc.text = "0";
    a = (lv_area_t){p10_c2, row1_y, p10_c2 + w0 + 4, row1_y + lh + 4};
    lv_draw_label(layer, &dsc, &a);

    // Dakika "09" (pembe)
    dsc.color = lv_color_hex(0xF6BAC5);

    dsc.text = "0";
    a = (lv_area_t){p09_c1, row2_y, p09_c1 + w0 + 4, row2_y + lh + 4};
    lv_draw_label(layer, &dsc, &a);

    dsc.text = "9";
    a = (lv_area_t){p09_c2, row2_y, p09_c2 + w9 + 4, row2_y + lh + 4};
    lv_draw_label(layer, &dsc, &a);
}

static void draw_activity_thumb(lv_layer_t *layer,
                                int32_t ox, int32_t oy, int32_t ow, int32_t oh,
                                float scl)
{
#ifdef BOARD_VARIANT_LCD128
    /* 163px selector kartı: fontlar ölçeklenemediği için küçük sabitler
     * (cihaz testi 2026-07-13: 40/44px fontlar karttan taşıyordu). */
    const lv_font_t* F_TIME = &lv_font_montserrat_16;
    const lv_font_t* F_STAT = &lv_font_montserrat_20;
    const lv_font_t* F_DATE = &lv_font_montserrat_12;
    const int32_t TIME_W = 84, STAT_W = 46, STAT_H = 24;
    const int32_t STATS_GAP = (int32_t)(28 * scl) + 8;
#else
    const lv_font_t* F_TIME = &lv_font_montserrat_40;
    const lv_font_t* F_STAT = &lv_font_montserrat_44;
    const lv_font_t* F_DATE = &lv_font_montserrat_22;
    const int32_t TIME_W = 180, STAT_W = 120, STAT_H = 70;
    const int32_t STATS_GAP = (int32_t)(62 * scl);
#endif

    // --- 1. HALKALARIN ÇİZİMİ (Full Screen Oranları) ---
#ifdef BOARD_VARIANT_LCD128
    /* 240 kartta 24px halka üçüncü halkayı yutuyordu (r3≈2px) — inceltildi. */
    int32_t rw = (int32_t)(14 * scl);
#else
    int32_t rw = (int32_t)(24 * scl);
#endif
    int32_t rcx = ox + (int32_t)(ow * 0.33f);
#ifdef BOARD_VARIANT_LCD128
    int32_t rcy = oy + (int32_t)(oh * 0.54f);   /* tarih için altta yer aç */
#else
    int32_t rcy = oy + (int32_t)(oh * 0.58f);
#endif
    int32_t br = (int32_t)(ow * 0.26f);

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.rounded = 1;
    arc.width = rw;
    arc.center.x = (float)rcx;
    arc.center.y = (float)rcy;

    arc.radius = br;
    arc.color = lv_color_make(50, 0, 15);
    arc.start_angle = 0;
    arc.end_angle = 360;
    lv_draw_arc(layer, &arc);
    arc.color = lv_color_hex(0xF20D3E);
    arc.start_angle = 270;
    arc.end_angle = 270 + (280 * 65 / 100);
    lv_draw_arc(layer, &arc);
    arc.color = lv_color_hex(0xFD1D82);
    arc.start_angle = 270 + (280 * 65 / 100);
    arc.end_angle = 270 + 280;
    lv_draw_arc(layer, &arc);

    int32_t r2 = br - rw - 4;
    arc.radius = r2;
    arc.color = lv_color_make(15, 35, 0);
    arc.start_angle = 0;
    arc.end_angle = 360;
    lv_draw_arc(layer, &arc);
    arc.color = lv_color_make(183, 255, 52);
    arc.start_angle = 270;
    arc.end_angle = 270 + 190;
    lv_draw_arc(layer, &arc);

    int32_t r3 = r2 - rw - 4;
    arc.radius = r3;
    arc.color = lv_color_make(0, 30, 50);
    arc.start_angle = 0;
    arc.end_angle = 360;
    lv_draw_arc(layer, &arc);
    arc.color = lv_color_make(63, 215, 255);
    arc.start_angle = 270;
    arc.end_angle = 270 + 130;
    lv_draw_arc(layer, &arc);

    // --- 2. METİNLERİN ÇİZİMİ ---
    lv_draw_label_dsc_t dsc_lbl;
    lv_draw_label_dsc_init(&dsc_lbl);
    dsc_lbl.text_local = 0;

    int32_t right_edge = ox + ow - (int32_t)(35 * scl);

    // Saat (Üst Sağ) - Alanı genişlettik ki ikiye bölünmesin
    dsc_lbl.font = F_TIME;
    dsc_lbl.color = lv_color_white();
    dsc_lbl.text = "10:09:30";
#ifdef BOARD_VARIANT_LCD128
    /* İkonlar kaldırıldı → saat orijinal tasarımdaki gibi SAĞA hizalı,
     * 170/00/06 kolonuyla aynı hatta (kullanıcı isteği 2026-07-13). */
    dsc_lbl.align = LV_TEXT_ALIGN_RIGHT;
    lv_area_t a_time = {right_edge - TIME_W, oy + (int32_t)(oh * 0.20f),
                        right_edge, oy + (int32_t)(oh * 0.32f)};
#else
    dsc_lbl.align = LV_TEXT_ALIGN_RIGHT;
    // x1'i iyice sola aldık (right_edge - 180) ki yazı sığsın
    lv_area_t a_time = {right_edge - TIME_W, oy + (int32_t)(oh * 0.22f), right_edge, oy + (int32_t)(oh * 0.35f)};
#endif
    lv_draw_label(layer, &dsc_lbl, &a_time);

    // İstatistik Rakamları (Dikey Hizalı)
    dsc_lbl.font = &lv_font_nunito_extrabold_60;
    int32_t stats_y = oy + (int32_t)(oh * 0.36f);
    int32_t stats_gap = STATS_GAP;
    // Rakam alanı genişliği: 120px yaptık, sağa dayalı
    int32_t lw = STAT_W;

    dsc_lbl.font = F_STAT;
    dsc_lbl.color = lv_color_make(255, 45, 112);
    dsc_lbl.text = "170";
    lv_area_t a_s1 = {right_edge - lw, stats_y, right_edge, stats_y + STAT_H};
    lv_draw_label(layer, &dsc_lbl, &a_s1);

    dsc_lbl.font = F_STAT;
    dsc_lbl.color = lv_color_make(183, 255, 52);
    dsc_lbl.text = "00";
    lv_area_t a_s2 = {right_edge - lw, stats_y + stats_gap, right_edge, stats_y + stats_gap + STAT_H};
    lv_draw_label(layer, &dsc_lbl, &a_s2);

    dsc_lbl.font = F_STAT;
    dsc_lbl.color = lv_color_make(63, 215, 255);
    dsc_lbl.text = "06";
    lv_area_t a_s3 = {right_edge - lw, stats_y + stats_gap * 2, right_edge, stats_y + stats_gap * 2 + STAT_H};
    lv_draw_label(layer, &dsc_lbl, &a_s3);

    // Tarih (Alt Orta)
    dsc_lbl.font = F_DATE;
    dsc_lbl.color = lv_color_white();
#ifdef BOARD_VARIANT_LCD128
    /* Dar yuvarlak kartta uzun tarih sığmıyor → sadece gün adı; alt-orta. */
    dsc_lbl.text = "THURSDAY";
    dsc_lbl.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t a_date = {ox, oy + (int32_t)(oh * 0.85f), ox + ow, oy + (int32_t)(oh * 0.95f)};
#else
    dsc_lbl.text = "THURSDAY, APRIL 16";
    dsc_lbl.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t a_date = {ox, oy + (int32_t)(oh * 0.90f), ox + ow, oy + (int32_t)(oh * 0.98f)};
#endif
    lv_draw_label(layer, &dsc_lbl, &a_date);

#ifndef BOARD_VARIANT_LCD128
    // --- 3. İKONLARIN ÇİZİMİ (MERKEZLEME DÜZELTİLDİ) ---
    lv_draw_rect_dsc_t bg_dsc;
    lv_draw_rect_dsc_init(&bg_dsc);
    bg_dsc.bg_color = lv_color_make(40, 40, 42);
    bg_dsc.radius = LV_RADIUS_CIRCLE;

#ifdef BOARD_VARIANT_LCD128
    /* Köşe daireleri yuvarlak kartın DIŞINA taşıyordu — içeri alındı;
     * 2. iterasyon: 36 çok küçük kaldı (ikon nokta gibi) → 44. */
    int32_t bg_sz = (int32_t)(44 * scl);
    int32_t bg_y = oy + (int32_t)(38 * scl);
    int32_t bg_pad = (int32_t)(34 * scl);
#else
    int32_t bg_sz = (int32_t)(55 * scl);
    int32_t bg_y = oy + (int32_t)(35 * scl);
    int32_t bg_pad = (int32_t)(25 * scl);
#endif

    // Sol Daire
    lv_area_t bg_l = {ox + bg_pad, bg_y, ox + bg_pad + bg_sz, bg_y + bg_sz};
    lv_draw_rect(layer, &bg_dsc, &bg_l);

    // Sağ Daire
    int32_t bg_r_x = ox + ow - bg_pad - bg_sz;
    lv_area_t bg_r = {bg_r_x, bg_y, bg_r_x + bg_sz, bg_y + bg_sz};
    lv_draw_rect(layer, &bg_dsc, &bg_r);

    // İkon Görselleri (Dairenin tam ortasına yerleştirme)
    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
#ifdef BOARD_VARIANT_LCD128
    img_dsc.scale_x = 155;
    img_dsc.scale_y = 155;
#else
    img_dsc.scale_x = 200;
    img_dsc.scale_y = 200;
#endif

    const lv_image_dsc_t *cloud = &partly_cloud_icon;
    const lv_image_dsc_t *run   = &running_man_icon;

    int32_t cx_l = (bg_l.x1 + bg_l.x2) / 2;
    int32_t cy_l = (bg_l.y1 + bg_l.y2) / 2;
    int32_t cx_r = (bg_r.x1 + bg_r.x2) / 2;
    int32_t cy_r = (bg_r.y1 + bg_r.y2) / 2;

    int32_t icon_box = (int32_t)roundf((float)bg_sz * 0.78f);
    if (icon_box < 20) icon_box = 20;

    lv_area_t cloud_area = {
        cx_l - icon_box / 2,
        cy_l - icon_box / 2,
        cx_l - icon_box / 2 + icon_box - 1,
        cy_l - icon_box / 2 + icon_box - 1
    };
    lv_area_t run_area = {
        cx_r - icon_box / 2,
        cy_r - icon_box / 2,
        cx_r - icon_box / 2 + icon_box - 1,
        cy_r - icon_box / 2 + icon_box - 1
    };

    img_dsc.src = cloud;
    lv_draw_image(layer, &img_dsc, &cloud_area);

    img_dsc.src = run;
    lv_draw_image(layer, &img_dsc, &run_area);
#endif /* !BOARD_VARIANT_LCD128 — 163px yuvarlak kartta köşe
    * ikonları sığmıyor/karmaşa yaratıyordu (3 iterasyon denendi, cihaz testi
    * 2026-07-13) → LCD128 thumb'ı ikonsuz sade tasarım. Tam ekran Activity
    * kadranı ve AMOLED thumb'ı etkilenmez. */
}

static void draw_new_thumb(lv_layer_t* layer,
                           int32_t ox, int32_t oy, int32_t ow, int32_t oh,
                           float scl)
{
    (void)scl;
    int32_t cx = ox + ow / 2;
    int32_t cy = oy + oh / 2;
    int32_t r  = (int32_t)roundf((float)LV_MIN(ow, oh) * 0.17f);

    lv_draw_rect_dsc_t c;
    lv_draw_rect_dsc_init(&c);
    c.bg_color = lv_color_make(0x6B, 0x6B, 0x72);
    c.bg_opa   = LV_OPA_COVER;
    c.radius   = LV_RADIUS_CIRCLE;
    lv_area_t a = {cx - r, cy - r, cx + r, cy + r};
    lv_draw_rect(layer, &c, &a);

    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = lv_color_white();
    ln.width = 7;
    ln.round_start = 1;
    ln.round_end = 1;

    ln.p1.x = cx - (int32_t)roundf((float)r * 0.45f);
    ln.p1.y = cy;
    ln.p2.x = cx + (int32_t)roundf((float)r * 0.45f);
    ln.p2.y = cy;
    lv_draw_line(layer, &ln);

    ln.p1.x = cx;
    ln.p1.y = cy - (int32_t)roundf((float)r * 0.45f);
    ln.p2.x = cx;
    ln.p2.y = cy + (int32_t)roundf((float)r * 0.45f);
    lv_draw_line(layer, &ln);
}

static void draw_delete_overlay(lv_layer_t* layer, const lv_area_t* /*card_area*/, int32_t /*radius*/)
{
    int32_t rx, ry, rw, rh;
    selector_trash_rect(&rx, &ry, &rw, &rh);
    int32_t cx = rx + rw / 2;
    int32_t cy = ry + rh / 2;

    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text_local = 0;
    d.align = LV_TEXT_ALIGN_CENTER;

#ifdef BOARD_VARIANT_LCD128
    d.font = &lv_font_montserrat_28;
#else
    d.font = &lv_font_montserrat_48;
#endif
    d.color = lv_color_make(0xFF, 0x4D, 0x67);
    d.text = LV_SYMBOL_TRASH;
    lv_area_t a1 = {cx - 56, cy - 66, cx + 56, cy + 10};
    lv_draw_label(layer, &d, &a1);

#ifdef BOARD_VARIANT_LCD128
    d.font = &lv_font_montserrat_16;
#else
    d.font = &lv_font_montserrat_28;
#endif
    d.color = lv_color_white();
    d.text = "Delete";
    lv_area_t a2 = {cx - 66, cy + 12, cx + 66, cy + 58};
    lv_draw_label(layer, &d, &a2);
}

// ── Snapshot helpers ───────────────────────────────────────────────────────────

/**
 * Pre-render the analog face static elements (background + ticks + notif dot + logo)
 * at full screen size (SCR_W × SCR_H) into PSRAM.
 * Drawn ONCE on app start; per-frame render only draws the dynamic hands on top.
 * Reduces analog face per-frame GPU cost from ~130 draw ops to ~8 draw ops.
 */
static void build_analog_base(void)
{
    if (s_base_valid) return;

    lv_color_format_t cf = LV_COLOR_FORMAT_RGB565;
    uint32_t stride   = lv_draw_buf_width_to_stride(SCR_W, cf);
    uint32_t buf_size = stride * (uint32_t)SCR_H;

    if (!s_base_raw) {
        s_base_raw = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_base_raw) {
            ESP_LOGE(TAG, "Analog base alloc FAIL");
            return;
        }
    }

    lv_result_t r = lv_draw_buf_init(&s_base_buf, SCR_W, SCR_H, cf,
                                      stride, s_base_raw, buf_size);
    if (r != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Analog base draw_buf_init FAIL");
        return;
    }

    lv_obj_t *canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_draw_buf(canvas, &s_base_buf);
    lv_obj_set_pos(canvas, -(SCR_W + 200), -(SCR_H + 200));

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // Black background
    {
        lv_draw_rect_dsc_t bg;
        lv_draw_rect_dsc_init(&bg);
        bg.bg_color = lv_color_black();
        bg.bg_opa   = LV_OPA_COVER;
        lv_area_t full = {0, 0, SCR_W - 1, SCR_H - 1};
        lv_draw_rect(&layer, &bg, &full);
    }

    // All 120 tick marks at full scale
    float scale = 1.0f;
    float w_in  = (float)(SCR_W - 2 * SAFE_INSET) * scale;
    float h_in  = (float)(SCR_H - 2 * SAFE_INSET) * scale;
    float ox0   = ((float)SCR_W - w_in) / 2.0f + 2.0f;
    float oy0   = ((float)SCR_H - h_in) / 2.0f;
    int32_t cx  = SCR_W / 2 + 2;
    (void)(SCR_H / 2 + CENTER_Y_ADJ);  // cy not needed in static base

    for (int i = 0; i < 120; i++) {
        draw_tick(&layer, i, w_in, h_in,
                  (float)(TICK_MARGIN + 4) * scale, (float)CORNER_R * scale,
                  28.0f * scale, 12.0f * scale,
                  (int)(10 * scale), (int)(2 * scale),
                  ox0, oy0);
    }

    // Notification dot (static — always red at 12 o'clock)
    int32_t notif_y = (int32_t)oy0 + (int32_t)((TICK_MARGIN + 4) * scale + 5 * scale);
    draw_circle(&layer, cx, notif_y, (int32_t)(5.0f * scale * 0.8f),
                lv_color_make(0xFF, 0x3B, 0x30));

    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_delete(canvas);

    // Logo — manual blit into PSRAM base buffer (bypasses LVGL A8 decoder).
    // Must be done AFTER finish_layer so the LVGL batch has flushed.
    if (!s_logo_metrics_ready) init_logo_metrics();
    {
#ifdef BOARD_VARIANT_LCD128
        /* 240 ekranda logo (140px asset, 1:1 blit) merkeze taşıyordu —
         * akrep/yelkovan pivotuna değiyordu (cihaz testi 2026-07-13).
         * Tick'lerin hemen altına, pivottan uzağa çekildi. */
        /* Blit bu varyantta LOGO_SCALE_256/256 küçültüyor → metrikler de
         * aynı oranda. DROP=44: logo üstü 12-tick'in altından başlar
         * (cihaz testiyle ayarlandı), pivota (y~114) değmez. */
        const float LOGO_DROP = 44.0f;
        int32_t logo_x = cx - ((s_logo_center_x * LOGO_SCALE_256) >> 8);
        int32_t logo_y = (int32_t)roundf((float)notif_y + LOGO_DROP * scale)
                         - ((s_logo_min_y * LOGO_SCALE_256) >> 8);
#else
        const float LOGO_DROP = 55.0f;
        int32_t logo_x = cx - s_logo_center_x;
        int32_t logo_y = (int32_t)roundf((float)notif_y + LOGO_DROP * scale) - s_logo_min_y;
#endif
        blit_corezzle_logo_rgb565(s_base_raw, stride, SCR_W, SCR_H, logo_x, logo_y);
    }

    s_base_valid = true;
    ESP_LOGI(TAG, "Analog base built (%dx%d, %u bytes PSRAM)", SCR_W, SCR_H, buf_size);
}

/**
 * Pre-render a face thumbnail (SNAP_W×SNAP_H) into PSRAM using lv_canvas_init_layer.
 * lv_canvas_init_layer works without display dependency — it writes directly to the draw_buf.
 * Called on selector entry; result used by draw_selector_side_thumb and center card blit.
 */
static void build_face_snapshot(face_index_t face)
{
    if (s_snap_valid[face]) return;

    lv_color_format_t cf = LV_COLOR_FORMAT_RGB565;
    uint32_t stride   = lv_draw_buf_width_to_stride(SNAP_W, cf);
    uint32_t buf_size = stride * (uint32_t)SNAP_H;

    if (!s_snap_raw_ptr[face]) {
        s_snap_raw_ptr[face] = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_snap_raw_ptr[face]) {
            ESP_LOGE(TAG, "Snap alloc FAIL face=%d", (int)face);
            return;
        }
    }

    lv_result_t r = lv_draw_buf_init(&s_snap_buf_obj[face], SNAP_W, SNAP_H, cf,
                                      stride, s_snap_raw_ptr[face], buf_size);
    if (r != LV_RESULT_OK) {
        ESP_LOGE(TAG, "draw_buf_init FAIL face=%d", (int)face);
        return;
    }

    // Canvas on active screen — lv_canvas_init_layer writes directly to draw_buf
    lv_obj_t *canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_draw_buf(canvas, &s_snap_buf_obj[face]);
    lv_obj_set_pos(canvas, -(SNAP_W + 200), -(SNAP_H + 200));  // well off-screen

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // Step 1: clear entire buffer to pure black (heap_caps_malloc gives garbage data;
    //          lv_draw_rect with radius only fills inside the rounded rect — corners stay garbage)
    {
        lv_draw_rect_dsc_t clr;
        lv_draw_rect_dsc_init(&clr);
        clr.bg_color = lv_color_black();
        clr.bg_opa   = LV_OPA_COVER;
        clr.radius   = 0;
        lv_area_t full = {0, 0, SNAP_W - 1, SNAP_H - 1};
        lv_draw_rect(&layer, &clr, &full);
    }
    // Step 2: card background with rounded corners + border
    {
        lv_draw_rect_dsc_t bg;
        lv_draw_rect_dsc_init(&bg);
        bg.bg_color = lv_color_black(); // dark card bg (slightly visible on AMOLED)
        bg.bg_opa       = LV_OPA_COVER;
        bg.radius       = (int32_t)roundf(CORNER_R * SEL_CARD_SCALE);
        bg.border_color = lv_color_make(0x55, 0x55, 0x60);
        bg.border_width = 2;
        bg.border_opa   = LV_OPA_COVER;
        lv_area_t full  = {0, 0, SNAP_W - 1, SNAP_H - 1};
        lv_draw_rect(&layer, &bg, &full);
    }

    // Face content
    switch (face) {
        case FACE_ANALOG:
            draw_analog_thumb(&layer, 0, 0, SNAP_W, SNAP_H, SEL_CARD_SCALE);
            break;
        case FACE_DIGITAL:
            draw_digital_thumb(&layer, 0, 0, SNAP_W, SNAP_H, SEL_CARD_SCALE);
            break;
        case FACE_ACTIVITY:
            draw_activity_thumb(&layer, 0, 0, SNAP_W, SNAP_H, SEL_CARD_SCALE);
            break;
        case FACE_IO_CONTROL:                                              /* v1.2.1 */
            draw_io_control_thumb(&layer, 0, 0, SNAP_W, SNAP_H, SEL_CARD_SCALE);
            break;
        case FACE_CUSTOM_0:
        case FACE_CUSTOM_1:
        {
            // Scale the full-size custom image (410×502) into the SNAP canvas (279×341).
            // "cover": use max scale so the image fills every pixel; edges are cropped.
            int rel = (int)face - FACE_BUILTIN_COUNT;
            if (rel >= 0 && rel < s_custom_face_count) {
                int32_t sc_x = (256 * SNAP_W) / FACE_UPLOAD_W;   // 256*279/410 = 174
                int32_t sc_y = (256 * SNAP_H) / FACE_UPLOAD_H;   // 256*341/502 = 173
                int32_t sc   = (sc_x > sc_y) ? sc_x : sc_y;      // cover → 174
                int32_t sw_s = (FACE_UPLOAD_W * sc) / 256;        // 410*174/256 = 278
                int32_t sh_s = (FACE_UPLOAD_H * sc) / 256;        // 502*174/256 = 341
                int32_t ox_s = (SNAP_W - sw_s) / 2;               // center horizontally
                int32_t oy_s = (SNAP_H - sh_s) / 2;               // center vertically
                lv_draw_image_dsc_t img;
                lv_draw_image_dsc_init(&img);
                img.src           = &s_custom_buf_obj[rel];
                img.opa           = LV_OPA_COVER;
                img.scale_x       = (uint32_t)sc;
                img.scale_y       = (uint32_t)sc;
                img.image_area.x1 = ox_s;
                img.image_area.y1 = oy_s;
                img.image_area.x2 = ox_s + sw_s - 1;
                img.image_area.y2 = oy_s + sh_s - 1;
                lv_area_t snap_full = {0, 0, SNAP_W - 1, SNAP_H - 1};
                lv_draw_image(&layer, &img, &snap_full);
            }
            break;
        }
        default: break;
    }

    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_delete(canvas);

    s_snap_valid[face] = true;
    ESP_LOGI(TAG, "Snapshot OK face=%d (%dx%d stride=%u)", (int)face, SNAP_W, SNAP_H, stride);
}

static void free_face_snapshots(void)
{
    for (int i = 0; i < (int)FACE_SLOT_COUNT; i++) {
        if (s_snap_raw_ptr[i]) {
            heap_caps_free(s_snap_raw_ptr[i]);
            s_snap_raw_ptr[i] = NULL;
        }
        s_snap_valid[i] = false;
    }
    // Also free the analog static base
    if (s_base_raw) {
        heap_caps_free(s_base_raw);
        s_base_raw  = NULL;
    }
    s_base_valid = false;
}

static void draw_selector_side_thumb(lv_layer_t* layer,
                                      float d_cards, int32_t x_off,
                                      face_index_t face)
{
    if (fabsf(d_cards) > 1.5f) return;

    // All cards same scale — no deformation
    float s   = SEL_CARD_SCALE;
    int32_t sw    = (int32_t)roundf((float)SCR_W * s);
    int32_t sh    = (int32_t)roundf((float)SCR_H * s);
    int32_t cy_c  = SCR_H/2 + (int32_t)roundf(s_draw_y_off);
    int32_t oy    = cy_c - sh/2;
    // d_cards = (side_card_idx - s_sel_visual_pos), already encodes full offset from camera.
    // Do NOT add x_off — that's only for the active card and would double-count.
    int32_t cx_c  = SCR_W/2 + (int32_t)roundf(d_cards * SEL_CARD_SPACING);
    int32_t ox    = cx_c - sw/2;

    float t   = clampf_(fabsf(d_cards), 0.0f, 1.0f);
    int32_t rad = (int32_t)roundf(CORNER_R * s);

    lv_area_t card_area = {ox, oy, ox+sw-1, oy+sh-1};

    // 1. Card background — clearly different from AMOLED black so rounded frame is visible
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    // bg.bg_color     = lv_color_make(0x28, 0x28, 0x2C);
    bg.bg_opa = LV_OPA_TRANSP;
    bg.radius       = rad;
    bg.border_color = lv_color_make(0x55, 0x55, 0x60);
    bg.border_width = 2;
    bg.border_opa   = LV_OPA_COVER;
    lv_draw_rect(layer, &bg, &card_area);

    // 2. Face thumbnail — custom and "Yeni" are drawn live
    if ((int)face == face_new_idx())
    {
        draw_new_thumb(layer, ox, oy, sw, sh, s);
    }
    else if (face_is_custom((int)face))
    {
        int rel = custom_rel_for_face((int)face);
        int slot = custom_slot_for_face((int)face);
        if (rel >= 0 && slot >= 0 && slot < FACE_UPLOAD_MAX_SLOTS) {
            // Lazily build thumbnail on first draw (corner-masked, pre-scaled)
            if (!s_custom_thumb_raw[slot]) {
                const face_upload_slot_t* u = face_upload_get_slot(slot);
                if (u && u->buf && u->valid)
                    ensure_custom_thumb(slot, (const uint16_t*)u->buf);
            }
            lv_draw_image_dsc_t img_dsc;
            lv_draw_image_dsc_init(&img_dsc);
            img_dsc.opa = LV_OPA_COVER;
            if (s_custom_thumb_raw[slot]) {
                // Pre-scaled 279×341 thumbnail with baked-in corner mask → fits card exactly
                img_dsc.src        = &s_custom_thumb_obj[slot];
                img_dsc.image_area = card_area;
                lv_draw_image(layer, &img_dsc, &card_area);
            } else {
                // Fallback: full 410×502 image, cover-scale into card, clip to card bounds
                img_dsc.src = &s_custom_buf_obj[rel];
                int32_t sc_x = (256 * sw) / FACE_UPLOAD_W;
                int32_t sc_y = (256 * sh) / FACE_UPLOAD_H;
                int32_t sc   = sc_x > sc_y ? sc_x : sc_y;
                int32_t rw   = (FACE_UPLOAD_W * sc) / 256;
                int32_t rh   = (FACE_UPLOAD_H * sc) / 256;
                lv_area_t positioned = {
                    ox + (sw - rw) / 2, oy + (sh - rh) / 2,
                    ox + (sw - rw) / 2 + rw - 1, oy + (sh - rh) / 2 + rh - 1
                };
                img_dsc.image_area = positioned;
                lv_draw_image(layer, &img_dsc, &card_area);
            }
        }
    }
    else if (s_snap_valid[face] && s_snap_raw_ptr[face])
    {
        lv_draw_image_dsc_t img_dsc;
        lv_draw_image_dsc_init(&img_dsc);
        img_dsc.src = &s_snap_buf_obj[face]; // Pointer adresi
        img_dsc.opa = LV_OPA_COVER;
        img_dsc.image_area = card_area;
        lv_draw_image(layer, &img_dsc, &card_area);
    }
    else
    {
        switch (face) {
        case FACE_ANALOG:
            draw_analog_thumb(layer, ox, oy, sw, sh, s);
            break;
        case FACE_DIGITAL:
            draw_digital_thumb(layer, ox, oy, sw, sh, s);
            break;
        case FACE_ACTIVITY:
            draw_activity_thumb(layer, ox, oy, sw, sh, s);
            break;
        case FACE_IO_CONTROL:                                /* v1.2.1 */
            draw_io_control_thumb(layer, ox, oy, sw, sh, s);
            break;
        default:
            break;
        }
    }

    // 3. Dim overlay — black alpha over content (side cards are darker)
    {
        lv_draw_rect_dsc_t dim;
        lv_draw_rect_dsc_init(&dim);
        dim.bg_color = lv_color_black();
        dim.bg_opa   = (lv_opa_t)(int)(LV_OPA_COVER * 0.55f * t);
        dim.radius   = rad;
        lv_draw_rect(layer, &dim, &card_area);
    }

    // 4. Border redrawn on top of dim so it stays crisp and always visible
    {
        lv_draw_rect_dsc_t bdr;
        lv_draw_rect_dsc_init(&bdr);
        bdr.bg_opa      = LV_OPA_TRANSP;
        bdr.radius      = rad;
        bdr.border_color = lv_color_make(0x60, 0x60, 0x70);
        bdr.border_width = 2;
        bdr.border_opa   = (lv_opa_t)(int)(LV_OPA_COVER * (1.0f - 0.3f * t));
        lv_draw_rect(layer, &bdr, &card_area);
    }
}

// ─── Main draw callback ───────────────────────────────────────────────────────
// ─── Main draw callback ───────────────────────────────────────────────────────
static void face_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);

    /* --- KUSURSUZ ZAMAN HESAPLAMA (DOUBLE PRECISION) ---
     *
     * v1.4.55 (2026-05-19) — audit orta öncelik #1a: localtime_r minute-gated
     * cache. localtime_r DST + timezone hesaplaması ~10-30 µs/çağrı, ve
     * face_draw_cb her LVGL render frame'inde (anim_timer_cb invalidate ile
     * ~30 ms periyot) çağrılıyor. tm_min/tm_hour sadece dakika geçişinde
     * değiştiği için her frame yeniden hesaplamak boşa CPU.
     *
     * Cache stratejisi:
     *   - gettimeofday: her frame (ucuz, saniye akışkan render için şart)
     *   - localtime_r: sadece minute boundary geçince (tv.tv_sec / 60 değişimi)
     *   - sec_a hesaplaması: gettimeofday'den direkt (cache yok)
     *   - min_a / hour_a: cached tm + s_flow karışımı (her frame doğru güncel)
     *
     * Net kazanç: ~10-30 µs/frame (frame budget 33 ms → %0.05-0.1, görünmez)
     * ama doğru pattern + analog face frame budget'ı boşa harcamıyor. */
    float sec_a, min_a, hour_a;
    struct tm ti;

    if (s_face_mode == MODE_SELECTOR)
    {
        // Önizleme modunda zaman dondurulur (10:09:30)
        sec_a = 30.0f / 60.0f * (2.0f * (float)M_PI);
        min_a = 9.5f / 60.0f * (2.0f * (float)M_PI);
        hour_a = (10.0f + 9.5f / 60.0f) / 12.0f * (2.0f * (float)M_PI);
    }
    else
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);

        /* v1.4.55: minute-gated localtime_r cache */
        static time_t   s_cached_min_sec = 0;        /* son cache'lenen tv.tv_sec */
        static struct tm s_cached_tm;
        if ((tv.tv_sec / 60) != (s_cached_min_sec / 60)) {
            localtime_r(&tv.tv_sec, &s_cached_tm);
            s_cached_min_sec = tv.tv_sec;
        }
        ti = s_cached_tm;

        // Kritik Düzeltme: Saniye sıçramasını önlemek için saniye ve mikrosaniyeyi
        // modulo işlemine sokarak tek bir akışkan sayı elde ediyoruz.
        // Hassasiyet kaybını önlemek için double kullanıyoruz.
        double now_f = (double)(tv.tv_sec % 86400) + (double)tv.tv_usec / 1000000.0;

        // Saniye açısı (0-60 arası sürekli artan double üzerinden)
        double s_flow = fmod(now_f, 60.0);
        sec_a = (float)(s_flow / 60.0 * (2.0 * M_PI));

        // Dakika açısı (ti.tm_min ve s_flow birleşimi)
        double m_flow = (double)ti.tm_min + s_flow / 60.0;
        min_a = (float)(m_flow / 60.0 * (2.0 * M_PI));

        // Saat açısı
        double h_flow = (double)(ti.tm_hour % 12) + m_flow / 60.0;
        hour_a = (float)(h_flow / 12.0 * (2.0 * M_PI));
    }

    /* 1. Ekranı Temizle (Siyah Arka Plan) */
    lv_draw_rect_dsc_t bg_dsc;
    lv_draw_rect_dsc_init(&bg_dsc);
    bg_dsc.bg_color = lv_color_black();
    lv_area_t full_scr = {0, 0, SCR_W - 1, SCR_H - 1};
    lv_draw_rect(layer, &bg_dsc, &full_scr);

    float scale = (s_face_mode == MODE_SELECTOR) ? SEL_CARD_SCALE : s_draw_scale;
    if (scale < 0.68f) scale = 0.68f;
    if (scale > 1.0f)  scale = 1.0f;
    int32_t x_off = (s_face_mode == MODE_SELECTOR) ? (int32_t)roundf(s_frame_x_off) : 0;

    int32_t dw = (int32_t)(SCR_W * scale);
    int32_t dh = (int32_t)(SCR_H * scale);
    int32_t dx = (SCR_W - dw) / 2 + x_off;
    int32_t dy = (SCR_H - dh) / 2 + (int32_t)s_draw_y_off;

    // =========================================================================
    // 1b. SELECTOR: all cards rendered as snapshots for smooth drag
    // =========================================================================
    if (s_face_mode == MODE_SELECTOR) {
        // Side cards (use snapshots if available, else live thumb draw)
        draw_selector_side_thumb(layer, s_frame_left_d,  x_off, s_frame_left_face);
        draw_selector_side_thumb(layer, s_frame_right_d, x_off, s_frame_right_face);

        lv_area_t card_area = {dx, dy, dx + dw - 1, dy + dh - 1};
        int32_t   card_rad  = (int32_t)roundf(CORNER_R * scale);
        int32_t   delete_lift = (face_is_custom((int)s_frame_center))
            ? (int32_t)roundf((float)selector_delete_lift_px(scale) * (s_sel_delete_mode ? 1.0f : s_sel_delete_reveal))
            : 0;
        if (delete_lift > 0) {
            card_area.y1 -= delete_lift;
            card_area.y2 -= delete_lift;
        }
        int32_t cdx = card_area.x1;
        int32_t cdy = card_area.y1;

        // Center card: custom/new drawn live; built-ins can use snapshot blit.
        if ((int)s_frame_center == face_new_idx()) {
            draw_new_thumb(layer, cdx, cdy, dw, dh, scale);

            /* FACE_NEW center: dim + border continuous-with-side (see snapshot blit). */
            float t_center_n = clampf_(fabsf(s_frame_x_off) / SEL_CARD_SPACING, 0.0f, 1.0f);
            if (t_center_n > 0.001f) {
                lv_draw_rect_dsc_t dim;
                lv_draw_rect_dsc_init(&dim);
                dim.bg_color = lv_color_black();
                dim.bg_opa   = (lv_opa_t)(int)(LV_OPA_COVER * 0.55f * t_center_n);
                dim.radius   = card_rad;
                lv_draw_rect(layer, &dim, &card_area);
            }

            lv_draw_rect_dsc_t bdr;
            lv_draw_rect_dsc_init(&bdr);
            bdr.bg_opa       = LV_OPA_TRANSP;
            bdr.radius       = card_rad;
            bdr.border_color = lv_color_make(0x60, 0x60, 0x70);
            bdr.border_width = 2;
            bdr.border_opa   = (lv_opa_t)(int)(LV_OPA_COVER * (1.0f - 0.3f * t_center_n));
            lv_draw_rect(layer, &bdr, &card_area);
            return;
        }

        if (face_is_custom((int)s_frame_center)) {
            int rel = custom_rel_for_face((int)s_frame_center);
            int slot = custom_slot_for_face((int)s_frame_center);
            if (rel >= 0 && slot >= 0 && slot < FACE_UPLOAD_MAX_SLOTS) {
                // Lazily build thumbnail on first draw
                if (!s_custom_thumb_raw[slot]) {
                    const face_upload_slot_t* u = face_upload_get_slot(slot);
                    if (u && u->buf && u->valid)
                        ensure_custom_thumb(slot, (const uint16_t*)u->buf);
                }
                lv_draw_image_dsc_t img_dsc;
                lv_draw_image_dsc_init(&img_dsc);
                img_dsc.opa = LV_OPA_COVER;
                if (s_custom_thumb_raw[slot]) {
                    img_dsc.src        = &s_custom_thumb_obj[slot];
                    img_dsc.image_area = card_area;
                    lv_draw_image(layer, &img_dsc, &card_area);
                } else {
                    img_dsc.src = &s_custom_buf_obj[rel];
                    int32_t sc_x = (256 * dw) / FACE_UPLOAD_W;
                    int32_t sc_y = (256 * dh) / FACE_UPLOAD_H;
                    int32_t sc   = sc_x > sc_y ? sc_x : sc_y;
                    int32_t rw   = (FACE_UPLOAD_W * sc) / 256;
                    int32_t rh   = (FACE_UPLOAD_H * sc) / 256;
                    lv_area_t positioned = {
                        cdx + (dw - rw) / 2, cdy + (dh - rh) / 2,
                        cdx + (dw - rw) / 2 + rw - 1, cdy + (dh - rh) / 2 + rh - 1
                    };
                    img_dsc.image_area = positioned;
                    lv_draw_image(layer, &img_dsc, &card_area);
                }

                if ((s_sel_delete_mode || s_sel_delete_reveal > 0.01f) && face_is_custom((int)s_frame_center)) {
                    draw_delete_overlay(layer, &card_area, card_rad);
                }

                /* Custom face center: dim + border continuous-with-side (see snapshot blit). */
                float t_center_c = clampf_(fabsf(s_frame_x_off) / SEL_CARD_SPACING, 0.0f, 1.0f);
                if (t_center_c > 0.001f) {
                    lv_draw_rect_dsc_t dim;
                    lv_draw_rect_dsc_init(&dim);
                    dim.bg_color = lv_color_black();
                    dim.bg_opa   = (lv_opa_t)(int)(LV_OPA_COVER * 0.55f * t_center_c);
                    dim.radius   = card_rad;
                    lv_draw_rect(layer, &dim, &card_area);
                }

                lv_draw_rect_dsc_t bdr;
                lv_draw_rect_dsc_init(&bdr);
                bdr.bg_opa       = LV_OPA_TRANSP;
                bdr.radius       = card_rad;
                bdr.border_color = lv_color_make(0x60, 0x60, 0x70);
                bdr.border_width = 2;
                bdr.border_opa   = (lv_opa_t)(int)(LV_OPA_COVER * (1.0f - 0.3f * t_center_c));
                lv_draw_rect(layer, &bdr, &card_area);
                return;
            }
        }

        // Built-in center card: snapshot blit fast-path
        if (s_snap_valid[s_frame_center] && s_snap_raw_ptr[s_frame_center]) {
            lv_draw_image_dsc_t img_dsc;
            lv_draw_image_dsc_init(&img_dsc);
            img_dsc.src        = &s_snap_buf_obj[s_frame_center];
            img_dsc.opa        = LV_OPA_COVER;
            img_dsc.image_area = card_area;
            lv_draw_image(layer, &img_dsc, &card_area);

            /* Center-card dim overlay — visual_pos kart-merkezini geçtiği an center
             * face flip oluyor. Eski hal: side kart dim=0.55*t*255 (t∈[0.5,1.5]),
             * center kart dim=0 → crossover'da bir face'in dimi ~70'ten 0'a, diğeri
             * 0'dan ~70'e atlıyordu = parlaklık snap'i = "tık takılma".
             * Şimdi center kartın dimi t_center = |x_off|/spacing (∈[0,0.5]) ile
             * scale ediliyor → crossover sınırında her iki face de aynı parlaklıkta
             * (~70), continuous brightness curve. Apple Watch carousel hissi. */
            float t_center = clampf_(fabsf(s_frame_x_off) / SEL_CARD_SPACING, 0.0f, 1.0f);
            if (t_center > 0.001f) {
                lv_draw_rect_dsc_t dim;
                lv_draw_rect_dsc_init(&dim);
                dim.bg_color = lv_color_black();
                dim.bg_opa   = (lv_opa_t)(int)(LV_OPA_COVER * 0.55f * t_center);
                dim.radius   = card_rad;
                lv_draw_rect(layer, &dim, &card_area);
            }

            /* Border: side kart formülü ile uyumlu (same color, opa=1-0.3*t).
             * Önceki sabit COVER + farklı color (0x55 vs 0x60) crossover'da hop
             * yapıyordu; şimdi continuous. */
            lv_draw_rect_dsc_t bdr;
            lv_draw_rect_dsc_init(&bdr);
            bdr.bg_opa       = LV_OPA_TRANSP;
            bdr.radius       = card_rad;
            bdr.border_color = lv_color_make(0x60, 0x60, 0x70);
            bdr.border_width = 2;
            bdr.border_opa   = (lv_opa_t)(int)(LV_OPA_COVER * (1.0f - 0.3f * t_center));
            lv_draw_rect(layer, &bdr, &card_area);
            return;  // Snapshot blitted — skip all live rendering
        }

        // Fallback (snapshot not ready yet): draw card background + fall through to live render
        lv_draw_rect_dsc_t card_bg;
        lv_draw_rect_dsc_init(&card_bg);
        card_bg.bg_color     = lv_color_black();
        card_bg.bg_opa       = LV_OPA_COVER;
        card_bg.radius       = card_rad;
        card_bg.border_color = lv_color_make(0x55, 0x55, 0x60);
        card_bg.border_width = 2;
        card_bg.border_opa   = LV_OPA_COVER;
        lv_draw_rect(layer, &card_bg, &card_area);
    }

    if (face_is_custom((int)s_face_index)) {
        int rel = custom_rel_for_face((int)s_face_index);
        int slot = custom_slot_for_face((int)s_face_index);
        if (rel >= 0 && slot >= 0 && slot < FACE_UPLOAD_MAX_SLOTS) {
            lv_draw_image_dsc_t img;
            lv_draw_image_dsc_init(&img);
            img.src     = &s_custom_buf_obj[rel];
            img.opa     = LV_OPA_COVER;
            float scx = (float)dw / (float)FACE_UPLOAD_W;
            float scy = (float)dh / (float)FACE_UPLOAD_H;
            float sc = scx < scy ? scx : scy;
            if (sc > 1.0f) sc = 1.0f;
            int32_t rw = (int32_t)roundf((float)FACE_UPLOAD_W * sc);
            int32_t rh = (int32_t)roundf((float)FACE_UPLOAD_H * sc);
            int32_t px = dx + (dw - rw) / 2;
            int32_t py = dy + (dh - rh) / 2;
            img.scale_x = (int32_t)roundf(sc * (float)LV_SCALE_NONE);
            img.scale_y = (int32_t)roundf(sc * (float)LV_SCALE_NONE);
            img.pivot.x = 0;
            img.pivot.y = 0;
            lv_area_t full_img = { px, py, px + FACE_UPLOAD_W - 1, py + FACE_UPLOAD_H - 1 };
            lv_draw_image(layer, &img, &full_img);
        }
        return;
    }

    if ((int)s_face_index == face_new_idx()) {
        draw_new_thumb(layer, dx, dy, dw, dh, scale);
        return;
    }

    // =========================================================================
    // 2. AKTİVİTE EKRANI
    // =========================================================================
    if (s_face_index == FACE_ACTIVITY)
    {
        int32_t rcx = dx + (int32_t)(dw * 0.33f);
        int32_t rcy = dy + (int32_t)(dh * (s_face_mode == MODE_SELECTOR ? 0.62f : 0.58f));
#ifdef BOARD_VARIANT_LCD128
        /* 240 ekranda 24px halka üçüncü halkayı yutuyor (r3~2px) —
         * thumb'daki fix'in tam ekran karşılığı. */
        int32_t rw = (int32_t)(14 * scale);
#else
        int32_t rw = (int32_t)(24 * scale);
#endif
        int32_t br = (int32_t)(dw * 0.26f);

        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.rounded = 1;
        arc.width = rw;
        arc.center.x = rcx;
        arc.center.y = rcy;

        arc.color = lv_color_make(50, 0, 15);
        arc.start_angle = 0;
        arc.end_angle = 360;
        arc.radius = br;
        lv_draw_arc(layer, &arc);
        int32_t move_seg1 = (280 * 65) / 100;
        arc.color = lv_color_hex(0xF20D3E);
        arc.start_angle = 270;
        arc.end_angle = 270 + move_seg1;
        lv_draw_arc(layer, &arc);
        arc.color = lv_color_hex(0xFD1D82);
        arc.start_angle = 270 + move_seg1;
        arc.end_angle = 270 + 280;
        lv_draw_arc(layer, &arc);

        int32_t r2 = br - rw - 4;
        arc.radius = r2;
        arc.color = lv_color_make(15, 35, 0);
        arc.start_angle = 0;
        arc.end_angle = 360;
        lv_draw_arc(layer, &arc);
        arc.color = lv_color_make(183, 255, 52);
        arc.start_angle = 270;
        arc.end_angle = 270 + 190;
        lv_draw_arc(layer, &arc);

        int32_t r3 = r2 - rw - 4;
        arc.radius = r3;
        arc.color = lv_color_make(0, 30, 50);
        arc.start_angle = 0;
        arc.end_angle = 360;
        lv_draw_arc(layer, &arc);
        arc.color = lv_color_make(63, 215, 255);
        arc.start_angle = 270;
        arc.end_angle = 270 + 130;
        lv_draw_arc(layer, &arc);
        return;
    }

    // =========================================================================
    // 3. ANALOG SAAT EKRANI
    // =========================================================================
    if (s_face_index == FACE_ANALOG)
    {
        float w_in  = (float)(SCR_W - 2 * SAFE_INSET) * scale;
        float h_in  = (float)(SCR_H - 2 * SAFE_INSET) * scale;
        int32_t cx  = SCR_W / 2 + (int32_t)roundf(2.0f + x_off);
        int32_t cy  = SCR_H / 2 + (int32_t)roundf(s_draw_y_off) + CENTER_Y_ADJ;
        int32_t ox0 = (SCR_W - (int32_t)w_in) / 2 + (int32_t)roundf(2.0f + x_off);
        int32_t oy0 = (SCR_H - (int32_t)h_in) / 2 + (int32_t)roundf(s_draw_y_off);

        if (s_face_mode == MODE_NORMAL && s_base_valid) {
            // ── FAST PATH: blit pre-rendered static base, then draw only hands ──────
            lv_area_t base_area = {0, 0, SCR_W - 1, SCR_H - 1};
            lv_draw_image_dsc_t base_dsc;
            lv_draw_image_dsc_init(&base_dsc);
            base_dsc.src        = &s_base_buf;
            base_dsc.opa        = LV_OPA_COVER;
            base_dsc.image_area = base_area;
            lv_draw_image(layer, &base_dsc, &base_area);
        } else if (s_face_mode == MODE_NORMAL) {
            // ── SLOW FALLBACK: draw everything manually (base not ready yet) ────────
            for (int i = 0; i < 120; i++) {
                draw_tick(layer, i, w_in, h_in,
                          (TICK_MARGIN + 4) * scale, CORNER_R * scale,
                          28 * scale, 12 * scale,
                          (int)(10 * scale), (int)(2 * scale),
                          (float)ox0, (float)oy0);
            }
            int32_t notif_y_fb = oy0 + (int32_t)((TICK_MARGIN + 4) * scale + 5 * scale);
            draw_circle(layer, cx, notif_y_fb, (int32_t)(5 * scale * 0.8f),
                        lv_color_make(0xFF, 0x3B, 0x30));
            /* Logo skipped on this fallback — LVGL A8 decoder can't allocate
             * a 24.5 KB decode buffer from regular heap. The base-buffer
             * path (blit_corezzle_logo_rgb565) handles it instead. */
        } else {
            // ── SELECTOR FALLBACK (snapshot not ready): 12 ticks only ──────────────
            for (int i = 0; i < 12; i++) {
                draw_tick(layer, i * 10, w_in, h_in,
                          (TICK_MARGIN + 4) * scale, CORNER_R * scale,
                          28 * scale, 12 * scale,
                          (int)(10 * scale), (int)(2 * scale),
                          (float)ox0, (float)oy0);
            }
        }

        // ── DYNAMIC: hands + center cap (always drawn in normal mode) ─────────────
        // (In selector mode with valid snapshot, this code is never reached — returned early)
        draw_hand_pill(layer, hour_a,
                       (int32_t)(H_TIP * scale), (int32_t)(16 * scale),
                       (int32_t)(10 * scale), cx, cy);
        draw_hand_pill(layer, min_a,
                       (int32_t)(M_TIP * scale), (int32_t)(12 * scale),
                       (int32_t)(7 * scale), cx, cy);

        lv_draw_line_dsc_t s_hand;
        lv_draw_line_dsc_init(&s_hand);
        s_hand.color       = lv_color_make(0xFF, 0x3B, 0x30);
        s_hand.width       = (int)roundf(S_W * scale) < 1 ? 1 : (int)roundf(S_W * scale);
        s_hand.round_start = 1;
        s_hand.round_end   = 1;
        s_hand.p1.x = (lv_value_precise_t)px_(sec_a, -(float)S_TAIL * scale, cx);
        s_hand.p1.y = (lv_value_precise_t)py_(sec_a, -(float)S_TAIL * scale, cy);
        s_hand.p2.x = (lv_value_precise_t)px_(sec_a, 210.0f * scale, cx);
        s_hand.p2.y = (lv_value_precise_t)py_(sec_a, 210.0f * scale, cy);
        lv_draw_line(layer, &s_hand);

        // Center cap (drawn over hands)
        draw_circle(layer, cx, cy, (int32_t)(5.0f * scale), lv_color_white());
        draw_circle(layer, cx, cy, (int32_t)(2.0f * scale), lv_color_black());
    }
}

// ─── Timer ────────────────────────────────────────────────────────────────────
static void anim_timer_cb(lv_timer_t*)
{
    bool need_invalidate = false;

    if (s_face_mode == MODE_SELECTOR) {
        int64_t now_us = esp_timer_get_time();
        static int64_t s_last_sel_us = 0;
        float dt = (s_last_sel_us > 0)
                   ? clampf_((float)(now_us - s_last_sel_us) / 1000000.0f, 0.004f, 0.050f)
                   : 0.016f;
        s_last_sel_us = now_us;

        if (s_sel_release_pending && now_us >= s_sel_release_due_us) {
            s_sel_release_pending = false;
            s_sel_release_due_us = 0;
            s_sel_down = false;
            s_sel_press_start_us = 0;
            s_sel_x0 = -1;
            s_sel_y0 = -1;
            s_sel_last_x = -1;
            s_sel_last_y = -1;
            s_sel_last_sample_us = 0;

            /* Velocity-based momentum projection: hızlı flick → birkaç kart
             * uzağa kayar (Apple Watch carousel'i gibi). Lead factor 0.13 →
             * 0.35 (3× daha uzak proje), clamp ±0.55 → ±2.5 (en fazla 2-3
             * kart uzaklığa kayma). Yavaş drag yine tek kart snap'i.
             * Snap duration mesafe ile orantılı (uzun mesafe = uzun glide). */
            float lead = clampf_(s_sel_vel * 0.35f, -2.5f, 2.5f);
            int snap_idx = nearest_face_idx_(wrap_face_pos_(s_sel_pos + lead));
            s_sel_snap_target = (float)snap_idx;
            s_sel_snap_start = s_sel_pos;
            s_sel_snap_t = 0.0f;
            s_sel_snap_active = true;
        }

        bool interaction_active = s_sel_down || s_sel_release_pending;

        {
            float target = s_sel_delete_mode ? 1.0f : 0.0f;
            if (s_sel_down && !s_sel_delete_mode) {
                target = s_sel_delete_reveal;
            }
            float speed = s_sel_delete_mode ? 26.0f : 20.0f;
            s_sel_delete_reveal += (target - s_sel_delete_reveal) * clampf_(dt * speed, 0.0f, 1.0f);
            if (s_sel_delete_reveal < 0.001f) s_sel_delete_reveal = 0.0f;
            if (s_sel_delete_reveal > 0.999f) s_sel_delete_reveal = 1.0f;
        }

        if (interaction_active) {
            float d = shortest_delta_(s_sel_pos, s_sel_visual_pos);
            s_sel_visual_pos = wrap_face_pos_(s_sel_visual_pos + d);
        } else {
            if (!s_sel_snap_active) {
                s_sel_snap_target = (float)nearest_face_idx_(s_sel_pos);
                s_sel_snap_start = s_sel_pos;
                s_sel_snap_t = 0.0f;
                s_sel_snap_active = true;
            }

            float d_total = shortest_delta_(s_sel_snap_target, s_sel_snap_start);
            /* Adaptive duration: tek-card snap 280 ms, çok-card flick 600 ms'e
             * kadar uzar → uzun mesafe = uzun glide = "raylı kayma" hissi.
             * |d_total|=1.0 (tek kart) → 280 ms; |d_total|=3 (üç kart) → 460 ms */
            float adaptive_dur = SEL_SNAP_DURATION_S +
                                 fabsf(d_total) * 0.09f;   /* +90 ms per card */
            if (adaptive_dur > 0.6f) adaptive_dur = 0.6f;
            s_sel_snap_t += dt / adaptive_dur;
            if (s_sel_snap_t > 1.0f) s_sel_snap_t = 1.0f;
            float u = s_sel_snap_t;
            /* Apple Watch tarzı ease-out-quint: 1 - (1-u)^5
             * Hızlı başlangıç + uzun yumuşak glide = "raylı" fizik hissi. */
            float u_inv = 1.0f - u;
            float ease = 1.0f - u_inv * u_inv * u_inv * u_inv * u_inv;
            s_sel_pos = wrap_face_pos_(s_sel_snap_start + d_total * ease);

            float d_vis = shortest_delta_(s_sel_pos, s_sel_visual_pos);
            s_sel_visual_pos = s_sel_pos;
            s_sel_vel *= 0.10f;

            if (s_sel_snap_t >= 1.0f && fabsf(d_vis) < 0.0018f) {
                s_sel_pos = wrap_face_pos_(s_sel_snap_target);
                s_sel_visual_pos = s_sel_pos;
                s_sel_vel = 0.0f;
                s_sel_snap_active = false;
                s_sel_snap_t = 1.0f;
            }
        }

        selector_apply_pose();
        need_invalidate = true;
        if (need_invalidate && s_face) { freeze_frame(); lv_obj_invalidate(s_face); }
        return;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm ti;
    localtime_r(&tv.tv_sec, &ti);

    if (s_face_index == FACE_DIGITAL) {
        update_digital_labels(&ti);
    }
    /* Activity face: refresh labels every tick so the seconds counter
     * (%02d:%02d:%02d) and date stay live. LVGL's lv_label_set_text
     * diffs the string internally, so calling this at 30ms cadence is
     * cheap — only the changed-second repaint actually hits the GPU. */
    if (s_face_index == FACE_ACTIVITY) {
        update_activity_labels(&ti);
    }
    if (s_pending_face_index >= 0) {
        set_face_index((face_index_t)s_pending_face_index);
        s_pending_face_index = -1;
        need_invalidate = true;
    }
    if (s_face_index == FACE_ANALOG) {
        need_invalidate = true;
    }
    if (need_invalidate && s_face) { freeze_frame(); lv_obj_invalidate(s_face); }
}

// ─── App lifecycle ────────────────────────────────────────────────────────────
static void on_start(lv_obj_t* screen)
{
    ESP_LOGI(TAG, "Analog WatchFace starting");
    /* Session 13: BOOT'ta gecikmeli galeri senkronu.
     * refresh_custom_faces_from_gallery() ilk çağrıda depolama dosya sistemini
     * mount edip yüklü kadran dosyalarını okuyor — ölçüm: 467 ms, ve bu süre
     * UI task'ında geçtiği için ilk kare o kadar gecikiyordu (WatchFace
     * on_start 552 ms). Boot'ta bekleyen bir "galeriden seçim" olamayacağı
     * için bu iş ilk kare çizildikten sonra tek atımlık LVGL timer'ına
     * ertelenir. Sonraki açılışlarda (store yüklü) çağrı zaten ~0 ms. */
    if (s_gal_first_sync_done) {
        int64_t t_gal = esp_timer_get_time();
        refresh_custom_faces_from_gallery();
        int32_t gal_ms = (int32_t)((esp_timer_get_time() - t_gal) / 1000);
        if (gal_ms > 20) ESP_LOGW(TAG, "on_start: gallery sync %d ms", (int)gal_ms);
    } else if (!s_gal_sync_timer) {
        s_gal_sync_timer = lv_timer_create(gal_first_sync_cb, 250, NULL);
        lv_timer_set_repeat_count(s_gal_sync_timer, 1);
    }
    s_face_mode = MODE_NORMAL;
    s_draw_scale = 1.0f;
    s_draw_y_off = 0.0f;
    s_draw_x_off = 0.0f;
    s_sel_pos = (float)s_face_index;
    s_sel_visual_pos = s_sel_pos;
    s_sel_vel = 0.0f;
    s_sel_last_sample_us = 0;
    s_sel_release_pending = false;
    s_sel_release_due_us = 0;
    s_sel_snap_active = false;
    s_sel_snap_target = (float)s_face_index;
    s_sel_snap_start = s_sel_pos;
    s_sel_snap_t = 1.0f;
    ui_service_lock_swipe(false);

    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    s_face = lv_obj_create(screen);
    lv_obj_remove_style_all(s_face);
    lv_obj_set_size(s_face, SCR_W, SCR_H);
    lv_obj_set_pos(s_face, 0, 0);
    lv_obj_set_style_bg_opa(s_face, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_face, 0, 0);
    lv_obj_set_style_pad_all(s_face, 0, 0);
    lv_obj_clear_flag(s_face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_face, LV_OBJ_FLAG_CLICKABLE);
    // Disable pressed-state darkening (LVGL theme applies color filter on LV_STATE_PRESSED)
    lv_obj_set_style_color_filter_opa(s_face, 0, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_face, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_face, face_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    // Digital face layer (hidden by default)
    s_digital_layer = lv_obj_create(s_face);
    lv_obj_remove_style_all(s_digital_layer);
    lv_obj_set_size(s_digital_layer, SCR_W, SCR_H);
    lv_obj_set_pos(s_digital_layer, 0, 0);
    lv_obj_set_style_bg_opa(s_digital_layer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_digital_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_digital_layer, LV_OBJ_FLAG_CLICKABLE);

    int32_t split_y = (int32_t)roundf((float)SCR_H * 0.52f);
    s_digital_top = lv_obj_create(s_digital_layer);
    lv_obj_remove_style_all(s_digital_top);
    lv_obj_set_pos(s_digital_top, 0, 0);
    lv_obj_set_size(s_digital_top, SCR_W, split_y);
    lv_obj_set_style_bg_opa(s_digital_top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_digital_top, 0, 0);
    lv_obj_set_style_clip_corner(s_digital_top, true, 0);
    lv_obj_clear_flag(s_digital_top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_digital_top, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_digital_bottom = lv_obj_create(s_digital_layer);
    lv_obj_remove_style_all(s_digital_bottom);
    lv_obj_set_pos(s_digital_bottom, 0, split_y);
    lv_obj_set_size(s_digital_bottom, SCR_W, SCR_H - split_y);
    lv_obj_set_style_bg_opa(s_digital_bottom, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_digital_bottom, 0, 0);
    lv_obj_set_style_clip_corner(s_digital_bottom, true, 0);
    lv_obj_clear_flag(s_digital_bottom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_digital_bottom, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_digital_hour_tens_top = lv_label_create(s_digital_top);
    lv_obj_clear_flag(s_digital_hour_tens_top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_digital_hour_tens_top, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_digital_hour_tens_top, DGT_FONT_MAIN, 0);
    lv_obj_set_style_text_align(s_digital_hour_tens_top, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_pad_all(s_digital_hour_tens_top, 0, 0);
    lv_label_set_text(s_digital_hour_tens_top, "0");

    s_digital_hour_ones_top = lv_label_create(s_digital_top);
    lv_obj_clear_flag(s_digital_hour_ones_top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_digital_hour_ones_top, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_digital_hour_ones_top, DGT_FONT_MAIN, 0);
    lv_obj_set_style_text_align(s_digital_hour_ones_top, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_pad_all(s_digital_hour_ones_top, 0, 0);
    lv_label_set_text(s_digital_hour_ones_top, "0");

    s_digital_min_tens_top = lv_label_create(s_digital_top);
    lv_obj_clear_flag(s_digital_min_tens_top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_digital_min_tens_top, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_digital_min_tens_top, DGT_FONT_SMALL, 0);
    lv_obj_set_style_text_align(s_digital_min_tens_top, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_digital_min_tens_top, "0");

    s_digital_min_ones_top = lv_label_create(s_digital_top);
    lv_obj_clear_flag(s_digital_min_ones_top, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_digital_min_ones_top, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_digital_min_ones_top, DGT_FONT_MAIN, 0);
    lv_obj_set_style_text_align(s_digital_min_ones_top, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_digital_min_ones_top, "0");

    // s_digital_hour_tens_bot = lv_label_create(s_digital_bottom);
    // lv_obj_clear_flag(s_digital_hour_tens_bot, LV_OBJ_FLAG_CLICKABLE);
    // lv_obj_set_style_text_color(s_digital_hour_tens_bot, lv_color_black(), 0);
    // lv_obj_set_style_text_font(s_digital_hour_tens_bot, &lv_font_rubik_black_280, 0);
    // lv_obj_set_style_text_align(s_digital_hour_tens_bot, LV_TEXT_ALIGN_CENTER, 0);
    // lv_label_set_text(s_digital_hour_tens_bot, "0");

    s_digital_hour_ones_bot = lv_label_create(s_digital_bottom);
    lv_obj_clear_flag(s_digital_hour_ones_bot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_digital_hour_ones_bot, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_digital_hour_ones_bot, DGT_FONT_MID, 0);
    lv_obj_set_style_text_align(s_digital_hour_ones_bot, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_digital_hour_ones_bot, "0");

    s_digital_min_tens_bot = lv_label_create(s_digital_bottom);
    lv_obj_clear_flag(s_digital_min_tens_bot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_digital_min_tens_bot, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_digital_min_tens_bot, DGT_FONT_MAIN, 0);
    lv_obj_set_style_text_align(s_digital_min_tens_bot, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_pad_all(s_digital_min_tens_bot, 0, 0);
    lv_label_set_text(s_digital_min_tens_bot, "0");

    s_digital_min_ones_bot = lv_label_create(s_digital_bottom);
    lv_obj_clear_flag(s_digital_min_ones_bot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_digital_min_ones_bot, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_digital_min_ones_bot, DGT_FONT_MAIN, 0);
    lv_obj_set_style_text_align(s_digital_min_ones_bot, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_pad_all(s_digital_min_ones_bot, 0, 0);
    lv_label_set_text(s_digital_min_ones_bot, "0");

    s_activity_layer = lv_obj_create(s_face);
    lv_obj_remove_style_all(s_activity_layer);
    lv_obj_set_size(s_activity_layer, SCR_W, SCR_H);

    s_activity_time = lv_label_create(s_activity_layer);
    s_activity_goal = lv_label_create(s_activity_layer);
    s_activity_cal = lv_label_create(s_activity_layer);
    s_activity_hr = lv_label_create(s_activity_layer);
    s_activity_date = lv_label_create(s_activity_layer);

    /* v1.4.55 (2026-05-19) — audit orta öncelik #1b: sabit style'ları bir
     * kez burada set et. Önceden update_activity_labels her çağrıda 3 text
     * color + 3 font cascade yapıyordu → her seferinde LVGL invalidate.
     * Renkler hardcoded sabit, font'lar sadece face_mode'a bağlı (mode
     * değişimi seyrek). update_activity_labels artık sadece text + pos
     * güncelliyor; font'lar mode change-gated. */
    lv_obj_set_style_text_color(s_activity_goal, lv_color_make(255, 45, 112), 0);
    lv_obj_set_style_text_color(s_activity_cal,  lv_color_make(183, 255, 52), 0);
    lv_obj_set_style_text_color(s_activity_hr,   lv_color_make(63, 215, 255), 0);
    s_activity_icon_cloud_bg = lv_obj_create(s_activity_layer);
    lv_obj_remove_style_all(s_activity_icon_cloud_bg);
    lv_obj_set_size(s_activity_icon_cloud_bg, 56, 56); // Daire boyutu
    lv_obj_set_style_bg_color(s_activity_icon_cloud_bg, lv_color_make(28, 28, 30), 0); // Koyu Gri
    lv_obj_set_style_bg_opa(s_activity_icon_cloud_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_activity_icon_cloud_bg, LV_RADIUS_CIRCLE, 0);

    // Dinamik hava durumu ikonu: bitmap yerine, weather_icon modülü LVGL
    // primitifleriyle çizim yapar. Konteyner boyutu bg ile senkron tutulur
    // (update_activity_layout ikonu yeniden render ederek büyütür).
    s_activity_icon_cloud = lv_obj_create(s_activity_icon_cloud_bg);
    lv_obj_remove_style_all(s_activity_icon_cloud);
    lv_obj_set_size(s_activity_icon_cloud, 56, 56);
    lv_obj_set_style_bg_opa(s_activity_icon_cloud, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_activity_icon_cloud, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_activity_icon_cloud, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_activity_icon_cloud);
    // İlk render (servis henüz veri çekmediyse UNKNOWN çizilir — mantıklı placeholder)
    weather_data_t wd = {};
    weather_condition_t cond0 = weather_service_get(&wd) ? wd.condition : WEATHER_UNKNOWN;
    weather_icon_render(s_activity_icon_cloud, cond0);

    s_activity_icon_run_bg = lv_obj_create(s_activity_layer);
    lv_obj_remove_style_all(s_activity_icon_run_bg);
    lv_obj_set_size(s_activity_icon_run_bg, 56, 56);
    lv_obj_set_style_bg_color(s_activity_icon_run_bg, lv_color_make(28, 28, 30), 0);
    lv_obj_set_style_bg_opa(s_activity_icon_run_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_activity_icon_run_bg, LV_RADIUS_CIRCLE, 0);

    s_activity_icon_run = lv_image_create(s_activity_icon_run_bg);
    lv_image_set_src(s_activity_icon_run, &running_man_icon);
    lv_image_set_scale(s_activity_icon_run, 200);
    lv_obj_center(s_activity_icon_run);

    // İlk güncellemeyi yap
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm ti;
    localtime_r(&tv.tv_sec, &ti);
    update_activity_labels(&ti);

    /* Periyodik face redraw timer — eski sürüm her 500 ms tüm ekranı
     * invalidate ediyordu (analog seconds hand için). Digital face'de
     * minute değişmediği halde labellar her 500 ms repaint olunca
     * antialiasing edge pikselleri her render'da slightly farklı = digit
     * "titreme" görüntüsü (kullanıcı raporu 2026-05-04).
     *
     * Çözüm: timer 500 ms kalır, ama invalidate koşullu:
     *   - FACE_DIGITAL: yalnızca minute değişince invalidate
     *   - Diğer face'ler (analog/activity): her tick invalidate (akıcı hand)
     * Selector mode'da face_draw_cb anim_timer_cb'yi kontrol ediyor; bu
     * timer sadece "normal mode" face periyodik refresh'i için.
     *
     * v1.2.2 (2026-05-18): Eski bug — bu timer `s_timer` pointer'ına
     * atanıyordu, ardından on_start sonunda anim_timer_cb (30 ms) ile aynı
     * pointer ezildi → bu timer orphan kalıp LVGL listesinde sonsuza dek
     * çalışmaya devam ediyordu. Düzeltme: ayrı pointer `s_minute_timer`. */
    s_minute_timer = lv_timer_create([](lv_timer_t *) {
        if (!s_face) return;
        /* Digital ve io_control face'de minute change-based gating */
        if (s_face_mode == MODE_NORMAL &&
            (s_face_index == FACE_DIGITAL || s_face_index == FACE_IO_CONTROL)) {
            static int last_min = -1;
            time_t now; time(&now);
            struct tm ti; localtime_r(&now, &ti);
            if (ti.tm_min == last_min) return;   /* same minute, skip */
            last_min = ti.tm_min;
            /* v1.2.1: io_control için label update (saat + tarih) */
            if (s_face_index == FACE_IO_CONTROL) {
                update_io_control_labels(&ti);
            }
        }
        freeze_frame();
        lv_obj_invalidate(s_face);
    }, 500, NULL);

    s_selector_border = lv_obj_create(s_face);
    lv_obj_remove_style_all(s_selector_border);
    lv_obj_set_style_bg_opa(s_selector_border, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(s_selector_border, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_selector_border, lv_color_make(0x47, 0x47, 0x47), 0);
    lv_obj_set_style_border_opa(s_selector_border, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_selector_border, 5, 0);
    lv_obj_set_style_outline_width(s_selector_border, 0, 0);
    lv_obj_clear_flag(s_selector_border, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_selector_border, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_selector_border, LV_OBJ_FLAG_HIDDEN);

    s_selector_title = lv_label_create(s_face);
    lv_obj_set_style_text_color(s_selector_title, lv_color_make(0xE8, 0xEB, 0xEE), 0);
    lv_obj_set_style_text_font(s_selector_title, SEL_UI_FONT, 0);
    lv_obj_set_style_text_letter_space(s_selector_title, 1, 0);
    lv_obj_clear_flag(s_selector_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_selector_title, LV_OBJ_FLAG_HIDDEN);

    s_selector_edit_btn = lv_obj_create(s_face);
    lv_obj_remove_style_all(s_selector_edit_btn);
    lv_obj_set_style_bg_color(s_selector_edit_btn, lv_color_make(0x16, 0x18, 0x1C), 0);
    lv_obj_set_style_bg_opa(s_selector_edit_btn, (lv_opa_t)210, 0);
    lv_obj_set_style_border_color(s_selector_edit_btn, lv_color_make(0x7E, 0x86, 0x90), 0);
    lv_obj_set_style_border_width(s_selector_edit_btn, 1, 0);
    lv_obj_set_style_radius(s_selector_edit_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_selector_edit_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_selector_edit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_selector_edit_btn, LV_OBJ_FLAG_HIDDEN);

    s_selector_edit_label = lv_label_create(s_selector_edit_btn);
    lv_label_set_text(s_selector_edit_label, "Edit");
    lv_obj_set_style_text_color(s_selector_edit_label, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(s_selector_edit_label, 1, 0);
    lv_obj_set_style_text_font(s_selector_edit_label, SEL_UI_FONT, 0);
    lv_obj_clear_flag(s_selector_edit_label, LV_OBJ_FLAG_CLICKABLE);

    s_selector_side_left = lv_obj_create(s_face);
    lv_obj_remove_style_all(s_selector_side_left);
    lv_obj_set_style_bg_color(s_selector_side_left, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_selector_side_left, LV_OPA_TRANSP, 0);  // thumbnails drawn in face_draw_cb
    lv_obj_set_style_border_color(s_selector_side_left, lv_color_make(0x7A, 0x80, 0x88), 0);
    lv_obj_set_style_border_opa(s_selector_side_left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_selector_side_left, 0, 0);
    lv_obj_set_style_clip_corner(s_selector_side_left, true, 0);
    lv_obj_clear_flag(s_selector_side_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_selector_side_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_selector_side_left, LV_OBJ_FLAG_HIDDEN);

    s_selector_side_right = lv_obj_create(s_face);
    lv_obj_remove_style_all(s_selector_side_right);
    lv_obj_set_style_bg_color(s_selector_side_right, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_selector_side_right, LV_OPA_TRANSP, 0);  // thumbnails drawn in face_draw_cb
    lv_obj_set_style_border_color(s_selector_side_right, lv_color_make(0x7A, 0x80, 0x88), 0);
    lv_obj_set_style_border_opa(s_selector_side_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_selector_side_right, 0, 0);
    lv_obj_set_style_clip_corner(s_selector_side_right, true, 0);
    lv_obj_clear_flag(s_selector_side_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_selector_side_right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_selector_side_right, LV_OBJ_FLAG_HIDDEN);

    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm ti;
        localtime_r(&tv.tv_sec, &ti);
    update_activity_labels(&ti);

    update_selector_ui();
    }

    if (s_face_index != FACE_ACTIVITY) {
        lv_obj_add_flag(s_activity_layer, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_face_index != FACE_DIGITAL) {
        lv_obj_add_flag(s_digital_layer, LV_OBJ_FLAG_HIDDEN);
    }

    init_logo_metrics();

    enable_event_bubble_recursive(s_face);

    lv_obj_add_event_cb(s_face, face_event_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_face, face_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_face, face_event_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_face, face_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_face, face_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_face, face_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_face, face_event_cb, LV_EVENT_PRESS_LOST, NULL);

    /* v1.4.55 (audit orta #5a): cc_swipe_zone create bloğu kaldırıldı.
     * Eski tasarım üst 60 px'i invisible+CLICKABLE bir widget'la kapatıyordu
     * ve callback'leri no-op'tu → bu bant kullanıcının tap'lerini yutuyordu.
     * Control Center artık sadece gesture (cc_overlay_gesture_cb) ile açılır. */
    s_cc_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_cc_overlay);
    lv_obj_set_size(s_cc_overlay, SCR_W, SCR_H);
    lv_obj_align(s_cc_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_cc_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cc_overlay, LV_OPA_50, 0);
    lv_obj_add_flag(s_cc_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cc_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cc_overlay, cc_overlay_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_cc_overlay, cc_overlay_gesture_cb, LV_EVENT_GESTURE, NULL);

    s_cc_panel = lv_obj_create(s_cc_overlay);
    lv_obj_remove_style_all(s_cc_panel);
    lv_obj_set_size(s_cc_panel, SCR_W, 220);
    lv_obj_align(s_cc_panel, LV_ALIGN_TOP_MID, 0, -220);
    lv_obj_set_style_bg_color(s_cc_panel, lv_color_make(0x14, 0x17, 0x1C), 0);
    lv_obj_set_style_bg_opa(s_cc_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_cc_panel, 22, 0);
    lv_obj_set_style_pad_all(s_cc_panel, 20, 0);
    lv_obj_add_flag(s_cc_panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* cc_grid = lv_obj_create(s_cc_panel);
    lv_obj_remove_style_all(cc_grid);
    lv_obj_set_size(cc_grid, SCR_W - 40, 180);
    lv_obj_align(cc_grid, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_flex_flow(cc_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(cc_grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cc_grid, 14, 0);
    lv_obj_set_style_pad_column(cc_grid, 14, 0);

    struct cc_item_t { const char* text; lv_color_t bg; } cc_items[4] = {
        {"WiFi",      lv_color_make(0x2B, 0x72, 0xFF)},
        {"Bluetooth", lv_color_make(0x19, 0xA3, 0xFF)},
        {"Airplane",  lv_color_make(0x6A, 0x6E, 0x78)},
        {"Flashlight",lv_color_make(0xF0, 0xB1, 0x22)},
    };

    for (int i = 0; i < 4; i++) {
        lv_obj_t* tile = lv_obj_create(cc_grid);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, 170, 78);
        lv_obj_set_style_bg_color(tile, cc_items[i].bg, 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tile, 18, 0);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(tile);
        lv_label_set_text(lbl, cc_items[i].text);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
        lv_obj_center(lbl);
    }

    app_manager_enable_gestures(screen);

    s_timer = lv_timer_create(anim_timer_cb, 30, NULL);
    // Pre-render static analog base (bg + ticks + logo) — frees per-frame render cost
    build_analog_base();
    /* v1.2.1: io_control face widget'ları */
    build_io_control_layer();
    freeze_frame(); lv_obj_invalidate(s_face);
}

static void on_resume(void)
{
    refresh_custom_faces_from_gallery();
    if (s_face_mode == MODE_SELECTOR) selector_apply_pose();
    if (s_face) { freeze_frame(); lv_obj_invalidate(s_face); }
    if (s_timer) lv_timer_resume(s_timer);
}
static void on_pause(void)  { if (s_timer) lv_timer_pause(s_timer);  }

static void on_stop(void)
{
    s_cc_open = false;
    s_cc_overlay = NULL;
    s_cc_panel = NULL;
    /* v1.4.55: s_cc_swipe_zone kaldırıldı (audit orta #5a) */
    s_cc_drag_active = false;
    s_cc_drag_suppress_until_us = 0;
    free_face_snapshots();
    for (int i = 0; i < FACE_UPLOAD_MAX_SLOTS; i++) {
        if (s_custom_thumb_raw[i]) {
            heap_caps_free(s_custom_thumb_raw[i]);
            s_custom_thumb_raw[i] = NULL;
        }
        s_custom_hidden_slot[i] = false;
    }
    if (s_gal_sync_timer) { lv_timer_delete(s_gal_sync_timer); s_gal_sync_timer = NULL; }
    if (s_timer)        { lv_timer_delete(s_timer);        s_timer        = NULL; }
    if (s_minute_timer) { lv_timer_delete(s_minute_timer); s_minute_timer = NULL; }   /* v1.2.2 */
    s_face = NULL;
    s_digital_layer = NULL;
    s_logo_metrics_ready = false;
    s_digital_top = NULL;
    s_digital_bottom = NULL;
    s_digital_hour_tens_top = NULL;
    s_digital_hour_ones_top = NULL;
    s_digital_min_tens_top = NULL;
    s_digital_min_ones_top = NULL;
    s_digital_hour_tens_bot = NULL;
    s_digital_hour_ones_bot = NULL;
    s_digital_min_tens_bot = NULL;
    s_digital_min_ones_bot = NULL;
    /* v1.2.1: io_control face cleanup (screen ile silinir, sadece pointer NULL) */
    s_io_layer = NULL;
    s_io_time_label = NULL;
    s_io_date_label = NULL;
    s_io_toggle_outer[0] = NULL;  s_io_toggle_outer[1] = NULL;
    s_io_toggle_knob[0]  = NULL;  s_io_toggle_knob[1]  = NULL;
    s_io_toggle_card[0]  = NULL;  s_io_toggle_card[1]  = NULL;
    s_io_toggle_title[0] = NULL;  s_io_toggle_title[1] = NULL;
    /* s_io_state RTC_DATA_ATTR — sleep cycle'ında korunur, on_stop'ta dokunma */
    s_activity_layer = NULL;
    s_activity_time = NULL;
    s_activity_goal = NULL;
    s_activity_cal = NULL;
    s_activity_hr = NULL;
    s_activity_date = NULL;
    s_activity_gauge_val = NULL;
    s_activity_gauge_min = NULL;
    s_activity_gauge_max = NULL;
    s_activity_msg_badge = NULL;
    s_activity_msg_label = NULL;
    s_activity_notif_dot = NULL;
    s_activity_touch_layer = NULL;
    s_selector_border = NULL;
    s_selector_title = NULL;
    s_selector_edit_btn = NULL;
    s_selector_edit_label = NULL;
    s_selector_side_left = NULL;
    s_selector_side_right = NULL;
    s_sel_swipe_just_changed = false;
    s_sel_dragged = false;
    s_sel_swipe_block_until_us = 0;
    s_sel_press_start_us = 0;
    s_sel_ignore_release_once = false;
    s_ignore_next_selector_click = false;
    s_hold_down = false;
    s_hold_fired = false;
    s_hold_start_us = 0;
    s_norm_swiped = false;
    s_norm_x0 = -1;
    s_norm_y0 = -1;
    s_norm_last_x = -1;
    s_norm_last_y = -1;
    s_face_mode = MODE_NORMAL;
    s_draw_scale = 1.0f;
    s_draw_y_off = 0.0f;
    s_draw_x_off = 0.0f;
    s_sel_pos = 0.0f;
    s_sel_visual_pos = 0.0f;
    s_sel_vel = 0.0f;
    s_sel_last_sample_us = 0;
    s_sel_release_pending = false;
    s_sel_release_due_us = 0;
    s_sel_snap_active = false;
    s_sel_snap_target = 0.0f;
    s_sel_snap_start = 0.0f;
    s_sel_snap_t = 1.0f;
    s_sel_delete_mode = false;
    s_sel_delete_reveal = 0.0f;
    s_sel_last_drag_us = 0;
    s_sel_click_armed = false;
    s_sel_click_armed_until_us = 0;
    s_custom_face_count = 0;
    for (int i = 0; i < FACE_UPLOAD_MAX_SLOTS; i++) s_custom_slot_map[i] = -1;
    ui_service_lock_swipe(false);
}

static bool on_event(const os_event_t* event)
{
    if (!event) return false;
    if (event->type == EVENT_WEATHER_UPDATE) {
        // Hava durumu arka plan görevi yeni veri yayınladı → ikonu yenile.
        // UI task context'indeyiz (event_bus çağrısı LVGL task'ından geliyor),
        // doğrudan render güvenli.
        if (s_activity_icon_cloud) {
            weather_icon_render(s_activity_icon_cloud,
                                (weather_condition_t)event->data);
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
    .name      = "WatchFace",
};

const app_interface_t* watchface_app_get_interface(void) { return &s_iface; }
