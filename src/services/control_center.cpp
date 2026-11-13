/**
 * @file control_center.cpp
 * @brief Apple Watch style Control Center overlay.
 *
 * Panel structure (on lv_layer_top):
 *
 *   ┌─────────────────────────────────────┐  ← above screen (hidden radius)
 *   │          [CC_PAD_TOP_EXTRA]         │
 *   ├─────────────────────────────────────┤  ← y=0, screen top edge
 *   │  ┌────────────┐  ┌────────────┐    │
 *   │  │  WiFi      │  │ Bluetooth  │    │  row 0
 *   │  └────────────┘  └────────────┘    │
 *   │  ┌────────────┐  ┌────────────┐    │
 *   │  │  Airplane  │  │ Flashlight │    │  row 1
 *   │  └────────────┘  └────────────┘    │
 *   ╰─────────────────────────────────────╯  ← bottom, rounded corners
 *   ░░░░░░░░░░ semi-transparent backdrop ░░  ← tap here to close
 */

#include "control_center.h"
#include "wifi_service.h"
#include "power_service.h"                 /* Session 34: ortak parlaklık seviyesi */
#include "ui_service.h"                    /* swipe kilidi + g_swipe_dir */
#include "../assets/sun_icons.h"
#include "../drivers/power/axp2101_driver.h"
#include "../../include/wear_motion.h"
#include <lvgl.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <stdio.h>

/* ─── Layout ─────────────────────────────────────────────────────────────────
 *
 * Visible panel height = CC_PANEL_H = 502 px  (full screen, Apple Watch style)
 * Physical panel object height = CC_PANEL_H + CC_PAD_TOP_EXTRA = 522 px
 *   (extra 20 px at top hides the rounded corners above the screen edge)
 *
 * Panel open  y = -CC_PAD_TOP_EXTRA = -20   (top 20 px above screen)
 * Panel closed y = -(CC_PANEL_H + CC_PAD_TOP_EXTRA) = -522  (fully above screen)
 *
 * Button grid math (width):
 *   CC_GRID_X = (410 - (2×170 + 20)) / 2 = 25
 *   25 + 170 + 20 + 170 + 25 = 410  ✓
 *
 * Vertical layout inside visible area (top→bottom):
 *   CC_PAD_H (60) + 3×CC_BTN_H (255) + 2×CC_GAP_Y (30)
 *   + CC_EDIT_GAP (48) + CC_EDIT_H (54) + CC_PAD_BOTTOM (45) = 492 ≤ 502 ✓
 * ─────────────────────────────────────────────────────────────────────────── */

#ifdef BOARD_VARIANT_LCD128
/* 240×240 yuvarlak LCD (cihaz testi 2026-07-13: 410×502 ölçüler ekrana
 * sığmıyordu). 2 sütun × 3 satır korunur, tüm ölçüler ~0.55 ölçekli. */
#define CC_SCREEN_W       240
#define CC_SCREEN_H       240
#define CC_PANEL_W        240
#define CC_PANEL_H        240
#elif defined(DISPLAY_VARIANT_18INCH)
/* Session 34: 1.8" (368×448) kendi dalını aldı — daha önce 410×502
 * ölçülerine düşüyordu, panel ekrandan taşıyordu. */
#define CC_SCREEN_W       368
#define CC_SCREEN_H       448
#define CC_PANEL_W        368
#define CC_PANEL_H        448
#else
#define CC_SCREEN_W       410
#define CC_SCREEN_H       502
#define CC_PANEL_W        410
#define CC_PANEL_H        502   /* full-screen panel (Apple Watch style) */
#endif
#define CC_PAD_TOP_EXTRA   20   /* hides top radius above screen edge */
#define CC_PANEL_TOTAL_H  (CC_PANEL_H + CC_PAD_TOP_EXTRA)  /* = 522 */
#define CC_PANEL_Y_OPEN   (-(int32_t)CC_PAD_TOP_EXTRA)     /* = -20  */
#define CC_PANEL_Y_CLOSE  (-(int32_t)CC_PANEL_TOTAL_H)     /* = -522 */

/* Session 34: ızgaranın ALTINA parlaklık pill'i eklendi; dikey bütçe buna
 * göre yeniden dağıtıldı (aksi halde Edit butonu ekranın altına taşıyordu).
 *   240×240 : 34 + 3*40 + 2*4  + 8+26  + 6  + 32 + 6  = 240 ✓ (tam sığar;
 *              yuvarlak ekranda ilk satır eskisiyle aynı y'de bırakıldı)
 *   368×448 : 20 + 40 + 3*76 + 2*12 + 14+56 + 20 + 48 + 18 = 468 ≤ 468 ✓
 *   410×502 : 20 + 40 + 3*85 + 2*15 + 14+56 + 20 + 54 + 20 = 509 ≤ 522 ✓ */
