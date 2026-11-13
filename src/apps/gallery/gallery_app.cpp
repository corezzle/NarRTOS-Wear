#include "gallery_app.h"
#include <esp_timer.h>
#include <esp_log.h>

#include "../../core/app_manager.h"
#include "../../services/face_upload_service.h"
#include "../../services/gallery_service.h"
#include "../../services/gallery_pack.h"   /* Session 35: flash'teki hazır görseller */

#include <lvgl.h>
#include <stdint.h>

static const char* TAG = "Gallery";

// ── Layout constants ──────────────────────────────────────────────────────────
// 2-column grid. Cards at 190×233 → same aspect ratio as 410×502 source
// so photos fill cards with zero crop distortion.
// Math: 2×10 (side pad) + 190 + 10 (col gap) + 190 = 410 ✓

#ifdef BOARD_VARIANT_LCD128
#define GALL_COLS        2
#define GALL_CARD_W    100
#define GALL_CARD_H    122
#define GALL_SIDE_PAD   10
#define GALL_COL_GAP    10
#define GALL_ROW_GAP    10
#define GALL_CORNER_R   10
#define GALL_HDR_H      42
#define GALL_GRID_TOP   10
#else
#define GALL_COLS        2
#define GALL_CARD_W    190    // card width
#define GALL_CARD_H    233    // card height  (190/233 ≈ 410/502)
#define GALL_SIDE_PAD   10    // left/right screen margin
#define GALL_COL_GAP    10    // gap between columns
#define GALL_ROW_GAP    12    // vertical gap between rows
#define GALL_CORNER_R   12    // card corner radius
#define GALL_HDR_H      60    // header height
#define GALL_GRID_TOP   18    // breathing room below header
#endif

// ── Thumbnail cache ───────────────────────────────────────────────────────────
// Pre-rendered at card size (190×233) in PSRAM — replaces per-frame SW scaling.
// Built once per slot, reused across gallery sessions (screens persist).
// 190×233×2 = 88 540 B per slot, 12 slots max ≈ 1 MB PSRAM.

static void*          s_thumb_raw[FACE_UPLOAD_MAX_SLOTS] = {};
static lv_image_dsc_t s_thumb_dsc[FACE_UPLOAD_MAX_SLOTS];

static void ensure_thumb(int slot, const uint16_t* src565)
{
    if (!src565 || slot < 0 || slot >= FACE_UPLOAD_MAX_SLOTS) return;
    if (s_thumb_raw[slot]) return;          // already built

    const int tw = GALL_CARD_W;            // 190
    const int th = GALL_CARD_H;            // 233
    const size_t bytes = (size_t)tw * th * 2u;

    uint16_t* dst = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst) dst = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!dst) return;

    // Nearest-neighbor downscale 410×502 → 190×233.
    // clip_corner on the card widget handles rounded-corner clipping visually.
    for (int y = 0; y < th; y++) {
        int sy = (y * FACE_UPLOAD_H) / th;
        const uint16_t* srow = src565 + (size_t)sy * FACE_UPLOAD_W;
        uint16_t*       drow = dst    + (size_t)y  * tw;
        for (int x = 0; x < tw; x++) {
            drow[x] = srow[(x * FACE_UPLOAD_W) / tw];
        }
    }

    s_thumb_raw[slot] = dst;
    uint32_t stride = lv_draw_buf_width_to_stride(tw, LV_COLOR_FORMAT_RGB565);
    lv_image_dsc_t* d = &s_thumb_dsc[slot];
    d->header.magic      = LV_IMAGE_HEADER_MAGIC;
    d->header.cf         = LV_COLOR_FORMAT_RGB565;
    d->header.flags      = 0;
    d->header.w          = (uint16_t)tw;
    d->header.h          = (uint16_t)th;
    d->header.stride     = (uint16_t)stride;
    d->header.reserved_2 = 0;
    d->data_size         = (uint32_t)bytes;
    d->data              = (const uint8_t*)dst;
    d->reserved          = NULL;
}

// ── Widget handles ────────────────────────────────────────────────────────────

static lv_obj_t* s_back_btn   = NULL;
static lv_obj_t* s_grid       = NULL;
static lv_obj_t* s_full_img   = NULL;
static lv_obj_t* s_full_overlay = NULL;
static lv_obj_t*      s_item_btn[FACE_UPLOAD_MAX_SLOTS] = {};