#ifdef BOARD_VARIANT_LCD128
#define CC_BTN_W           92
#define CC_BTN_H           40   /* Session 34: 42 → 40, pill'e yer */
#define CC_PAD_H           34   /* ilk satır ESKİSİYLE AYNI y'de kalsın (yuvarlak
                                 * ekranda üst bant zaten dar; yukarı kaydırmak
                                 * kutucukların köşesini bezel'e sokuyordu) */
#define CC_PAD_BOTTOM       6
#define CC_GAP_X           10
#define CC_GAP_Y            4
#elif defined(DISPLAY_VARIANT_18INCH)
#define CC_BTN_W          152
#define CC_BTN_H           76
#define CC_PAD_H           40
#define CC_PAD_BOTTOM      18
#define CC_GAP_X           18
#define CC_GAP_Y           12
#else
#define CC_BTN_W          170
#define CC_BTN_H           85
#define CC_PAD_H           40   /* top padding inside visible area (ikonlar tepeden ayrı) */
#define CC_PAD_BOTTOM      20   /* bottom padding below Edit button */
#define CC_GAP_X           20   /* horizontal gap between columns */
#define CC_GAP_Y           15   /* vertical gap between rows */
#endif
#define CC_GRID_X         ((CC_PANEL_W - (2 * CC_BTN_W + CC_GAP_X)) / 2)  /* = 25 */
/* LV_RADIUS_CIRCLE: button min(w,h)/2 → pill shape. clip_corner KAPALI kalmalı. */
#define CC_BTN_R           LV_RADIUS_CIRCLE
#define CC_PANEL_R         22   /* panel corner radius */

/* Edit button (Apple Watch alt kısım) */
#ifdef BOARD_VARIANT_LCD128
#define CC_EDIT_W         100
#define CC_EDIT_H          32   /* Session 34: 34 → 32 */
#define CC_EDIT_GAP         6   /* Session 34: parlaklık pill'i için daraltıldı */
#elif defined(DISPLAY_VARIANT_18INCH)
#define CC_EDIT_W         180
#define CC_EDIT_H          48
#define CC_EDIT_GAP        20
#else
#define CC_EDIT_W         200
#define CC_EDIT_H          54
#define CC_EDIT_GAP        20   /* Session 34: 48 → 20 (pill için) */
#endif

/* Remove button (red minus in top-left of each tile in edit mode) */
#ifdef BOARD_VARIANT_LCD128
#define CC_MINUS_SIZE      20
#define CC_MINUS_OFFSET     3
#elif defined(DISPLAY_VARIANT_18INCH)
#define CC_MINUS_SIZE      27
#define CC_MINUS_OFFSET     5
#else
#define CC_MINUS_SIZE      30
#define CC_MINUS_OFFSET     5   /* inset from tile top-left */
#endif

/* Grid math check:
 *   60 + 3*85 + 2*15 + 48 + 44 + 55
 *   = 60 + 255 + 30 + 48 + 44 + 55 = 492  (fits inside 502 panel)
 */

/* v1.2.1 (2026-05-18): 130/90 → 100/100 (WEAR_ANIM_FAST simetrik).
 * Watchface CC (watchface_app.cpp) ile aynı görsel etki idi ama farklı
 * sürelerde çalışıyordu — kullanıcı CC'nin nereden açıldığına göre farklı
 * hız hissediyordu. Tek standart davranış. */
#define CC_ANIM_OPEN_MS   100   /* slide-down */
#define CC_ANIM_CLOSE_MS  100   /* slide-up */
#define CC_BACKDROP_OPA   120   /* ~47% black */

/* ─── Button definitions ──────────────────────────────────────────────────── */

typedef enum {
    CC_WIFI = 0,    /* row 0 col 0 */
    CC_BT,          /* row 0 col 1 */
    CC_AIRPLANE,    /* row 1 col 0 */
    CC_FLASH,       /* row 1 col 1 */
    CC_SOUND,       /* row 2 col 0 */
    CC_BATTERY,     /* row 2 col 1 (text %) */
    CC_COUNT
} cc_id_t;

/* ─── Session 34: parlaklık pill'i (C6 Session 25b portu) ─────────────────────
 * Kutucuk DEĞİL — ızgaranın altında, Ayarlar'daki ile aynı sürüklemeli pill.
 * Panelin kapanma hareketi yalnız YUKARI sürüklemede tetikleniyor
 * (panel_drag_close_cb: dy <= -8), pill yatay çalıştığı için çakışma yok;
 * yine de pill'e close-drag handler'ı BAĞLANMAZ ve sürüklerken global yatay
 * swipe nav'ı kilitlenir (Ayarlar'daki brightness bar'da da aynı koruma var). */
#define CC_BR_MIN   10     /* 0 = ekran kapalı görünür; kasten yasak */
#define CC_BR_MAX  255
#ifdef BOARD_VARIANT_LCD128
#define CC_BR_H      26
#define CC_BR_GAP     8   /* ızgara ile pill arası */
#else
#define CC_BR_H      56
#define CC_BR_GAP    14
#endif
#define CC_BR_W     (2 * CC_BTN_W + CC_GAP_X)   /* ızgarayla aynı genişlik */

static const char* const CC_SYMBOLS[CC_COUNT] = {
    LV_SYMBOL_WIFI,
    LV_SYMBOL_BLUETOOTH,
    LV_SYMBOL_GPS,        /* airplane mode — closest available symbol */
    LV_SYMBOL_EYE_OPEN,   /* flashlight */
    LV_SYMBOL_VOLUME_MAX, /* sound (toggles to MUTE when off) */
    "",                   /* battery: text percentage, set dynamically */
};

/* ─── Widget state ────────────────────────────────────────────────────────── */

static lv_obj_t* s_backdrop         = NULL;
static lv_obj_t* s_panel            = NULL;
static lv_obj_t* s_btns  [CC_COUNT] = {};
static lv_obj_t* s_icons [CC_COUNT] = {};
static lv_obj_t* s_minus [CC_COUNT] = {};   /* red "−" button on each tile (edit mode) */
static bool      s_active[CC_COUNT] = {};
static bool      s_hidden[CC_COUNT] = {};   /* tile removed via edit mode */
static lv_obj_t* s_edit_btn         = NULL;
/* Session 34: parlaklık pill'i (Ayarlar'daki ile aynı kalıp) */
static lv_obj_t*   s_br_bar         = NULL;   /* dış track — hit-test burada */
static lv_obj_t*   s_br_fill        = NULL;   /* iç beyaz dolgu */
static int32_t     s_br_bar_w       = 0;      /* track iç genişliği (px) */
static bool        s_br_dragging    = false;
static lv_timer_t* s_br_unlock_timer = NULL;  /* gecikmeli swipe kilidi bırakma */
static lv_obj_t* s_edit_lbl         = NULL;
static bool      s_edit_mode        = false;
static bool      s_open             = false;
static bool      s_close_drag_active = false;
static int16_t   s_close_drag_y0 = -1;
static bool      s_close_drag_moved = false;
static int64_t   s_ignore_click_until_us = 0;

static void set_buttons_clickable(bool en)
{
    for (int i = 0; i < CC_COUNT; i++) {
        if (!s_btns[i]) continue;
        if (en) lv_obj_add_flag(s_btns[i], LV_OBJ_FLAG_CLICKABLE);
        else    lv_obj_clear_flag(s_btns[i], LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ─── Style helpers ───────────────────────────────────────────────────────── */

/* ─── Session 34: parlaklık pill'i mantığı (C6 Session 25b portu) ─────────── */

/* Dolgu genişliğini seviyeye göre çiz. */
static void cc_bright_visual(uint8_t level)
{
    if (!s_br_fill || s_br_bar_w <= 0) return;
    const int32_t span = CC_BR_MAX - CC_BR_MIN;
    int32_t rel = ((int32_t)level - CC_BR_MIN) * s_br_bar_w / (span > 0 ? span : 1);
    if (rel < CC_BR_H)     rel = CC_BR_H;      /* pill ucu yuvarlak kalsın */
    if (rel > s_br_bar_w)  rel = s_br_bar_w;
    lv_obj_set_width(s_br_fill, rel);
}

/* Panel açılırken / Ayarlar'dan değişmişse pill'i güncel seviyeye çek. */
static void cc_bright_sync(void)
{
    cc_bright_visual(power_service_get_brightness());
}

/* Dokunuş x'inden seviye üret. */
static int32_t cc_bright_level_from_x(int32_t x_abs)
{
    const int32_t bar_x = lv_obj_get_x(s_br_bar) + lv_obj_get_x(s_panel);
    int32_t rel = x_abs - bar_x;
    if (rel < 0)          rel = 0;
    if (rel > s_br_bar_w) rel = s_br_bar_w;

    const int32_t span = CC_BR_MAX - CC_BR_MIN;
    int32_t val = CC_BR_MIN + (rel * span + s_br_bar_w / 2) / s_br_bar_w;
    if (val < CC_BR_MIN) val = CC_BR_MIN;
    if (val > CC_BR_MAX) val = CC_BR_MAX;
    return val;
}

static void cc_bright_apply_x(int32_t x_abs)
{
    if (!s_br_bar || s_br_bar_w <= 0) return;
    const int32_t val = cc_bright_level_from_x(x_abs);

    cc_bright_visual((uint8_t)val);   /* görsel HER tick — akıcı olsun */

    /* HW yazımı throttle'lı (50 ms): panele yazmak bloklayan bir SPI işlemi;
     * her PRESSING'de çağrılırsa sürükleme tırtıklı olur (Ayarlar'daki
     * brightness bar'da yaşanmış, aynı çözüm). */
    static int64_t last_us = 0;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_us >= 50000) {
        last_us = now_us;
        power_service_set_brightness((uint8_t)val);
    }
}

/* Sürükleme bitiminden 200 ms sonra swipe kilidini bırak (aşağıdaki yarış). */
static void cc_bright_unlock_cb(lv_timer_t* /*t*/)
{
    if (!s_br_dragging) {
        g_swipe_dir = 0;
        ui_service_lock_swipe(false);
    }
    s_br_unlock_timer = NULL;   /* repeat_count=1 → LVGL kendisi siler */
}

static void cc_bright_event_cb(lv_event_t* e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_br_dragging = true;
        /* Yatay sürükleme global "geri" jestine yem olmasın. */
        ui_service_lock_swipe(true);
        g_swipe_dir = 0;
        cc_bright_apply_x(p.x);
        return;
    }
    if (code == LV_EVENT_PRESSING && s_br_dragging) {
        cc_bright_apply_x(p.x);
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_br_dragging && s_br_bar && s_br_bar_w > 0) {
            /* Kesin commit — throttle penceresi içinde bırakılmış olabilir. */
            const int32_t val = cc_bright_level_from_x(p.x);
            cc_bright_visual((uint8_t)val);
            power_service_set_brightness((uint8_t)val);
        }
        s_br_dragging = false;
        g_swipe_dir = 0;
        /* KRİTİK: kilidi HEMEN kaldırma. touch_read_cb'nin parmak kalkışındaki
         * NİHAİ swipe tespiti bu callback'ten birkaç tick SONRA çalışıyor;
         * kilit o an kapalıysa g_swipe_dir set oluyor ve "CC açıkken yatay
         * swipe" navigasyonu tetikleniyordu → kullanıcı barı kaydırırken
         * ekran değişiyordu. 200 ms daha kilitli tut, sonra tek atımlık
         * timer ile bırak (Ayarlar'da da aynı çözüm). */
        s_ignore_click_until_us = esp_timer_get_time() + 180000;
        if (!s_br_unlock_timer) {
            s_br_unlock_timer = lv_timer_create(cc_bright_unlock_cb, 200, NULL);
            lv_timer_set_repeat_count(s_br_unlock_timer, 1);
        } else {
            lv_timer_reset(s_br_unlock_timer);   /* art arda sürüklemede uzat */
        }
        return;
    }
}