/* ── Session 35: flash'taki hazır görseller ('gallery' bölümü, mmap) ────────
 * Kullanıcının yüklediği kadran fotoğraflarıyla AYNI ızgarada gösterilir:
 * önce paket görselleri, sonra yüklenenler. İkisini ayırmak için kart
 * kimliği paket görsellerinde GALL_PACK_ID_BASE ile ofsetlenir. */
#define GALL_PACK_MAX      32
#define GALL_PACK_ID_BASE  100
static lv_obj_t*      s_pack_btn[GALL_PACK_MAX] = {};
static lv_image_dsc_t s_pack_dsc[GALL_PACK_MAX];

// Full-resolution descriptors: [0..MAX-1] fallback thumbnails, [MAX] fullscreen
static lv_image_dsc_t s_item_img[FACE_UPLOAD_MAX_SLOTS];   // fallback (rare)
static lv_image_dsc_t s_full_img_dsc;
static int  s_full_slot  = -1;

/* ── Session 36: ÇİFT DOKUNUŞLA BÜYÜTME (C6 Session 18 portu) ───────────────
 * Dokunmatik tek parmaklı (self-cap) → telefonlardaki iki parmak pinch
 * MÜMKÜN DEĞİL. Onun yerine:
 *   çift dokunuş         → dokunulan noktayı merkezleyerek 2 kat büyütür
 *   tekrar çift dokunuş  → normale döner
 *   büyükken sürükleme   → resim içinde gezinme
 *   tek dokunuş (büyük değilken) → tam ekrandan çıkar (eski davranış)
 * Ölçekleme yazılımla yapıldığı için oran SABİT 2x (akıcılık). */
#define GALL_ZOOM_SCALE     512      /* 256 = 1x, 512 = 2x */
/* Çift dokunuş penceresi = tek dokunuşta çıkış gecikmesi (ikisi aynı olmak
 * ZORUNDA: daha erken çıkarsak ikinci dokunuş ızgaraya düşüp başka bir resim
 * açardı). */
#define GALL_DTAP_MS        220
#define GALL_DTAP_DIST      40       /* iki dokunuş arası izin verilen kayma */

static bool        s_zoomed      = false;
static int32_t     s_zoom_ox     = 0;     /* büyükken görüntü kaydırması */
static int32_t     s_zoom_oy     = 0;
static int64_t     s_last_tap_us = 0;
static lv_point_t  s_last_tap_pt = {0, 0};
static lv_point_t  s_pan_last    = {0, 0};
static bool        s_panning     = false;
/* Tek dokunuş ANINDA çıkış yapınca ikinci dokunuşa fırsat kalmıyor; çıkış
 * çift dokunuş penceresi kadar geciktirilir, bu sürede ikinci dokunuş
 * gelirse çıkış iptal edilip büyütme yapılır. */
static lv_timer_t* s_tap_timer   = NULL;

static void tap_timer_cancel(void)
{
    if (s_tap_timer) { lv_timer_delete(s_tap_timer); s_tap_timer = NULL; }
}
static bool s_pick_mode  = false;

// ── Tap-vs-scroll ayrıştırma ─────────────────────────────────────────────────
// s_tap_valid: yeni bir dokunuşta true olur; hareket veya grid scroll
// algılanırsa false'a düşer. LV_EVENT_CLICKED ancak true iken işlenir.
//
// Hareket eşiği: LV_EVENT_PRESSING her tick'te ateşlenir → gerçek zamanlı.
// Grid scroll: LV_EVENT_SCROLL callback'i s_tap_valid'i false yapar.
#define TAP_MAX_DIST  10          // px — bu kadar kaymak = scroll niyeti
static bool       s_tap_valid = true;
static lv_point_t s_press_pt  = {0, 0};

static void slot_press_cb(lv_event_t* e);
static void slot_pressing_cb(lv_event_t* e);
static void slot_click_cb(lv_event_t* e);
static void grid_scroll_cb(lv_event_t* e);
static void full_img_click_cb(lv_event_t* e);
static void full_press_cb(lv_event_t* e);
static void full_pressing_cb(lv_event_t* e);

// ─── Fullscreen view ──────────────────────────────────────────────────────────