static void refresh_btn(cc_id_t id)
{
    if (!s_btns[id] || !s_icons[id]) return;

    /* Battery: no toggle.
     *   Pil takılı değil  → "USB" mavi tile
     *   Şarj olurken      → "⚡N%" yeşil tile
     *   Diğer             → "N%" karanlık slate tile
     */
    if (id == CC_BATTERY) {
        axp2101_bat_status_t bat = {};
        uint8_t pct         = 0;
        bool    charging    = false;
        bool    bat_present = true;
        if (axp2101_get_bat_status(&bat) == ESP_OK) {
            pct         = bat.percent;
            charging    = (bat.charge_state == AXP_CHARGE_CHARGING);
            bat_present = bat.bat_present;
        }

        char       txt[16];
        lv_color_t bg_col;
        lv_color_t text_col;

        if (!bat_present) {
            /* USB-only: pil yok, yüzde anlamsız */
            snprintf(txt, sizeof(txt), "USB");
            bg_col   = lv_color_make(0x14, 0x32, 0x55);   /* koyu cyan */
            text_col = lv_color_make(0x4A, 0xC0, 0xFF);   /* parlak cyan */
        } else if (charging) {
            /* "⚡N%" — Montserrat fontunda LV_SYMBOL_CHARGE glyph'i mevcut;
             * 170px tile genişliğine 40px fontla "⚡100%" rahat sığar. */
            snprintf(txt, sizeof(txt), LV_SYMBOL_CHARGE " %u%%", (unsigned)pct);
            bg_col   = lv_color_make(0x1E, 0x4A, 0x2E);   /* koyu yeşil */
            text_col = lv_color_make(0x4F, 0xE0, 0x9D);   /* parlak yeşil */
        } else {
            snprintf(txt, sizeof(txt), "%u%%", (unsigned)pct);
            bg_col   = lv_color_make(0x2C, 0x2C, 0x3C);   /* karanlık slate */
            text_col = lv_color_white();
        }

        lv_label_set_text(s_icons[id], txt);
        lv_obj_set_style_bg_color  (s_btns [id], bg_col,   0);
        lv_obj_set_style_text_color(s_icons[id], text_col, 0);
        lv_obj_invalidate(s_btns[id]);
        return;
    }

    bool       on       = s_active[id];
    lv_color_t bg_col   = on ? lv_color_make(0x00, 0x7A, 0xFF)   /* iOS blue */
                             : lv_color_make(0x2C, 0x2C, 0x3C);  /* dark slate */
    lv_color_t icon_col = on ? lv_color_white()
                             : lv_color_make(0xCC, 0xCC, 0xD0);

    /* Sound: swap VOLUME_MAX / MUTE symbols */
    if (id == CC_SOUND) {
        lv_label_set_text(s_icons[id], on ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    }

    lv_obj_set_style_bg_color  (s_btns[id],  bg_col,   0);
    lv_obj_set_style_text_color(s_icons[id], icon_col, 0);
    lv_obj_invalidate(s_btns[id]);
}

/* ─── Event callbacks ─────────────────────────────────────────────────────── */

static void btn_click_cb(lv_event_t* e)
{
    if (esp_timer_get_time() < s_ignore_click_until_us) return;
    /* v1.4.64 (2026-06-08): Edit mode'da tile body tap = remove (Apple iOS
     * pattern, tap-anywhere-on-icon). Önceden sadece sol-üst 30×30 kırmızı
     * (-) badge tıklanabiliyordu — kullanıcı küçük hedef raporu sonrası
     * launcher ile aynı UX davranışına geçildi. */
    if (s_edit_mode) {
        cc_id_t id = (cc_id_t)(intptr_t)lv_event_get_user_data(e);
        if (id < 0 || id >= CC_COUNT) return;
        s_hidden[id] = true;
        if (s_btns [id]) lv_obj_add_flag(s_btns [id], LV_OBJ_FLAG_HIDDEN);
        if (s_minus[id]) lv_obj_add_flag(s_minus[id], LV_OBJ_FLAG_HIDDEN);
        return;
    }

    cc_id_t id = (cc_id_t)(intptr_t)lv_event_get_user_data(e);
    if (id < 0 || id >= CC_COUNT) return;

    /* Battery is read-only — tapping just refreshes the % */
    if (id == CC_BATTERY) {
        refresh_btn(id);
        return;
    }

    s_active[id] = !s_active[id];
    refresh_btn(id);

    if (id == CC_WIFI && !s_active[CC_WIFI]) {
        wifi_service_disconnect();
    }
}

/* Red "−" button on a tile: removes that tile from the grid (edit mode only) */
static void minus_click_cb(lv_event_t* e)
{
    if (!s_edit_mode) return;
    cc_id_t id = (cc_id_t)(intptr_t)lv_event_get_user_data(e);
    if (id < 0 || id >= CC_COUNT) return;

    s_hidden[id] = true;
    if (s_btns [id]) lv_obj_add_flag(s_btns [id], LV_OBJ_FLAG_HIDDEN);
    if (s_minus[id]) lv_obj_add_flag(s_minus[id], LV_OBJ_FLAG_HIDDEN);
}

/* "Edit" / "Done" button at the bottom of the panel */
static void edit_click_cb(lv_event_t* e)
{
    (void)e;
    if (esp_timer_get_time() < s_ignore_click_until_us) return;

    s_edit_mode = !s_edit_mode;
    if (s_edit_lbl) lv_label_set_text(s_edit_lbl, s_edit_mode ? "Done" : "Edit");

    /* Show/hide minus buttons on all visible tiles */
    for (int i = 0; i < CC_COUNT; i++) {
        if (!s_minus[i]) continue;
        if (s_edit_mode && !s_hidden[i]) {
            lv_obj_clear_flag(s_minus[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_minus[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void backdrop_click_cb(lv_event_t* e)
{
    (void)e;
    control_center_close();
}

static void panel_gesture_cb(lv_event_t* /*e*/)
{
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        control_center_close();
    }
}

static void panel_drag_close_cb(lv_event_t* e)
{
    if (!s_open) return;

    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_event_get_indev(e);
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_close_drag_active = true;
        s_close_drag_y0 = p.y;
        s_close_drag_moved = false;
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_close_drag_moved) {
            s_ignore_click_until_us = esp_timer_get_time() + 180000;
        }
        s_close_drag_active = false;
        s_close_drag_y0 = -1;
        s_close_drag_moved = false;
        return;
    }

    if (code == LV_EVENT_PRESSING && s_close_drag_active && s_close_drag_y0 >= 0) {
        int16_t dy = (int16_t)(p.y - s_close_drag_y0);
        if (dy <= -8 || dy >= 8) {
            s_close_drag_moved = true;
        }
        if (dy <= -8) {
            s_ignore_click_until_us = esp_timer_get_time() + 220000;
            s_close_drag_active = false;
            s_close_drag_y0 = -1;
            s_close_drag_moved = false;
            control_center_close();
        }
    }
}

/* ─── Animation ───────────────────────────────────────────────────────────── */

static void anim_y_exec(void* obj, int32_t y)
{
    lv_obj_set_y((lv_obj_t*)obj, y);
}

static void on_open_done(lv_anim_t* a)
{
    (void)a;
    /* Slide-down finished — tiles must be tappable again */
    set_buttons_clickable(true);
}

static void on_close_done(lv_anim_t* a)
{
    (void)a;
    /* Hide both layers; s_open already set false at anim start */
    if (s_backdrop) lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (s_panel)    lv_obj_add_flag(s_panel,    LV_OBJ_FLAG_HIDDEN);
    set_buttons_clickable(true);

    /* Leave edit mode so the next open starts in normal (clean) state */
    if (s_edit_mode) {
        s_edit_mode = false;
        if (s_edit_lbl) lv_label_set_text(s_edit_lbl, "Edit");
        for (int i = 0; i < CC_COUNT; i++) {
            if (s_minus[i]) lv_obj_add_flag(s_minus[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_open = false;
}

/* ─── Public API ──────────────────────────────────────────────────────────── */

void control_center_init(void)
{
    lv_obj_t* top = lv_layer_top();

    /* ── Backdrop (full-screen dimmer + tap-to-close) ────────────────────── */
    s_backdrop = lv_obj_create(top);
    lv_obj_remove_style_all(s_backdrop);
    lv_obj_set_size(s_backdrop, CC_SCREEN_W, CC_SCREEN_H);
    lv_obj_set_pos (s_backdrop, 0, 0);
    lv_obj_set_style_bg_color   (s_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa     (s_backdrop, CC_BACKDROP_OPA,  0);
    lv_obj_set_style_border_width(s_backdrop, 0, 0);
    lv_obj_set_style_pad_all    (s_backdrop, 0, 0);
    lv_obj_clear_flag(s_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag  (s_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag  (s_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_backdrop, backdrop_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_backdrop, panel_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_backdrop, panel_drag_close_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_backdrop, panel_drag_close_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_backdrop, panel_drag_close_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_backdrop, panel_drag_close_cb, LV_EVENT_PRESS_LOST, NULL);

    /* ── Panel ───────────────────────────────────────────────────────────── */
    s_panel = lv_obj_create(top);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, CC_PANEL_W, CC_PANEL_TOTAL_H);
    lv_obj_set_pos (s_panel, 0, CC_PANEL_Y_CLOSE);      /* starts fully above */
    lv_obj_set_style_bg_color    (s_panel, lv_color_make(0x18, 0x18, 0x22), 0);
    lv_obj_set_style_bg_opa      (s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius      (s_panel, CC_PANEL_R,   0);
    /* clip_corner yok — radius sadece bg köşelerini yuvarlıyor, child overflow yok */
    lv_obj_set_style_border_width(s_panel, 0,            0);
    lv_obj_set_style_pad_all     (s_panel, 0,            0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag  (s_panel, LV_OBJ_FLAG_CLICKABLE);  /* eat stray touches */
    lv_obj_add_flag  (s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_panel, panel_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_panel, panel_drag_close_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_panel, panel_drag_close_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_panel, panel_drag_close_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_panel, panel_drag_close_cb, LV_EVENT_PRESS_LOST, NULL);

    /* ── 2×3 button grid ─────────────────────────────────────────────────── */
    for (int i = 0; i < CC_COUNT; i++) {
        int col = i % 2;
        int row = i / 2;

        /* Position relative to panel; offset by CC_PAD_TOP_EXTRA so buttons
           appear in the visible portion of the panel (below screen top edge) */
        int32_t bx = (int32_t)CC_GRID_X + col * (CC_BTN_W + CC_GAP_X);
        int32_t by = (int32_t)CC_PAD_TOP_EXTRA + CC_PAD_H
                     + row * (CC_BTN_H + CC_GAP_Y);

        /* Card */
        lv_obj_t* btn = lv_obj_create(s_panel);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, CC_BTN_W, CC_BTN_H);
        lv_obj_set_pos (btn, bx, by);
        lv_obj_set_style_bg_opa      (btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius      (btn, CC_BTN_R, 0);
        /* clip_corner kasıyla kaldırıldı — LVGL 34 KB off-screen layer açmaya çalışıyor */
        lv_obj_set_style_border_width(btn, 0,        0);
        lv_obj_set_style_pad_all     (btn, 0,        0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag  (btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, btn_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, panel_drag_close_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(btn, panel_drag_close_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(btn, panel_drag_close_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(btn, panel_drag_close_cb, LV_EVENT_PRESS_LOST, NULL);
        /* CC butonlarında press_feedback YOK: bu butonların kendi state-flip
         * color change'i (örn. WiFi gri→mavi) zaten net visual feedback veriyor.
         * Üstüne opacity anim binerse render queue'da çakışma → kullanıcı
         * "kayma" hissediyordu (raporlandı 2026-05-04). */
        s_btns[i] = btn;

        /* Big centered icon / battery %.  No name label underneath (Apple Watch style). */
        lv_obj_t* icon = lv_label_create(btn);
        if (i == CC_BATTERY) {
            /* Text percentage — use a numeric-friendly font size */
            lv_label_set_text(icon, "--%");
#ifdef BOARD_VARIANT_LCD128
            lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
#else
            lv_obj_set_style_text_font(icon, &lv_font_montserrat_40, 0);
#endif
        } else {
            lv_label_set_text(icon, CC_SYMBOLS[i]);
#ifdef BOARD_VARIANT_LCD128
            lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
#else
            lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
#endif
        }
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        s_icons[i] = icon;

        /* Red "−" remove button — top-left of tile, hidden until edit mode */
        lv_obj_t* minus = lv_obj_create(btn);
        lv_obj_remove_style_all(minus);
        lv_obj_set_size(minus, CC_MINUS_SIZE, CC_MINUS_SIZE);
        lv_obj_set_pos (minus, CC_MINUS_OFFSET, CC_MINUS_OFFSET);
        lv_obj_set_style_bg_color    (minus, lv_color_make(0xFF, 0x3B, 0x30), 0); /* iOS red */
        lv_obj_set_style_bg_opa      (minus, LV_OPA_COVER,       0);
        lv_obj_set_style_radius      (minus, LV_RADIUS_CIRCLE,   0);
        lv_obj_set_style_border_width(minus, 0,                  0);
        lv_obj_set_style_pad_all     (minus, 0,                  0);
        lv_obj_clear_flag(minus, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag  (minus, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag  (minus, LV_OBJ_FLAG_HIDDEN);
        /* v1.4.64: 30×30 görsel boyut zor tıklanıyordu. ext_click_area 18 →
         * effective 66×66. Parent btn_click_cb edit mode'da remove logic'i
         * de yapıyor (defense in depth). */
        lv_obj_set_ext_click_area(minus, 18);
        lv_obj_add_event_cb(minus, minus_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t* minus_lbl = lv_label_create(minus);
        lv_label_set_text(minus_lbl, LV_SYMBOL_MINUS);
        lv_obj_set_style_text_color(minus_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_style_text_font (minus_lbl, &lv_font_montserrat_14, 0);
#else
        lv_obj_set_style_text_font (minus_lbl, &lv_font_montserrat_20, 0);
#endif
        lv_obj_align(minus_lbl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(minus_lbl, LV_OBJ_FLAG_CLICKABLE);
        s_minus[i] = minus;

        /* Initial toggle state:
         *   WiFi: from service; Sound: on by default; others: off; Battery: n/a */
        if (i == CC_WIFI)       s_active[i] = wifi_service_is_connected();
        else if (i == CC_SOUND) s_active[i] = true;
        else                    s_active[i] = false;
        refresh_btn((cc_id_t)i);
    }

    /* ── Session 34: parlaklık pill'i (ızgaranın altı) ──────────────────── */
    {
        const int32_t by = (int32_t)CC_PAD_TOP_EXTRA + CC_PAD_H
                         + 3 * CC_BTN_H + 2 * CC_GAP_Y + CC_BR_GAP;

        s_br_bar = lv_obj_create(s_panel);
        lv_obj_remove_style_all(s_br_bar);
        lv_obj_set_size(s_br_bar, CC_BR_W, CC_BR_H);
        lv_obj_set_pos (s_br_bar, (int32_t)CC_GRID_X, by);
        lv_obj_set_style_bg_color    (s_br_bar, lv_color_make(0x2C, 0x2C, 0x3C), 0);
        lv_obj_set_style_bg_opa      (s_br_bar, LV_OPA_COVER,     0);
        lv_obj_set_style_radius      (s_br_bar, CC_BR_H / 2,      0);
        lv_obj_set_style_border_width(s_br_bar, 0,                0);
        lv_obj_set_style_pad_all     (s_br_bar, 0,                0);
        lv_obj_clear_flag(s_br_bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag  (s_br_bar, LV_OBJ_FLAG_CLICKABLE);
        s_br_bar_w = CC_BR_W;

        /* İç dolgu: soldan büyüyen beyaz pill. */
        s_br_fill = lv_obj_create(s_br_bar);
        lv_obj_remove_style_all(s_br_fill);
        lv_obj_set_size(s_br_fill, CC_BR_H, CC_BR_H);
        lv_obj_set_pos (s_br_fill, 0, 0);
        lv_obj_set_style_bg_color    (s_br_fill, lv_color_white(), 0);
        lv_obj_set_style_bg_opa      (s_br_fill, LV_OPA_COVER,     0);
        lv_obj_set_style_radius      (s_br_fill, CC_BR_H / 2,      0);
        lv_obj_set_style_border_width(s_br_fill, 0,                0);
        lv_obj_clear_flag(s_br_fill, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                                     LV_OBJ_FLAG_SCROLLABLE));

        /* Güneş ikonu SOL uçta: dolgunun en dar hali CC_BR_H olduğu için ikon
         * HER seviyede beyaz dolgunun üstünde kalır → koyu renge boyanır.
         * (Sağ uca konulursa yüksek seviyelerde beyaz üstünde beyaz kaybolur.) */
        lv_obj_t* sun = lv_image_create(s_br_bar);
#ifdef BOARD_VARIANT_LCD128
        lv_image_set_src(sun, &sun_icon_16);
        lv_obj_align(sun, LV_ALIGN_LEFT_MID, 8, 0);
#else
        lv_image_set_src(sun, &sun_icon_24);
        lv_obj_align(sun, LV_ALIGN_LEFT_MID, 16, 0);
#endif
        lv_obj_set_style_image_recolor    (sun, lv_color_make(0x2C, 0x2C, 0x3C), 0);
        lv_obj_set_style_image_recolor_opa(sun, LV_OPA_COVER, 0);
        lv_obj_clear_flag(sun, LV_OBJ_FLAG_CLICKABLE);

        /* NOT: panel_drag_close_cb BAĞLANMAZ — pill'de sürükleme paneli
         * kapatma hareketi sayılmamalı. */
        lv_obj_add_event_cb(s_br_bar, cc_bright_event_cb, LV_EVENT_PRESSED,    NULL);
        lv_obj_add_event_cb(s_br_bar, cc_bright_event_cb, LV_EVENT_PRESSING,   NULL);
        lv_obj_add_event_cb(s_br_bar, cc_bright_event_cb, LV_EVENT_RELEASED,   NULL);
        lv_obj_add_event_cb(s_br_bar, cc_bright_event_cb, LV_EVENT_PRESS_LOST, NULL);

        cc_bright_sync();
    }

    /* ── "Edit" / "Done" button (Apple Watch style, bottom of panel) ─────── */
    {
        int32_t ex = (int32_t)(CC_PANEL_W - CC_EDIT_W) / 2;
        /* Session 34: pill ızgara ile Edit arasına girdi. */
        int32_t ey = (int32_t)CC_PAD_TOP_EXTRA + CC_PAD_H
                   + 3 * CC_BTN_H + 2 * CC_GAP_Y
                   + CC_BR_GAP + CC_BR_H + CC_EDIT_GAP;

        s_edit_btn = lv_obj_create(s_panel);
        lv_obj_remove_style_all(s_edit_btn);
        lv_obj_set_size(s_edit_btn, CC_EDIT_W, CC_EDIT_H);
        lv_obj_set_pos (s_edit_btn, ex, ey);
        lv_obj_set_style_bg_color    (s_edit_btn, lv_color_make(0x2C, 0x2C, 0x3C), 0);
        lv_obj_set_style_bg_opa      (s_edit_btn, LV_OPA_COVER,     0);
        lv_obj_set_style_radius      (s_edit_btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_edit_btn, 0,                0);
        lv_obj_set_style_pad_all     (s_edit_btn, 0,                0);
        lv_obj_clear_flag(s_edit_btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag  (s_edit_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_edit_btn, edit_click_cb,        LV_EVENT_CLICKED,    NULL);
        lv_obj_add_event_cb(s_edit_btn, panel_drag_close_cb,  LV_EVENT_PRESSED,    NULL);
        lv_obj_add_event_cb(s_edit_btn, panel_drag_close_cb,  LV_EVENT_PRESSING,   NULL);
        lv_obj_add_event_cb(s_edit_btn, panel_drag_close_cb,  LV_EVENT_RELEASED,   NULL);
        lv_obj_add_event_cb(s_edit_btn, panel_drag_close_cb,  LV_EVENT_PRESS_LOST, NULL);

        s_edit_lbl = lv_label_create(s_edit_btn);
        lv_label_set_text(s_edit_lbl, "Edit");
        lv_obj_set_style_text_color(s_edit_lbl, lv_color_white(),        0);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_style_text_font (s_edit_lbl, &lv_font_montserrat_16,  0);
#else
        lv_obj_set_style_text_font (s_edit_lbl, &lv_font_montserrat_28,  0);
#endif
        lv_obj_align(s_edit_lbl, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(s_edit_lbl, LV_OBJ_FLAG_CLICKABLE);
    }
}

void control_center_open(void)
{
    if (s_open || !s_panel || !s_backdrop) return;
    s_open = true;

    /* Session 34: parlaklık Ayarlar'dan da değişmiş olabilir. */
    cc_bright_sync();

    /* Refresh real-time state (WiFi connection + battery %) */
    s_active[CC_WIFI] = wifi_service_is_connected();
    refresh_btn(CC_WIFI);
    refresh_btn(CC_BATTERY);

    /* Make visible */
    lv_obj_clear_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_panel,    LV_OBJ_FLAG_HIDDEN);
    set_buttons_clickable(false);

    /* Slide panel down: from fully above screen to open position */
    lv_anim_delete(s_panel, anim_y_exec);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var         (&a, s_panel);
    lv_anim_set_exec_cb     (&a, anim_y_exec);
    lv_anim_set_values      (&a, lv_obj_get_y(s_panel), CC_PANEL_Y_OPEN);
    lv_anim_set_duration    (&a, CC_ANIM_OPEN_MS);
    lv_anim_set_path_cb     (&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, on_open_done);
    lv_anim_start(&a);
}

void control_center_close(void)
{
    if (!s_open || !s_panel) return;
    /* s_open stays true until on_close_done() — prevents re-entry during anim */

    lv_anim_delete(s_panel, anim_y_exec);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var         (&a, s_panel);
    lv_anim_set_exec_cb     (&a, anim_y_exec);
    lv_anim_set_values      (&a, lv_obj_get_y(s_panel), CC_PANEL_Y_CLOSE);
    lv_anim_set_duration    (&a, CC_ANIM_CLOSE_MS);
    lv_anim_set_path_cb     (&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, on_close_done);
    lv_anim_start(&a);
}

bool control_center_is_open(void)
{
    return s_open;
}