static void set_fullscreen_slot(int slot)
{
    /* Session 35: paket görseli — veri flash'ta (mmap), RAM'e kopyalanmaz. */
    if (slot >= GALL_PACK_ID_BASE) {
        uint16_t fw = 0, fh = 0;
        const void* fdata = gallery_pack_full(slot - GALL_PACK_ID_BASE, &fw, &fh);
        if (!fdata) return;
        uint32_t fstride = lv_draw_buf_width_to_stride(fw, LV_COLOR_FORMAT_RGB565);
        s_full_img_dsc.header.magic      = LV_IMAGE_HEADER_MAGIC;
        s_full_img_dsc.header.cf         = LV_COLOR_FORMAT_RGB565;
        s_full_img_dsc.header.flags      = 0;
        s_full_img_dsc.header.w          = fw;
        s_full_img_dsc.header.h          = fh;
        s_full_img_dsc.header.stride     = (uint16_t)fstride;
        s_full_img_dsc.header.reserved_2 = 0;
        s_full_img_dsc.data_size         = (uint32_t)fstride * fh;
        s_full_img_dsc.data              = (const uint8_t*)fdata;
        s_full_img_dsc.reserved          = NULL;

        s_full_slot = slot;
        /* Session 36: yeni resim normal boyutta açılsın. */
        s_zoomed = false; s_zoom_ox = s_zoom_oy = 0; s_panning = false;
        s_last_tap_us = 0;
        tap_timer_cancel();
        if (s_full_img) { lv_image_set_scale(s_full_img, 256); lv_obj_set_pos(s_full_img, 0, 0); }
        if (s_grid)     lv_obj_add_flag(s_grid,     LV_OBJ_FLAG_HIDDEN);
        if (s_back_btn) lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_full_overlay) {
            lv_image_set_src(s_full_img, &s_full_img_dsc);
            lv_obj_clear_flag(s_full_img,     LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_full_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    const face_upload_slot_t* u = face_upload_get_slot(slot);
    if (!u || !u->buf || !u->valid) return;

    uint32_t stride = lv_draw_buf_width_to_stride(FACE_UPLOAD_W, LV_COLOR_FORMAT_RGB565);
    s_full_img_dsc.header.magic      = LV_IMAGE_HEADER_MAGIC;
    s_full_img_dsc.header.cf         = LV_COLOR_FORMAT_RGB565;
    s_full_img_dsc.header.flags      = 0;
    s_full_img_dsc.header.w          = FACE_UPLOAD_W;
    s_full_img_dsc.header.h          = FACE_UPLOAD_H;
    s_full_img_dsc.header.stride     = (uint16_t)stride;
    s_full_img_dsc.header.reserved_2 = 0;
    s_full_img_dsc.data_size         = FACE_UPLOAD_BUF_BYTES;
    s_full_img_dsc.data              = (const uint8_t*)u->buf;
    s_full_img_dsc.reserved          = NULL;

    s_full_slot = slot;
    s_zoomed = false; s_zoom_ox = s_zoom_oy = 0; s_panning = false;   /* Session 36 */
    s_last_tap_us = 0;
    tap_timer_cancel();
    if (s_full_img) { lv_image_set_scale(s_full_img, 256); lv_obj_set_pos(s_full_img, 0, 0); }
    if (s_grid)     lv_obj_add_flag(s_grid,     LV_OBJ_FLAG_HIDDEN);
    if (s_back_btn) lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_full_overlay) {
        lv_image_set_src(s_full_img, &s_full_img_dsc);
        lv_obj_clear_flag(s_full_img,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_full_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void exit_fullscreen(void)
{
    tap_timer_cancel();
    s_full_slot = -1;
    /* Session 36: büyütme durumu bir sonraki resme taşınmasın */
    s_zoomed = false; s_zoom_ox = s_zoom_oy = 0; s_panning = false;
    if (s_full_img) { lv_image_set_scale(s_full_img, 256); lv_obj_set_pos(s_full_img, 0, 0); }
    if (s_full_img)     lv_obj_add_flag(s_full_img,     LV_OBJ_FLAG_HIDDEN);
    if (s_full_overlay) lv_obj_add_flag(s_full_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_grid)         lv_obj_clear_flag(s_grid,     LV_OBJ_FLAG_HIDDEN);
    if (s_back_btn)     lv_obj_clear_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
}

static void back_click_cb(lv_event_t* e)
{
    (void)e;
    if (s_full_slot >= 0) { exit_fullscreen(); return; }
    s_pick_mode = false;
    app_manager_back();
}

// ─── Grid refresh ─────────────────────────────────────────────────────────────

static void refresh_ui(void)
{
    // Guard: refresh_ui can be triggered via lv_async_call queued by the HTTP
    // server task (upload_done_async). If the gallery app stopped between
    // queue and dispatch, s_grid is NULL and lv_obj_create would dereference
    // it. Sync state regardless so memory accounting stays correct, then bail.
    gallery_service_sync_uploaded();
    if (!s_grid) return;

    for (int i = 0; i < FACE_UPLOAD_MAX_SLOTS; i++) {
        if (s_item_btn[i]) { lv_obj_delete(s_item_btn[i]); s_item_btn[i] = NULL; }
    }
    for (int i = 0; i < GALL_PACK_MAX; i++) {
        if (s_pack_btn[i]) { lv_obj_delete(s_pack_btn[i]); s_pack_btn[i] = NULL; }
    }

    /* ── Session 35: flash paketindeki hazır görseller (mmap; ölçekleme yok,
     * kapaklar zaten kart ölçüsünde kırpılmış) ─────────────────────────── */
    const int np = gallery_pack_count();
    for (int i = 0; i < np && i < GALL_PACK_MAX; i++) {
        uint16_t tw = 0, th = 0;
        const void* tdata = gallery_pack_thumb(i, &tw, &th);
        if (!tdata) continue;
        uint32_t tstride = lv_draw_buf_width_to_stride(tw, LV_COLOR_FORMAT_RGB565);
        lv_image_dsc_t* d = &s_pack_dsc[i];
        d->header.magic      = LV_IMAGE_HEADER_MAGIC;
        d->header.cf         = LV_COLOR_FORMAT_RGB565;
        d->header.flags      = 0;
        d->header.w          = tw;
        d->header.h          = th;
        d->header.stride     = (uint16_t)tstride;
        d->header.reserved_2 = 0;
        d->data_size         = (uint32_t)tstride * th;
        d->data              = (const uint8_t*)tdata;
        d->reserved          = NULL;

        lv_obj_t* btn = lv_obj_create(s_grid);
        lv_obj_remove_style_all(btn);
        s_pack_btn[i] = btn;
        lv_obj_set_size(btn, GALL_CARD_W, GALL_CARD_H);
        lv_obj_set_style_radius(btn, GALL_CORNER_R, 0);
        lv_obj_set_style_bg_color(btn, lv_color_make(0x18, 0x18, 0x1E), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(btn, true, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        /* Kartlarda tema PRESSED dönüşümünü kilitle (yüklenen kartlarla aynı). */
        lv_obj_set_style_transform_scale_x(btn, 256, 0);
        lv_obj_set_style_transform_scale_y(btn, 256, 0);
        lv_obj_set_style_transform_scale_x(btn, 256, LV_STATE_PRESSED);
        lv_obj_set_style_transform_scale_y(btn, 256, LV_STATE_PRESSED);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        const intptr_t id = GALL_PACK_ID_BASE + i;
        lv_obj_add_event_cb(btn, slot_press_cb,    LV_EVENT_PRESSED,  (void*)id);
        lv_obj_add_event_cb(btn, slot_pressing_cb, LV_EVENT_PRESSING, (void*)id);
        lv_obj_add_event_cb(btn, slot_click_cb,    LV_EVENT_CLICKED,  (void*)id);

        lv_obj_t* img = lv_image_create(btn);
        lv_image_set_src(img, d);
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    int n = gallery_service_uploaded_count();
    for (int i = 0; i < n; i++) {
        int slot = gallery_service_uploaded_slot_at(i);
        const face_upload_slot_t* u = face_upload_get_slot(slot);
        if (!u || !u->buf || !u->valid) continue;

        // Build or reuse pre-rendered 190×233 thumbnail (fast pixel blit, no scaling).
        ensure_thumb(slot, (const uint16_t*)u->buf);

        // ── Card — lv_obj (lv_btn değil: pressed-state flash yok = jitter yok) ──
        lv_obj_t* btn = lv_obj_create(s_grid);
        lv_obj_remove_style_all(btn);          // theme pressed-state temizlenir
        s_item_btn[i] = btn;
        lv_obj_set_size(btn, GALL_CARD_W, GALL_CARD_H);
        lv_obj_set_style_radius(btn, GALL_CORNER_R, 0);
        lv_obj_set_style_bg_color(btn, lv_color_make(0x18, 0x18, 0x1E), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(btn, true, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        /* Defansif transform pinning: theme PRESSED state'i bazı durumlarda
         * remove_style_all sonrasında bile (lazy theme apply) child widget'a
         * subtle scale/translate sızdırabilir → kart tıklanınca anlık shrink
         * + thumb shift. Tüm state'lerde scale=256, translate=0 lock. */
        lv_obj_set_style_transform_scale_x(btn, 256, 0);
        lv_obj_set_style_transform_scale_y(btn, 256, 0);
        lv_obj_set_style_translate_x(btn, 0, 0);
        lv_obj_set_style_translate_y(btn, 0, 0);
        lv_obj_set_style_transform_scale_x(btn, 256, LV_STATE_PRESSED);
        lv_obj_set_style_transform_scale_y(btn, 256, LV_STATE_PRESSED);
        lv_obj_set_style_translate_x(btn, 0, LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(btn, 0, LV_STATE_PRESSED);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);   // kart iç scroll yok
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, slot_press_cb,    LV_EVENT_PRESSED,  (void*)(intptr_t)slot);
        lv_obj_add_event_cb(btn, slot_pressing_cb, LV_EVENT_PRESSING, (void*)(intptr_t)slot);
        lv_obj_add_event_cb(btn, slot_click_cb,    LV_EVENT_CLICKED,  (void*)(intptr_t)slot);

        // ── Thumbnail image — 1:1 blit of pre-rendered buffer ─────────────────
        // No scale transform = O(44K) pixel blit per frame instead of O(206K) scaled read.
        lv_obj_t* img = lv_image_create(btn);
        if (s_thumb_raw[slot]) {
            lv_image_set_src(img, &s_thumb_dsc[slot]);   // 190×233 pre-scaled → 1:1 blit
        } else {
            // Fallback (PSRAM alloc failed): scale full image — slow but rare.
            // Uses the static s_item_img[i] descriptor (i = gallery display index).
            uint32_t st = lv_draw_buf_width_to_stride(FACE_UPLOAD_W, LV_COLOR_FORMAT_RGB565);
            s_item_img[i].header.magic      = LV_IMAGE_HEADER_MAGIC;
            s_item_img[i].header.cf         = LV_COLOR_FORMAT_RGB565;
            s_item_img[i].header.flags      = 0;
            s_item_img[i].header.w          = FACE_UPLOAD_W;
            s_item_img[i].header.h          = FACE_UPLOAD_H;
            s_item_img[i].header.stride     = (uint16_t)st;
            s_item_img[i].header.reserved_2 = 0;
            s_item_img[i].data_size         = FACE_UPLOAD_BUF_BYTES;
            s_item_img[i].data              = (const uint8_t*)u->buf;
            s_item_img[i].reserved          = NULL;
            lv_image_set_src(img, &s_item_img[i]);
            int32_t sc = (int32_t)((float)GALL_CARD_W * LV_SCALE_NONE / FACE_UPLOAD_W + 0.5f);
            lv_image_set_pivot(img, 0, 0);
            lv_image_set_scale(img, (uint32_t)sc);
        }
        lv_obj_set_pos(img, 0, 0);   // thumb already matches card exactly
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    }

    // Revalidate fullscreen if the displayed slot was deleted
    if (s_full_slot >= 0) {
        bool found = false;
        int n2 = gallery_service_uploaded_count();
        for (int i = 0; i < n2; i++) {
            if (gallery_service_uploaded_slot_at(i) == s_full_slot) { found = true; break; }
        }
        if (!found) exit_fullscreen();
        else        set_fullscreen_slot(s_full_slot);
    }
}

// ─── Upload-done callback ─────────────────────────────────────────────────────
// Called from HTTP server FreeRTOS task — must not touch LVGL directly.

static void upload_done_async(void* /*unused*/) { refresh_ui(); }
static void upload_done_cb(int slot)
{
    // Invalidate pre-rendered thumbnail for this slot so it rebuilds with new data
    if (slot >= 0 && slot < FACE_UPLOAD_MAX_SLOTS && s_thumb_raw[slot]) {
        heap_caps_free(s_thumb_raw[slot]);
        s_thumb_raw[slot] = NULL;
    }
    lv_async_call(upload_done_async, NULL);
}

// ─── WiFi AP start — deferred so it doesn't block the slide-in animation ─────

static void start_upload_service_async(void* /*unused*/)
{
    if (!face_upload_service_is_running()) {
        face_upload_service_start(upload_done_cb);
    }
}

// ─── Event callbacks ──────────────────────────────────────────────────────────

// Yeni dokunuş: tap geçerli, başlangıç noktasını kaydet.
static void slot_press_cb(lv_event_t* e)
{
    (void)e;
    s_tap_valid = true;
    lv_indev_t* indev = lv_indev_get_act();
    if (indev) lv_indev_get_point(indev, &s_press_pt);
}

// Her tick'te ateşlenir: anlık toplam sapma > eşik ise tap iptal.
static void slot_pressing_cb(lv_event_t* e)
{
    (void)e;
    if (!s_tap_valid) return;
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t cur;
    lv_indev_get_point(indev, &cur);
    int32_t dx = cur.x - s_press_pt.x; if (dx < 0) dx = -dx;
    int32_t dy = cur.y - s_press_pt.y; if (dy < 0) dy = -dy;
    if (dx > TAP_MAX_DIST || dy > TAP_MAX_DIST) s_tap_valid = false;
}

// Grid scroll algılandı: bu dokunuşta tap tetiklenemez.
static void grid_scroll_cb(lv_event_t* e)
{
    (void)e;
    s_tap_valid = false;
}

// Bırakma sonrası: ancak s_tap_valid ise resmi aç.
static void slot_click_cb(lv_event_t* e)
{
    if (!s_tap_valid) return;

    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    /* Session 35: paket görselleri bir yükleme slotu değil → kadran olarak
     * seçilemez; seçim modunda da yalnız tam ekran açılır. */
    if (s_pick_mode && slot < GALL_PACK_ID_BASE) {
        gallery_service_set_pending_pick(slot);
        s_pick_mode = false;
        app_manager_back();
        return;
    }
    set_fullscreen_slot(slot);
}

/* ── Session 36: çift dokunuşla büyütme ─────────────────────────────────── */

static void zoom_apply(void)
{
    if (!s_full_img) return;
    if (s_zoomed) {
        lv_image_set_scale(s_full_img, GALL_ZOOM_SCALE);
        /* Ölçek merkezden uygulanır; kaydırmayı konumla veriyoruz. */
        lv_obj_set_pos(s_full_img, s_zoom_ox, s_zoom_oy);
    } else {
        lv_image_set_scale(s_full_img, 256);
        lv_obj_set_pos(s_full_img, 0, 0);
    }
    lv_obj_invalidate(s_full_img);
}

/** Büyükken kaydırma sınırı: kenarlar ekranın içine girmesin. */
static void zoom_clamp(void)
{
    const int32_t zw = (int32_t)LV_HOR_RES * GALL_ZOOM_SCALE / 256;
    const int32_t zh = (int32_t)LV_VER_RES * GALL_ZOOM_SCALE / 256;
    const int32_t max_x = (zw - LV_HOR_RES) / 2;
    const int32_t max_y = (zh - LV_VER_RES) / 2;
    if (s_zoom_ox >  max_x) s_zoom_ox =  max_x;
    if (s_zoom_ox < -max_x) s_zoom_ox = -max_x;
    if (s_zoom_oy >  max_y) s_zoom_oy =  max_y;
    if (s_zoom_oy < -max_y) s_zoom_oy = -max_y;
}

static void zoom_reset(void)
{
    s_zoomed  = false;
    s_zoom_ox = s_zoom_oy = 0;
    s_panning = false;
    zoom_apply();
}

/* Büyükken parmakla gezinme */
static void full_press_cb(lv_event_t* e)
{
    (void)e;
    if (!s_zoomed) return;
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &s_pan_last);
    s_panning = true;
}

static void full_pressing_cb(lv_event_t* e)
{
    (void)e;
    if (!s_zoomed || !s_panning) return;
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t cur;
    lv_indev_get_point(indev, &cur);
    int32_t dx = cur.x - s_pan_last.x;
    int32_t dy = cur.y - s_pan_last.y;
    if (dx == 0 && dy == 0) return;
    s_pan_last = cur;
    s_zoom_ox += dx;
    s_zoom_oy += dy;
    zoom_clamp();
    lv_obj_set_pos(s_full_img, s_zoom_ox, s_zoom_oy);
}

static void full_img_click_cb(lv_event_t* e)
{
    (void)e;
    if (s_full_slot < 0) return;

    lv_indev_t* indev = lv_indev_get_act();
    lv_point_t pt = {0, 0};
    if (indev) lv_indev_get_point(indev, &pt);

    int64_t now = esp_timer_get_time();
    int32_t adx = pt.x - s_last_tap_pt.x; if (adx < 0) adx = -adx;
    int32_t ady = pt.y - s_last_tap_pt.y; if (ady < 0) ady = -ady;
    bool double_tap = (s_last_tap_us != 0) &&
                      ((now - s_last_tap_us) < (int64_t)GALL_DTAP_MS * 1000) &&
                      (adx <= GALL_DTAP_DIST && ady <= GALL_DTAP_DIST);

    if (double_tap) {
        tap_timer_cancel();             /* bekleyen çıkış iptal */
        s_last_tap_us = 0;              /* üçüncü dokunuş yeni sayılsın */
        if (s_zoomed) {
            ESP_LOGI(TAG, "zoom OFF");
            zoom_reset();
        } else {
            /* Dokunulan noktayı merkeze getir: ekran merkezinden sapmanın
             * ölçek farkı kadarı kaydırma olur. */
            s_zoomed  = true;
            s_zoom_ox = (LV_HOR_RES / 2 - pt.x) * (GALL_ZOOM_SCALE - 256) / 256;
            s_zoom_oy = (LV_VER_RES / 2 - pt.y) * (GALL_ZOOM_SCALE - 256) / 256;
            zoom_clamp();
            zoom_apply();
            ESP_LOGI(TAG, "zoom ON @%d,%d (ofs %d,%d)",
                     (int)pt.x, (int)pt.y, (int)s_zoom_ox, (int)s_zoom_oy);
        }
        return;
    }

    s_last_tap_us = now;
    s_last_tap_pt = pt;
    /* Tek dokunuş: HEMEN çıkma — ikinci dokunuş gelirse büyütme yapılacak.
     * Pencere dolduğunda ve büyütme yoksa tam ekrandan çıkılır. */
    tap_timer_cancel();
    if (!s_zoomed) {
        s_tap_timer = lv_timer_create([](lv_timer_t* t) {
            (void)t;
            s_tap_timer = NULL;
            if (s_full_slot >= 0 && !s_zoomed) exit_fullscreen();
        }, GALL_DTAP_MS, NULL);
        if (s_tap_timer) lv_timer_set_repeat_count(s_tap_timer, 1);
    }
}

static void gesture_cb(lv_event_t* /*e*/)
{
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_LEFT) {
        if (s_full_slot >= 0) { exit_fullscreen(); return; }
        s_pick_mode = false;
        app_manager_back();
    }
}

// ─── App lifecycle ────────────────────────────────────────────────────────────

static void on_start(lv_obj_t* screen)
{
    s_pick_mode = gallery_service_consume_pick_mode_request();
    gallery_pack_init();   /* Session 35: idempotent; paket yoksa sessizce boş */

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // ── Back button — direkt screen'e bağlı, container yok ───────────────────
    s_back_btn = lv_btn_create(screen);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_back_btn, 36, 36);
#else
    lv_obj_set_size(s_back_btn, 44, 44);
#endif
    lv_obj_set_pos(s_back_btn, 40, 20);  // move arrow slightly downward
    lv_obj_set_style_radius(s_back_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_make(0x1E, 0x20, 0x26), 0);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_back_btn, 1, 0);
    lv_obj_set_style_border_color(s_back_btn, lv_color_make(0x38, 0x3C, 0x44), 0);
    lv_obj_set_style_border_opa(s_back_btn, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
    /* v1.4.64: 44×44 görsel boyut Apple HIG sınırında ama display'in sol-üst
     * curve'üne çok yakın (pos x=40, y=20) — kullanıcı "tırnak ucu ile
     * tıklamak gerekiyor" diyordu. ext_click_area 28 → effective 100×100
     * isabet alanı (button merkezinden her yöne +28 px). Görsel değişmez. */
    lv_obj_set_ext_click_area(s_back_btn, 28);
    lv_obj_add_event_cb(s_back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* back_lbl = lv_label_create(s_back_btn);
    lv_obj_set_style_text_color(back_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    // ── Thumbnail grid ────────────────────────────────────────────────────────
    int32_t grid_y = GALL_HDR_H + GALL_GRID_TOP;
    int32_t grid_h = LV_VER_RES - grid_y;

    s_grid = lv_obj_create(screen);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, LV_HOR_RES, grid_h);
    lv_obj_align(s_grid, LV_ALIGN_TOP_MID, 0, grid_y);
    lv_obj_set_style_bg_color(s_grid, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_grid, GALL_SIDE_PAD, 0);
    lv_obj_set_style_pad_right(s_grid, GALL_SIDE_PAD, 0);
    lv_obj_set_style_pad_top(s_grid, 0, 0);
    lv_obj_set_style_pad_bottom(s_grid, 24, 0);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(s_grid, GALL_COL_GAP, 0);
    lv_obj_set_style_pad_row(s_grid, GALL_ROW_GAP, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_OFF);
    // Smooth momentum scroll — no snap stops, elastic disabled for tighter feel
    lv_obj_add_flag(s_grid,    LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(s_grid,  LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_snap_y(s_grid, LV_SCROLL_SNAP_NONE);
    // Grid scroll algılandığında tap'leri iptal et
    lv_obj_add_event_cb(s_grid, grid_scroll_cb, LV_EVENT_SCROLL, NULL);

    // ── Fullscreen overlay ────────────────────────────────────────────────────
    s_full_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_full_overlay);
    lv_obj_set_size(s_full_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(s_full_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_full_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_full_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_full_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_full_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_full_overlay, full_img_click_cb, LV_EVENT_CLICKED, NULL);
    /* Session 36: büyükken parmakla gezinme */
    lv_obj_add_event_cb(s_full_overlay, full_press_cb,    LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(s_full_overlay, full_pressing_cb, LV_EVENT_PRESSING, NULL);

    s_full_img = lv_image_create(s_full_overlay);
    lv_obj_set_pos(s_full_img, 0, 0);
    lv_obj_add_flag(s_full_img, LV_OBJ_FLAG_HIDDEN);

    // Populate grid with pre-rendered thumbnails (fast — just pixel copies)
    refresh_ui();
    /* Session 35: ızgara açılışta EN ÜSTTEN başlasın. Kart sayısı görüş
     * penceresini aşınca (paket görselleri geldi → 6 kart, 3 satır) LVGL
     * yerleşim sonrası kaydırmayı en alta bırakıyordu; ilk satır ekranın
     * üstünde kalıyordu. */
    if (s_grid) lv_obj_scroll_to_y(s_grid, 0, LV_ANIM_OFF);

    app_manager_enable_gestures(screen);
    lv_obj_add_event_cb(screen, gesture_cb, LV_EVENT_GESTURE, NULL);

    // Start WiFi AP on next LVGL tick — doesn't block the slide-in animation.
    // esp_wifi_stop()/esp_wifi_start() can take 200-500ms; deferring keeps
    // the transition smooth. Photos in PSRAM are already available immediately.
    lv_async_call(start_upload_service_async, NULL);
}

static void on_resume(void)
{
    s_pick_mode = gallery_service_consume_pick_mode_request();
    exit_fullscreen();
    refresh_ui();
    if (s_grid) lv_obj_scroll_to_y(s_grid, 0, LV_ANIM_OFF);   /* Session 35 */
}

static void on_pause(void) {}

static void on_stop(void)
{
    if (face_upload_service_is_running()) {
        face_upload_service_stop();
    }
    // Free pre-rendered thumbnails (slots may be deleted/repacked before next open)
    for (int i = 0; i < FACE_UPLOAD_MAX_SLOTS; i++) {
        if (s_thumb_raw[i]) { heap_caps_free(s_thumb_raw[i]); s_thumb_raw[i] = NULL; }
    }
    s_full_slot    = -1;
    tap_timer_cancel();   /* Session 36: LVGL nesnesinden bağımsız timer */
    s_pick_mode    = false;
    s_back_btn     = NULL;
    s_grid         = NULL;
    s_full_img     = NULL;
    s_full_overlay = NULL;
    for (int i = 0; i < FACE_UPLOAD_MAX_SLOTS; i++) s_item_btn[i] = NULL;
    for (int i = 0; i < GALL_PACK_MAX; i++) s_pack_btn[i] = NULL;
}

static bool on_event(const os_event_t* /*event*/) { return false; }

static const app_interface_t s_iface = {
    .on_start  = on_start,
    .on_resume = on_resume,
    .on_pause  = on_pause,
    .on_stop   = on_stop,
    .on_event  = on_event,
    .name      = "Gallery",
};

const app_interface_t* gallery_app_get_interface(void) { return &s_iface; }
