/**
 * @file store_app.cpp
 * @brief Store / mağaza uygulaması — v1.2.4 (2026-05-19).
 *
 * 2 tab + nav:
 *   Apps    → built-in app catalog (read-only browser)
 *   Faces   → watchface marketplace (stub, backend henüz yok)
 *
 * Lifecycle:
 *   on_start  → screen oluştur, tab bar + ilk tab içeriği (Apps default)
 *   on_resume → ekran tekrar açılırsa scroll position reset, vb.
 *   on_pause  → başka app öne geçtiyse — şimdilik no-op
 *   on_stop   → cleanup, widget pointer'larını NULL'la
 *   on_event  → şu an event abone değil, false döner
 *
 * Saat üzerinde mevcut uygulamalarda hiçbir şey bozulmasın diye:
 *   - tüm UI state'i local static
 *   - app_manager pattern'ine sıkı sıkıya bağlı
 *   - başka modüllere include eklemeden duruyor (sadece app_manager + lvgl)
 *
 * v1.2.4 ilk iskelet — UI içeriği aşağıdaki adımlarda doldurulacak:
 *   1. on_start: tab bar + boş content (bu commit) ✓
 *   2. Apps tab listesi (7 built-in app catalog)
 *   3. Detay sayfası (slide_forward + back)
 *   4. Faces tab stub (Coming soon)
 *   5. Polish (animations, theming)
 */

#include "store_app.h"
#include "store_state.h"             /* v1.2.6: catalog + install state */
#include "store_service.h"           /* v1.2.7: HTTP catalog fetch + cache */
#include "../../core/app_manager.h"
#include "../../core/os_config.h"
#include "../../services/wifi_service.h"   /* v1.2.6: install WiFi check */

#include <lvgl.h>
#include <esp_log.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "Store";

/* v1.2.6 (2026-05-21): Catalog ayrı dosyaya (store_state.h/.cpp) taşındı.
 * Apps tab: system app'ler + installed downloadable'lar
 * Faces tab: tüm downloadable'lar (status badge: Install / Installed) */

// ─── State (on_stop'ta NULL'lanır) ───────────────────────────────────────────

static lv_obj_t* s_screen      = NULL;
static lv_obj_t* s_header_lbl  = NULL;
static lv_obj_t* s_tab_apps    = NULL;     /* tab bar — "Apps" buton */
static lv_obj_t* s_tab_faces   = NULL;     /* tab bar — "Faces" buton */
static lv_obj_t* s_content     = NULL;     /* tab içeriği container */

typedef enum {
    STORE_TAB_APPS  = 0,
    STORE_TAB_FACES = 1,
} store_tab_t;

static store_tab_t s_current_tab = STORE_TAB_APPS;

/* Detay sayfası state — Apps tab'inde bir satıra tıklanınca açılır.
 * Content area iki child barındırır: list view (s_apps_list) ve detail
 * view (s_detail_view). İkisi de child of s_content. Hangisi visible ise
 * diğeri hidden. Apple Watch detail sayfası tarzı — overlay yerine
 * content-replace pattern (slide_forward animasyon yok, basit toggle). */
static lv_obj_t* s_apps_list    = NULL;     /* Apps tab — kategori listesi */
static lv_obj_t* s_faces_panel  = NULL;     /* Faces tab — stub panel */
static lv_obj_t* s_detail_view  = NULL;     /* Apps detail sayfası */
static int       s_detail_entry_idx = -1;   /* Hangi catalog entry'si açık */

/* v1.2.6: detail page render state. NORMAL = Open/Install/Uninstall butonları.
 * INSTALLING = simulated download in progress (spinner + "Installing..."). */
typedef enum {
    DETAIL_STATE_NORMAL     = 0,
    DETAIL_STATE_INSTALLING = 1,
} detail_state_t;

static detail_state_t s_detail_state = DETAIL_STATE_NORMAL;
static lv_timer_t*    s_install_timer = NULL;   /* mock download süresi */
#define INSTALL_MOCK_DURATION_MS 1500             /* Apple App Store tarzı kısa "indirme" */

// ─── Forward declarations ────────────────────────────────────────────────────

/* ─── Session 38b: liste satırı ölçüleri ──────────────────────────────────────
 * Satır yüksekliği = dikey ADIM olmalı; eskiden LCD128'de satır 54 px iken
 * adım 80 px sabit kodluydu → satırlar arasında 26 px ölü boşluk kalıyor ve
 * ayraç kendi satırından kopuk duruyordu. Ayrıca ikon (18..52) ile metin (82)
 * arasında 30 px boşluk vardı ve alt yazı sağdaki okun ALTINA giriyordu.
 * C6'daki 368 px'lik yerleşimin oranları 240'a ölçeklendi:
 *   ikon sağı + 10 px → metin; metin genişliği ok'tan önce biter (DOT ile
 *   kırpılır); ayraç satırın en altında. */
#ifdef BOARD_VARIANT_LCD128
#define ST_SUB_W     118          /* rozetli satırlar için — rozet 186'da başlar */
#define ST_PANEL_W   240
#define ST_PANEL_H   178
/* Detay sayfası — 240'ta ikon/başlık/açıklama küçültülür, açıklama paneli
 * aşmayacak genişliğe (176) alınır (eskiden 360 px, x=-60'tan başlıyordu). */
#define ST_D_ICON    48
#define ST_D_ICON_Y  30
#define ST_D_NAME_Y  88
#define ST_D_DESC_W  176
#define ST_D_DESC_Y  114
#define ST_D_BTN_F   (&lv_font_montserrat_16)   /* Open / Install */
#define ST_D_BTN2_F  (&lv_font_montserrat_14)   /* Uninstall */
#define ST_D_BTN_STEP 34                        /* Open → Uninstall arası */
#define ST_LIST_PAD_B 60   /* yuvarlak ekran: son öğe alt kirişten yukarı kaydırılabilsin */
#define ST_SCR_W     240
#define ST_BADGE_R   206          /* durum rozetinin sağ kenarı */
#define ST_ROW_H     62
#define ST_ICON_SZ   36
#define ST_ICON_X    16
#define ST_ICON_Y    13
#define ST_TEXT_X    62
#define ST_NAME_Y    11
#define ST_SUB_Y     33
#define ST_TEXT_W    118          /* 62..180 — ok/rozet 186+ */
#define ST_CHEV_X    196
#define ST_CHEV_Y    22
#define ST_SEP_X     30
#define ST_SEP_W     180
#else
#define ST_SUB_W     280
#define ST_PANEL_W   410
#define ST_PANEL_H   380
#define ST_D_ICON    92
#define ST_D_ICON_Y  44
#define ST_D_NAME_Y  148
#define ST_D_DESC_W  360
#define ST_D_DESC_Y  192
#define ST_D_BTN_F   (&lv_font_montserrat_24)
#define ST_D_BTN2_F  (&lv_font_montserrat_22)
#define ST_D_BTN_STEP (36 + 18)
#define ST_LIST_PAD_B 0
#define ST_SCR_W     410
#define ST_BADGE_R   391
#define ST_ROW_H     80
#define ST_ICON_SZ   50
#define ST_ICON_X    18
#define ST_ICON_Y    15
#define ST_TEXT_X    82
#define ST_NAME_Y    14
#define ST_SUB_Y     44
#define ST_TEXT_W    280
#define ST_CHEV_X    380
#define ST_CHEV_Y    30
#define ST_SEP_X     15
#define ST_SEP_W     380
#endif

static void build_header(lv_obj_t* screen);
static void build_tab_bar(lv_obj_t* screen);
static void build_content(lv_obj_t* screen);
static void switch_to_tab(store_tab_t tab);
static void tab_apps_clicked_cb(lv_event_t* e);
static void tab_faces_clicked_cb(lv_event_t* e);
static void build_apps_list(lv_obj_t* parent);
static void build_faces_panel(lv_obj_t* parent);
static void show_app_detail(int catalog_idx);
static void hide_app_detail(void);
static void app_row_clicked_cb(lv_event_t* e);
static void detail_back_cb(lv_event_t* e);
static void detail_open_cb(lv_event_t* e);

// ─── Header (sabit üst başlık) ───────────────────────────────────────────────

static void build_header(lv_obj_t* screen)
{
    s_header_lbl = lv_label_create(screen);
    lv_obj_set_style_text_color(s_header_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_header_lbl, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(s_header_lbl, &lv_font_montserrat_28, 0);
#endif
    lv_label_set_text(s_header_lbl, "Store");
#ifdef BOARD_VARIANT_LCD128
    lv_obj_align(s_header_lbl, LV_ALIGN_TOP_MID, 0, 14);
#else
    lv_obj_align(s_header_lbl, LV_ALIGN_TOP_MID, 0, 22);
#endif
    lv_obj_clear_flag(s_header_lbl, LV_OBJ_FLAG_CLICKABLE);
}

// ─── Tab bar (Apps / Faces toggle) ───────────────────────────────────────────
//
// Apple Watch tarzı pill segmented control. İki "buton" (label) yan yana,
// aktif olan beyaz dolgulu kart, pasif olan gri text. Tıklayınca switch.

static lv_obj_t* make_tab_button(lv_obj_t* parent, const char* text,
                                  int32_t x, int32_t y,
                                  lv_event_cb_t cb)
{
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(btn, 90, 28);
#else
    lv_obj_set_size(btn, 158, 38);
#endif
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x2C, 0x2C, 0x2E), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
#endif
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void build_tab_bar(lv_obj_t* screen)
{
#ifdef BOARD_VARIANT_LCD128
    const int32_t TAB_Y = 38;
    s_tab_apps  = make_tab_button(screen, "Apps",  26,        TAB_Y, tab_apps_clicked_cb);
    s_tab_faces = make_tab_button(screen, "Faces", 26 + 90+8, TAB_Y, tab_faces_clicked_cb);
#else
    const int32_t TAB_Y = 68;
    s_tab_apps  = make_tab_button(screen, "Apps",  46,         TAB_Y, tab_apps_clicked_cb);
    s_tab_faces = make_tab_button(screen, "Faces", 46 + 158+10, TAB_Y, tab_faces_clicked_cb);
#endif
}

// Forward declarations — switch_to_tab() rebuild_faces_panel() çağırıyor (v1.4.61)
static void rebuild_apps_list(void);
static void rebuild_faces_panel(void);

static void apply_tab_visual(void)
{
    if (!s_tab_apps || !s_tab_faces) return;

    /* Aktif: beyaz bg + siyah text. Pasif: koyu gri bg + beyaz text. */
    bool apps_active = (s_current_tab == STORE_TAB_APPS);
    lv_obj_t* apps_lbl  = lv_obj_get_child(s_tab_apps,  0);
    lv_obj_t* faces_lbl = lv_obj_get_child(s_tab_faces, 0);

    lv_obj_set_style_bg_color(s_tab_apps,
        apps_active ? lv_color_white() : lv_color_make(0x2C, 0x2C, 0x2E), 0);
    if (apps_lbl) lv_obj_set_style_text_color(apps_lbl,
        apps_active ? lv_color_black() : lv_color_white(), 0);

    lv_obj_set_style_bg_color(s_tab_faces,
        !apps_active ? lv_color_white() : lv_color_make(0x2C, 0x2C, 0x2E), 0);
    if (faces_lbl) lv_obj_set_style_text_color(faces_lbl,
        !apps_active ? lv_color_black() : lv_color_white(), 0);
}

// ─── Content area (tab içeriği) ──────────────────────────────────────────────

static void build_content(lv_obj_t* screen)
{
    s_content = lv_obj_create(screen);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, ST_PANEL_W, ST_PANEL_H);
    #ifdef BOARD_VARIANT_LCD128
lv_obj_set_pos(s_content, 0, 74);
#else
lv_obj_set_pos(s_content, 0, 120);
#endif
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    /* Apps list + Faces panel + Detail view child'ları — başlangıçta hepsi
     * yaratılır, sonra visibility ile geçiş yapılır. Apps default visible. */
    build_apps_list(s_content);
    build_faces_panel(s_content);
    /* Detail view lazy — ilk app satır tıklamasında yaratılır. */
    if (s_faces_panel)  lv_obj_add_flag(s_faces_panel, LV_OBJ_FLAG_HIDDEN);
}

static void switch_to_tab(store_tab_t tab)
{
    if (tab == s_current_tab) return;
    s_current_tab = tab;
    apply_tab_visual();

    /* Eğer detail açıksa, tab değişiminde kapat. */
    hide_app_detail();

    /* Visibility swap */
    if (s_apps_list && s_faces_panel) {
        if (tab == STORE_TAB_APPS) {
            lv_obj_clear_flag(s_apps_list,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_faces_panel,  LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_apps_list,    LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_faces_panel, LV_OBJ_FLAG_HIDDEN);
            /* v1.4.61: Faces tab'e her geçişte panel'i yeniden çiz. Backend
             * catalog Apps tab açıkken async olarak dolmuş olabilir; eğer
             * burada rebuild etmezsek build sırasındaki (cache boş ↔ fallback)
             * snapshot kalır, kullanıcı eski 2-entry compile-time listesini
             * görür. Cheap operation (panel objelerini lv_obj_clean + birkaç
             * label yaratımı), kullanıcı tab tıkladığında run eder. */
            rebuild_faces_panel();
        }
    }
}

// ─── Tab refresh helper'ları ─────────────────────────────────────────────────
//
// v1.2.6: install state değiştiğinde Apps + Faces listelerini yeniden çiz.
// Önceden compile-time fixed liste idi; şimdi runtime'da NVS-driven.
// (Forward decl'lar yukarıda, switch_to_tab() içinde de çağrılıyor.)

// ─── Apps tab — system + installed downloadable listesi ──────────────────────

/* Tek satır: solda renkli icon kutusu, sağda ad + subtitle.
 * Satır CLICKABLE → tıklanınca detail sayfası açılır. */
static void build_app_row(lv_obj_t* parent, int catalog_idx, int32_t y)
{
    const store_app_entry_t* e = store_catalog_at((size_t)catalog_idx);
    if (!e) return;

    /* Row container — CLICKABLE, PRESSED state'te hafif gri bg */
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, ST_SCR_W, ST_ROW_H);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_color(row, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_make(0x1C, 0x1C, 0x1E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(row, (void*)(intptr_t)catalog_idx);
    lv_obj_add_event_cb(row, app_row_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Icon kutusu — 50×50 px renkli yuvarlak köşe */
    lv_obj_t* icon_bg = lv_obj_create(row);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, ST_ICON_SZ, ST_ICON_SZ);
    lv_obj_set_pos(icon_bg, ST_ICON_X, ST_ICON_Y);
    lv_obj_set_style_bg_color(icon_bg, lv_color_make(e->cr, e->cg, e->cb), 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_bg, 12, 0);
    lv_obj_clear_flag(icon_bg, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(icon_bg, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* icon_lbl = lv_label_create(icon_bg);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_24, 0);
#endif
    lv_label_set_text(icon_lbl, e->icon_symbol);
    lv_obj_center(icon_lbl);
    lv_obj_clear_flag(icon_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(icon_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Ad — üst satır, beyaz */
    lv_obj_t* name_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(name_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_22, 0);
#endif
    lv_label_set_text(name_lbl, e->name);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    /* Tek satır + "..." : LONG_DOT ancak yükseklik SABİT olduğunda kırpar,
     * aksi halde etiket 2. satıra sarıp okun altına giriyordu. */
    lv_obj_set_size(name_lbl, ST_TEXT_W,
                    lv_font_get_line_height(lv_obj_get_style_text_font(name_lbl, LV_PART_MAIN)));
    lv_obj_set_pos(name_lbl, ST_TEXT_X, ST_NAME_Y);
    lv_obj_clear_flag(name_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(name_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Subtitle — alt satır, gri */
    lv_obj_t* sub_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(sub_lbl, lv_color_make(0x8E, 0x8E, 0x93), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(sub_lbl, &lv_font_montserrat_12, 0);
#else
    lv_obj_set_style_text_font(sub_lbl, &lv_font_montserrat_16, 0);
#endif
    lv_label_set_text(sub_lbl, e->subtitle);
    lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_size(sub_lbl, ST_TEXT_W,
                    lv_font_get_line_height(lv_obj_get_style_text_font(sub_lbl, LV_PART_MAIN)));
    lv_obj_set_pos(sub_lbl, ST_TEXT_X, ST_SUB_Y);
    lv_obj_clear_flag(sub_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sub_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Chevron (sağ ok) */
    lv_obj_t* chev = lv_label_create(row);
    lv_obj_set_style_text_color(chev, lv_color_make(0x8E, 0x8E, 0x93), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(chev, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(chev, &lv_font_montserrat_20, 0);
#endif
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_set_pos(chev, ST_CHEV_X, ST_CHEV_Y);
    lv_obj_clear_flag(chev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chev, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Separator (alt çizgi) */
    lv_obj_t* sep = lv_obj_create(row);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, ST_SEP_W, 1);
    lv_obj_set_pos(sep, ST_SEP_X, ST_ROW_H - 1);
    lv_obj_set_style_bg_color(sep, lv_color_make(0x38, 0x38, 0x3A), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sep, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(sep, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void build_apps_list(lv_obj_t* parent)
{
    /* Scrollable container — row sayısı duruma göre değişir, scroll gerekli. */
    s_apps_list = lv_obj_create(parent);
    lv_obj_remove_style_all(s_apps_list);
    lv_obj_set_size(s_apps_list, ST_PANEL_W, ST_PANEL_H);
    lv_obj_set_pos(s_apps_list, 0, 0);
    lv_obj_set_style_pad_bottom(s_apps_list, ST_LIST_PAD_B, 0);
    lv_obj_set_style_bg_opa(s_apps_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_apps_list, 0, 0);
    lv_obj_set_style_pad_all(s_apps_list, 0, 0);
    lv_obj_set_scroll_dir(s_apps_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_apps_list, LV_SCROLLBAR_MODE_OFF);

    rebuild_apps_list();
}

/* Apps tab içeriğini yeniden çiz — install state değişince çağrılır.
 * Filtering: system entry'ler her zaman, downloadable'lar sadece installed. */
static void rebuild_apps_list(void)
{
    if (!s_apps_list) return;
    lv_obj_clean(s_apps_list);

    int32_t y = 0;
    const size_t count = store_catalog_count();
    for (size_t i = 0; i < count; i++) {
        const store_app_entry_t* e = store_catalog_at(i);
        if (!e) continue;
        /* Filtering: system her zaman, downloadable sadece installed */
        if (!e->is_system && !store_is_installed(e->id)) continue;

        build_app_row(s_apps_list, (int)i, y);
        y += ST_ROW_H;
    }
}

// ─── Faces tab — downloadable apps mağazası ──────────────────────────────────
//
// v1.2.6: stub "Coming Soon" kaldırıldı. Artık catalog'daki tüm downloadable
// app'ler (is_system=false) burada listelenir. Status badge:
//   - Installed → yeşil/gri "Installed" yazısı (uninstall butonu detail'de)
//   - Available → mavi "Install" yazısı
// Backend gelince: install butonuna basınca HTTP fetch tetiklenecek. Şimdilik
// mock — sadece NVS flag toggle.

/* Faces tab row — Apps tab row ile aynı yapı + status badge sağda */
static void build_face_row(lv_obj_t* parent, int catalog_idx, int32_t y)
{
    const store_app_entry_t* e = store_catalog_at((size_t)catalog_idx);
    if (!e) return;
    bool installed = store_is_installed(e->id);

    /* Row container — CLICKABLE, PRESSED state'te hafif gri bg */
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, ST_SCR_W, ST_ROW_H);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_color(row, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_make(0x1C, 0x1C, 0x1E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(row, (void*)(intptr_t)catalog_idx);
    lv_obj_add_event_cb(row, app_row_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Icon kutusu */
    lv_obj_t* icon_bg = lv_obj_create(row);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, ST_ICON_SZ, ST_ICON_SZ);
    lv_obj_set_pos(icon_bg, ST_ICON_X, ST_ICON_Y);
    lv_obj_set_style_bg_color(icon_bg, lv_color_make(e->cr, e->cg, e->cb), 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_bg, 12, 0);
    lv_obj_clear_flag(icon_bg, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(icon_bg, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* icon_lbl = lv_label_create(icon_bg);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_24, 0);
#endif
    lv_label_set_text(icon_lbl, e->icon_symbol);
    lv_obj_center(icon_lbl);
    lv_obj_clear_flag(icon_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(icon_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Ad — üst */
    lv_obj_t* name_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(name_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_22, 0);
#endif
    lv_label_set_text(name_lbl, e->name);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    /* Tek satır + "..." : LONG_DOT ancak yükseklik SABİT olduğunda kırpar,
     * aksi halde etiket 2. satıra sarıp okun altına giriyordu. */
    lv_obj_set_size(name_lbl, ST_TEXT_W,
                    lv_font_get_line_height(lv_obj_get_style_text_font(name_lbl, LV_PART_MAIN)));
    lv_obj_set_pos(name_lbl, ST_TEXT_X, ST_NAME_Y);
    lv_obj_clear_flag(name_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(name_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Subtitle — alt */
    lv_obj_t* sub_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(sub_lbl, lv_color_make(0x8E, 0x8E, 0x93), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(sub_lbl, &lv_font_montserrat_12, 0);
#else
    lv_obj_set_style_text_font(sub_lbl, &lv_font_montserrat_16, 0);
#endif
    lv_label_set_text(sub_lbl, e->subtitle);
    lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_size(sub_lbl, ST_SUB_W,
                    lv_font_get_line_height(lv_obj_get_style_text_font(sub_lbl, LV_PART_MAIN)));
    lv_obj_set_pos(sub_lbl, ST_TEXT_X, ST_SUB_Y);
    lv_obj_clear_flag(sub_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sub_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* Status badge sağda — "Installed" (gri) veya "Install" (mavi) */
    lv_obj_t* badge = lv_label_create(row);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_20, 0);
#endif
    if (installed) {
        lv_obj_set_style_text_color(badge, lv_color_make(0x8E, 0x8E, 0x93), 0);
#ifdef BOARD_VARIANT_LCD128
        /* Session 38b: 240'ta "Installed" (90 px) alt yazının üstüne
         * biniyordu — App Store kalıbı: kısa simge. */
        lv_label_set_text(badge, LV_SYMBOL_OK);
#else
        lv_label_set_text(badge, "Installed");
#endif
    } else {
        lv_obj_set_style_text_color(badge, lv_color_make(0x0A, 0x84, 0xFF), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_label_set_text(badge, LV_SYMBOL_DOWNLOAD);
#else
        lv_label_set_text(badge, "Install");
#endif
    }
    lv_obj_update_layout(badge);
    lv_obj_set_pos(badge, ST_BADGE_R - lv_obj_get_width(badge), ST_CHEV_Y);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Alt separator */
    lv_obj_t* sep = lv_obj_create(row);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, ST_SEP_W, 1);
    lv_obj_set_pos(sep, ST_SEP_X, ST_ROW_H - 1);
    lv_obj_set_style_bg_color(sep, lv_color_make(0x38, 0x38, 0x3A), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sep, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(sep, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void build_faces_panel(lv_obj_t* parent)
{
    s_faces_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_faces_panel);
    lv_obj_set_size(s_faces_panel, ST_PANEL_W, ST_PANEL_H);
    lv_obj_set_pos(s_faces_panel, 0, 0);
    lv_obj_set_style_pad_bottom(s_faces_panel, ST_LIST_PAD_B, 0);
    lv_obj_set_style_bg_opa(s_faces_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_faces_panel, 0, 0);
    lv_obj_set_style_pad_all(s_faces_panel, 0, 0);
    lv_obj_set_scroll_dir(s_faces_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_faces_panel, LV_SCROLLBAR_MODE_OFF);

    rebuild_faces_panel();
}

/* v1.2.7 (2026-05-22): Remote catalog entry row. Apple App Store benzeri —
 * "Install" / "Installed" / "Update Required" badge'leri ile. */
static void remote_face_row_clicked_cb(lv_event_t* e);

static void build_remote_face_row(lv_obj_t* parent, int remote_idx, int32_t y,
                                   const store_remote_app_t* r)
{
    if (!r) return;
    const bool binary_ok = r->binary_available;
    const bool installed = binary_ok && store_is_installed(r->mapped_id);

    /* Row container */
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, ST_SCR_W, ST_ROW_H);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_color(row, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_make(0x1C, 0x1C, 0x1E), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(row, (void*)(intptr_t)remote_idx);
    lv_obj_add_event_cb(row, remote_face_row_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Icon kutusu — remote'tan gelen renk + sembol */
    lv_obj_t* icon_bg = lv_obj_create(row);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, ST_ICON_SZ, ST_ICON_SZ);
    lv_obj_set_pos(icon_bg, ST_ICON_X, ST_ICON_Y);
    lv_obj_set_style_bg_color(icon_bg, lv_color_make(r->cr, r->cg, r->cb), 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_bg, 12, 0);
    /* Eğer binary yoksa icon biraz solgun (50% opa) — UX hint */
    if (!binary_ok) {
        lv_obj_set_style_bg_opa(icon_bg, LV_OPA_50, 0);
    }
    lv_obj_clear_flag(icon_bg, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(icon_bg, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* icon_lbl = lv_label_create(icon_bg);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_24, 0);
#endif
    lv_label_set_text(icon_lbl, store_icon_from_string(r->icon_symbol));
    lv_obj_center(icon_lbl);
    lv_obj_clear_flag(icon_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(icon_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Ad */
    lv_obj_t* name_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(name_lbl,
        binary_ok ? lv_color_white() : lv_color_make(0x8E, 0x8E, 0x93), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_22, 0);
#endif
    lv_label_set_text(name_lbl, r->name);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    /* Tek satır + "..." : LONG_DOT ancak yükseklik SABİT olduğunda kırpar,
     * aksi halde etiket 2. satıra sarıp okun altına giriyordu. */
    lv_obj_set_size(name_lbl, ST_TEXT_W,
                    lv_font_get_line_height(lv_obj_get_style_text_font(name_lbl, LV_PART_MAIN)));
    lv_obj_set_pos(name_lbl, ST_TEXT_X, ST_NAME_Y);
    lv_obj_clear_flag(name_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(name_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Subtitle */
    lv_obj_t* sub_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(sub_lbl, lv_color_make(0x8E, 0x8E, 0x93), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(sub_lbl, &lv_font_montserrat_12, 0);
#else
    lv_obj_set_style_text_font(sub_lbl, &lv_font_montserrat_16, 0);
#endif
    lv_label_set_text(sub_lbl, r->subtitle);
    lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_size(sub_lbl, ST_TEXT_W,
                    lv_font_get_line_height(lv_obj_get_style_text_font(sub_lbl, LV_PART_MAIN)));
    lv_obj_set_pos(sub_lbl, ST_TEXT_X, ST_SUB_Y);
    lv_obj_clear_flag(sub_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sub_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Status badge — 3 durum */
    lv_obj_t* badge = lv_label_create(row);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(badge, &lv_font_montserrat_20, 0);
#endif
    if (!binary_ok) {
        /* Saatte binary yok veya firmware eski → "Update Req." kısa gösterim */
        lv_obj_set_style_text_color(badge, lv_color_make(0xFF, 0x9F, 0x0A), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_label_set_text(badge, LV_SYMBOL_WARNING);
#else
        lv_label_set_text(badge, "Update");
#endif
    } else if (installed) {
        lv_obj_set_style_text_color(badge, lv_color_make(0x8E, 0x8E, 0x93), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_label_set_text(badge, LV_SYMBOL_OK);
#else
        lv_label_set_text(badge, "Installed");
#endif
    } else {
        lv_obj_set_style_text_color(badge, lv_color_make(0x0A, 0x84, 0xFF), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_label_set_text(badge, LV_SYMBOL_DOWNLOAD);
#else
        lv_label_set_text(badge, "Install");
#endif
    }
    lv_obj_update_layout(badge);
    lv_obj_set_pos(badge, ST_BADGE_R - lv_obj_get_width(badge), ST_CHEV_Y);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Alt separator */
    lv_obj_t* sep = lv_obj_create(row);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, ST_SEP_W, 1);
    lv_obj_set_pos(sep, ST_SEP_X, ST_ROW_H - 1);
    lv_obj_set_style_bg_color(sep, lv_color_make(0x38, 0x38, 0x3A), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sep, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(sep, LV_OBJ_FLAG_EVENT_BUBBLE);
}

/* "Requires firmware update" modal — binary yok ya da firmware eski. */
static lv_obj_t* s_update_required_modal = NULL;

static void update_required_ok_cb(lv_event_t* /*e*/)
{
    if (s_update_required_modal) {
        lv_obj_delete_async(s_update_required_modal);
        s_update_required_modal = NULL;
    }
}

static void show_update_required_modal(const store_remote_app_t* r)
{
    if (!r || s_update_required_modal) return;

    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 410, 502);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    s_update_required_modal = overlay;

    lv_obj_t* card = lv_obj_create(overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 340, 260);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_make(0x1C, 0x1C, 0x1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_clear_flag(card, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    /* Sarı/turuncu uyarı icon */
    lv_obj_t* icon = lv_label_create(card);
    lv_obj_set_style_text_color(icon, lv_color_make(0xFF, 0x9F, 0x0A), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_46, 0);
    lv_label_set_text(icon, LV_SYMBOL_WARNING);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    /* Başlık */
    char title_buf[80];
    snprintf(title_buf, sizeof(title_buf), "%s", r->name);
    lv_obj_t* title = lv_label_create(card);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_label_set_text(title, title_buf);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    /* Açıklama — version + neden install edilemez */
    char desc_buf[160];
    int major = r->min_firmware_version_code / 10000;
    int minor = (r->min_firmware_version_code / 100) % 100;
    int patch = r->min_firmware_version_code % 100;
    snprintf(desc_buf, sizeof(desc_buf),
             "Requires firmware v%d.%d.%d or newer. "
             "Update your watch to install this app.",
             major, minor, patch);

    lv_obj_t* desc = lv_label_create(card);
    lv_obj_set_style_text_color(desc, lv_color_make(0xC7, 0xC7, 0xCC), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(desc, 4, 0);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, 300);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(desc, desc_buf);
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_clear_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    /* OK link */
    lv_obj_t* ok_lbl = lv_label_create(card);
    lv_obj_set_style_text_color(ok_lbl, lv_color_make(0x0A, 0x84, 0xFF), 0);
    lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_align(ok_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(ok_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(ok_lbl, 28);
    lv_obj_add_event_cb(ok_lbl, update_required_ok_cb, LV_EVENT_CLICKED, NULL);
}

/* Helper — verilen app_id_t için compile-time catalog index'ini bul */
static int find_catalog_idx_by_app_id(app_id_t id)
{
    const size_t count = store_catalog_count();
    for (size_t i = 0; i < count; i++) {
        const store_app_entry_t* e = store_catalog_at(i);
        if (e && e->id == id) return (int)i;
    }
    return -1;
}

static void remote_face_row_clicked_cb(lv_event_t* e)
{
    lv_obj_t* row = (lv_obj_t*)lv_event_get_current_target(e);
    int remote_idx = (int)(intptr_t)lv_obj_get_user_data(row);
    const store_remote_app_t* r = store_service_remote_at((size_t)remote_idx);
    if (!r) return;

    if (r->binary_available) {
        /* Saatte binary var + firmware uygun → compile-time detail page */
        int compile_idx = find_catalog_idx_by_app_id(r->mapped_id);
        if (compile_idx >= 0) {
            show_app_detail(compile_idx);
            return;
        }
        /* Defensive: binary_available true ama mapping yok (mantıken olmaz) */
    }
    /* Binary yok / firmware eski → "Requires firmware update" modal */
    show_update_required_modal(r);
}

/* Faces tab içeriğini yeniden çiz.
 * v1.2.7: backend cache varsa onu göster, yoksa compile-time fallback. */
static void rebuild_faces_panel(void)
{
    if (!s_faces_panel) return;
    lv_obj_clean(s_faces_panel);

    int32_t y = 0;
    bool any = false;

    if (store_service_has_cache() && store_service_remote_count() > 0) {
        /* Remote catalog modu — backend'den gelen liste */
        const size_t count = store_service_remote_count();
        for (size_t i = 0; i < count; i++) {
            const store_remote_app_t* r = store_service_remote_at(i);
            if (!r || !r->app_id[0]) continue;
            build_remote_face_row(s_faces_panel, (int)i, y, r);
            y += ST_ROW_H;
            any = true;
        }
    } else {
        /* Fallback — compile-time downloadable entries (WiFi yok veya ilk
         * açılış cache henüz boş). */
        const size_t count = store_catalog_count();
        for (size_t i = 0; i < count; i++) {
            const store_app_entry_t* e = store_catalog_at(i);
            if (!e || e->is_system) continue;
            build_face_row(s_faces_panel, (int)i, y);
            y += ST_ROW_H;
            any = true;
        }
    }

    /* Hiçbir entry yoksa placeholder */
    if (!any) {
        lv_obj_t* title = lv_label_create(s_faces_panel);
        lv_obj_set_style_text_color(title, lv_color_make(0x8E, 0x8E, 0x93), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_label_set_text(title, "No apps available");
        lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    }
}

// ─── App detail sayfası ──────────────────────────────────────────────────────

/* Forward decl — confirmation modal ve install/uninstall actions */
static void detail_install_cb(lv_event_t* e);
static void detail_uninstall_cb(lv_event_t* e);
static void show_uninstall_confirm(int catalog_idx);
static void show_no_internet_modal(void);   /* v1.2.6: WiFi-off install attempt */

static void show_app_detail(int catalog_idx)
{
    if (catalog_idx < 0 || catalog_idx >= (int)store_catalog_count()) return;
    if (!s_content) return;
    s_detail_entry_idx = catalog_idx;
    const store_app_entry_t* e = store_catalog_at((size_t)catalog_idx);
    if (!e) return;

    /* Detail view'ı lazy yarat (ilk kez açılıyorsa). Sonraki açılışlarda
     * eski child'ları temizleyip yeniden doldur.
     *
     * v1.2.6: Scrollable yapıldı — description uzunsa veya yeni
     * downloadable app'lerde description+butonlar 380px'i aştığında
     * kullanıcı scroll edebilsin. */
    if (!s_detail_view) {
        s_detail_view = lv_obj_create(s_content);
        lv_obj_remove_style_all(s_detail_view);
        lv_obj_set_size(s_detail_view, ST_PANEL_W, ST_PANEL_H);
        lv_obj_set_pos(s_detail_view, 0, 0);
        lv_obj_set_style_bg_color(s_detail_view, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_detail_view, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_detail_view, 0, 0);
        lv_obj_set_style_pad_all(s_detail_view, 0, 0);
        lv_obj_set_scroll_dir(s_detail_view, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s_detail_view, LV_SCROLLBAR_MODE_OFF);
        /* Yuvarlak ekranda panelin altı (y=252) ekranın dışında ve alt kiriş
         * dar → son buton kaydırılsa da bezel altında kalıyordu. Alt pay,
         * son öğenin görünür orta banda kadar yukarı çekilmesine izin verir. */
        lv_obj_set_style_pad_bottom(s_detail_view, ST_LIST_PAD_B, 0);
    } else {
        lv_obj_clean(s_detail_view);
    }
    lv_obj_clear_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);

    /* Geri butonu — sol üst (< Back)
     * v1.4.64: ext_click_area 14 → 28, pos x=14 → 24 (display sol-üst
     * curve'inden uzaklaş). Kullanıcı raporu: küçük back arrow zor tıklanıyor. */
    lv_obj_t* back = lv_label_create(s_detail_view);
    lv_obj_set_style_text_color(back, lv_color_make(0x0A, 0x84, 0xFF), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(back, &lv_font_montserrat_14, 0);
    lv_label_set_text(back, LV_SYMBOL_LEFT " Store");
    lv_obj_set_pos(back, 56, 10);   /* yuvarlak ekranda sol-üst köşe bezel altı */
#else
    lv_obj_set_style_text_font(back, &lv_font_montserrat_20, 0);
    lv_label_set_text(back, LV_SYMBOL_LEFT "  Store");
    lv_obj_set_pos(back, 24, 14);
#endif
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 28);
    lv_obj_add_event_cb(back, detail_back_cb, LV_EVENT_CLICKED, NULL);

    /* Büyük icon — orta üst */
    lv_obj_t* icon_bg = lv_obj_create(s_detail_view);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, ST_D_ICON, ST_D_ICON);
    lv_obj_set_style_bg_color(icon_bg, lv_color_make(e->cr, e->cg, e->cb), 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_bg, 22, 0);
    lv_obj_align(icon_bg, LV_ALIGN_TOP_MID, 0, ST_D_ICON_Y);
    lv_obj_clear_flag(icon_bg, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t* icon_lbl = lv_label_create(icon_bg);
    lv_obj_set_style_text_color(icon_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_24, 0);
#else
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_46, 0);
#endif
    lv_label_set_text(icon_lbl, e->icon_symbol);
    lv_obj_center(icon_lbl);
    lv_obj_clear_flag(icon_lbl, LV_OBJ_FLAG_CLICKABLE);

    /* App adı — büyük başlık */
    lv_obj_t* name = lv_label_create(s_detail_view);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(name, &lv_font_montserrat_28, 0);
#endif
    lv_label_set_text(name, e->name);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, ST_D_NAME_Y);
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

    /* Açıklama — multi-line wrap */
    lv_obj_t* desc = lv_label_create(s_detail_view);
    lv_obj_set_style_text_color(desc, lv_color_make(0xC7, 0xC7, 0xCC), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_20, 0);
#endif
    lv_obj_set_style_text_line_space(desc, 4, 0);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, ST_D_DESC_W);
    lv_label_set_text(desc, e->description);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, ST_D_DESC_Y);
    lv_obj_clear_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    /* v1.2.6: action buttons state-dependent + DİNAMİK yerleşim.
     *
     * Önceki sabit BOTTOM_MID yerleşim description uzun olduğunda text'in
     * üzerine biniyordu (kullanıcı raporu, Step Counter). Şimdi description
     * bbox'ı ölçülüp butonlar onun ALTINA yerleştiriliyor. Detail view
     * scrollable olduğu için içerik 380px'i aşarsa kullanıcı scroll eder.
     *
     * 3 render durumu:
     *   NORMAL + installed:    Open + Uninstall (downloadable) / Open (system)
     *   NORMAL + !installed:   Install (downloadable)
     *   INSTALLING:            spinner + "Installing..." (geri buton aktif)
     */
    lv_obj_update_layout(desc);
    const int32_t desc_bottom = lv_obj_get_y(desc) + lv_obj_get_height(desc);
#ifdef BOARD_VARIANT_LCD128
    int32_t btn_y = desc_bottom + 14;   /* 240'ta daha az kaydırma gereksin */
#else
    int32_t btn_y = desc_bottom + 28;   /* description'dan 28px aşağı */
#endif

    bool installed = store_is_installed(e->id);

    if (s_detail_state == DETAIL_STATE_INSTALLING) {
        /* Mock download in progress — text-only indicator.
         *
         * Önceki denemeler:
         *   1. lv_spinner: arc rotation → display buffer chunk timing race
         *      → arc dağınık görünüyordu
         *   2. lv_bar + lv_anim: bar fill 0→100 animation → aynı race, fill
         *      bölgesinin yarısı bir frame'de diğer kısmı sonraki frame'de
         *
         * Garanti çözüm: animation içeren widget yok. Sadece text glyph
         * rendering (chunk race'ten en az etkilenen kısım). Sade "Installing..."
         * statik göster — kullanıcı kısa süreyle (1.5 sn) bekler. Apple App
         * Store iPhone'da da minimal animation kullanır. */
        lv_obj_t* inst_lbl = lv_label_create(s_detail_view);
        lv_obj_set_style_text_color(inst_lbl, lv_color_make(0x0A, 0x84, 0xFF), 0);
        lv_obj_set_style_text_font(inst_lbl, ST_D_BTN_F, 0);
        lv_label_set_text(inst_lbl, "Installing...");
        lv_obj_align(inst_lbl, LV_ALIGN_TOP_MID, 0, btn_y);
        lv_obj_clear_flag(inst_lbl, LV_OBJ_FLAG_CLICKABLE);
    } else if (installed) {
        /* "Open" link */
        lv_obj_t* open_lbl = lv_label_create(s_detail_view);
        lv_obj_set_style_text_color(open_lbl, lv_color_make(0x0A, 0x84, 0xFF), 0);
        lv_obj_set_style_text_font(open_lbl, ST_D_BTN_F, 0);
        lv_label_set_text(open_lbl, "Open  " LV_SYMBOL_RIGHT);
        lv_obj_align(open_lbl, LV_ALIGN_TOP_MID, 0, btn_y);
        lv_obj_add_flag(open_lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(open_lbl, 24);
        lv_obj_add_event_cb(open_lbl, detail_open_cb, LV_EVENT_CLICKED, NULL);

        btn_y += ST_D_BTN_STEP;   /* Open height + gap */

        /* "Uninstall" link — sadece downloadable için */
        if (!e->is_system) {
            lv_obj_t* uninst_lbl = lv_label_create(s_detail_view);
            lv_obj_set_style_text_color(uninst_lbl, lv_color_make(0xFF, 0x3B, 0x30), 0);
            lv_obj_set_style_text_font(uninst_lbl, ST_D_BTN2_F, 0);
            lv_label_set_text(uninst_lbl, "Uninstall");
            lv_obj_align(uninst_lbl, LV_ALIGN_TOP_MID, 0, btn_y);
            lv_obj_add_flag(uninst_lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(uninst_lbl, 24);
            lv_obj_add_event_cb(uninst_lbl, detail_uninstall_cb, LV_EVENT_CLICKED, NULL);
        }
    } else {
        /* "Install" link — Faces tab içinden uninstalled app */
        lv_obj_t* inst_lbl = lv_label_create(s_detail_view);
        lv_obj_set_style_text_color(inst_lbl, lv_color_make(0x0A, 0x84, 0xFF), 0);
        lv_obj_set_style_text_font(inst_lbl, ST_D_BTN_F, 0);
        lv_label_set_text(inst_lbl, "Install  " LV_SYMBOL_DOWNLOAD);
        lv_obj_align(inst_lbl, LV_ALIGN_TOP_MID, 0, btn_y);
        lv_obj_add_flag(inst_lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(inst_lbl, 24);
        lv_obj_add_event_cb(inst_lbl, detail_install_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Apps list ve Faces panel'i gizle (detail tam ekran content area kaplar) */
    if (s_apps_list)   lv_obj_add_flag(s_apps_list,   LV_OBJ_FLAG_HIDDEN);
    if (s_faces_panel) lv_obj_add_flag(s_faces_panel, LV_OBJ_FLAG_HIDDEN);
}

static void hide_app_detail(void)
{
    if (s_detail_view) lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    /* Aktif tab'a göre uygun listeyi geri göster */
    if (s_current_tab == STORE_TAB_APPS) {
        if (s_apps_list)   lv_obj_clear_flag(s_apps_list,   LV_OBJ_FLAG_HIDDEN);
        if (s_faces_panel) lv_obj_add_flag(s_faces_panel,   LV_OBJ_FLAG_HIDDEN);
    } else {
        if (s_apps_list)   lv_obj_add_flag(s_apps_list,     LV_OBJ_FLAG_HIDDEN);
        if (s_faces_panel) lv_obj_clear_flag(s_faces_panel, LV_OBJ_FLAG_HIDDEN);
    }
    s_detail_entry_idx = -1;
}

static void app_row_clicked_cb(lv_event_t* e)
{
    lv_obj_t* row = (lv_obj_t*)lv_event_get_current_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    show_app_detail(idx);
}

static void detail_back_cb(lv_event_t* /*e*/)
{
    hide_app_detail();
}

static void detail_open_cb(lv_event_t* /*e*/)
{
    if (s_detail_entry_idx < 0 || s_detail_entry_idx >= (int)store_catalog_count()) return;
    const store_app_entry_t* e = store_catalog_at((size_t)s_detail_entry_idx);
    if (!e) return;
    ESP_LOGI(TAG, "Open app from Store detail: id=%d", (int)e->id);

    /* Text link kullanıldığı için background area split sorunu yok →
     * önceki workaround'lar (button hide + refr_now + 80ms delay) artık
     * gereksiz. Direct no-anim launch yeterli. */
    app_manager_launch_no_anim(e->id);
}

// ─── Install action — mock (NVS flag set) ────────────────────────────────────
//
// Backend henüz hazır değil. "Install" basınca:
//   1. NVS flag set (store_set_installed(id, true))
//   2. Detail view yenilenir (Install link → Open + Uninstall)
//   3. Apps + Faces listeleri rebuild (state değişti)
//   4. Launcher'a refresh event yayınla (yeni tile için)
//
// Backend gelince: 1. adımdan önce HTTP fetch + flash partition write
// eklenecek. Bu adımlar sonra sıralı olur.

/* Timer callback: mock install süresi sonrası NVS yazıp detail'i yenile. */
static void install_complete_cb(lv_timer_t* t)
{
    int idx = (int)(intptr_t)lv_timer_get_user_data(t);
    if (idx >= 0 && idx < (int)store_catalog_count()) {
        const store_app_entry_t* e = store_catalog_at((size_t)idx);
        if (e && !e->is_system) {
            ESP_LOGI(TAG, "Install complete: id=%d (%s)", (int)e->id, e->name);
            store_set_installed(e->id, true);
        }
    }

    /* State NORMAL'a dön, detail re-render → Open + Uninstall görünür */
    s_detail_state = DETAIL_STATE_NORMAL;
    if (s_detail_entry_idx == idx) {
        show_app_detail(idx);
    }
    rebuild_apps_list();
    rebuild_faces_panel();

    lv_timer_delete(t);
    s_install_timer = NULL;
}

static void detail_install_cb(lv_event_t* /*ev*/)
{
    if (s_detail_entry_idx < 0 || s_detail_entry_idx >= (int)store_catalog_count()) return;
    const store_app_entry_t* e = store_catalog_at((size_t)s_detail_entry_idx);
    if (!e || e->is_system) return;

    /* Re-entry guard — zaten install in progress */
    if (s_install_timer || s_detail_state == DETAIL_STATE_INSTALLING) return;

    /* v1.2.6: WiFi check — gerçek mağaza davranışı. Connect değilse modal göster,
     * install başlatma. Backend gelince HTTP fetch zaten WiFi'ye ihtiyaç
     * duyacak; şimdi mock olsa bile UX tutarlılığı için kontrol mantıklı. */
    if (!wifi_service_is_connected()) {
        ESP_LOGW(TAG, "Install blocked: no WiFi (id=%d %s)", (int)e->id, e->name);
        show_no_internet_modal();
        return;
    }

    ESP_LOGI(TAG, "Install start: id=%d (%s) — mock download %d ms",
             (int)e->id, e->name, INSTALL_MOCK_DURATION_MS);

    /* Spinner görünür hale getir — detail'i installing state ile re-render */
    s_detail_state = DETAIL_STATE_INSTALLING;
    show_app_detail(s_detail_entry_idx);

    /* Mock download tamamlanma timer'ı (gerçek HTTP fetch v1.3.0+'da gelecek) */
    s_install_timer = lv_timer_create(install_complete_cb,
                                       INSTALL_MOCK_DURATION_MS,
                                       (void*)(intptr_t)s_detail_entry_idx);
    lv_timer_set_repeat_count(s_install_timer, 1);
}

// ─── Uninstall action — confirmation dialog ile ──────────────────────────────

static void detail_uninstall_cb(lv_event_t* /*ev*/)
{
    if (s_detail_entry_idx < 0) return;
    show_uninstall_confirm(s_detail_entry_idx);
}

/* Confirmation modal state */
static lv_obj_t* s_confirm_overlay = NULL;
static int       s_confirm_idx     = -1;

/* v1.2.6 (2026-05-22): WiFi-yok install attempt için Apple App Store tarzı
 * "No Internet" modal. WiFi connect değilse install butonu basılınca
 * gösterilir, kullanıcı "OK" ile kapatır. Apple Watch'ta benzer modal pattern. */
static lv_obj_t* s_no_internet_modal = NULL;

static void no_internet_ok_cb(lv_event_t* /*e*/)
{
    if (s_no_internet_modal) {
        lv_obj_delete_async(s_no_internet_modal);
        s_no_internet_modal = NULL;
    }
}

static void show_no_internet_modal(void)
{
    if (s_no_internet_modal) return;   /* zaten açık */

    /* Yarı saydam siyah overlay — arka planı dim, tap'leri yutar */
    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 410, 502);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    s_no_internet_modal = overlay;

    /* Card */
    lv_obj_t* card = lv_obj_create(overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 320, 230);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_make(0x1C, 0x1C, 0x1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_clear_flag(card, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                             LV_OBJ_FLAG_SCROLLABLE));

    /* WiFi icon (kırmızı — "bağlantı yok" uyarı tonu) */
    lv_obj_t* icon = lv_label_create(card);
    lv_obj_set_style_text_color(icon, lv_color_make(0xFF, 0x3B, 0x30), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_46, 0);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    /* Title */
    lv_obj_t* title = lv_label_create(card);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_label_set_text(title, "No Internet");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    /* Description */
    lv_obj_t* desc = lv_label_create(card);
    lv_obj_set_style_text_color(desc, lv_color_make(0xC7, 0xC7, 0xCC), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(desc, 4, 0);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, 280);
    lv_label_set_text(desc, "Connect to Wi-Fi to install apps from the Store.");
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_clear_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    /* OK link — text-only (split fix prensibi: background pixel area yok) */
    lv_obj_t* ok_lbl = lv_label_create(card);
    lv_obj_set_style_text_color(ok_lbl, lv_color_make(0x0A, 0x84, 0xFF), 0);
    lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_align(ok_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(ok_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(ok_lbl, 28);
    lv_obj_add_event_cb(ok_lbl, no_internet_ok_cb, LV_EVENT_CLICKED, NULL);
}

static void confirm_cancel_cb(lv_event_t* /*e*/)
{
    if (s_confirm_overlay) {
        lv_obj_delete_async(s_confirm_overlay);
        s_confirm_overlay = NULL;
    }
    s_confirm_idx = -1;
}

static void confirm_delete_cb(lv_event_t* /*ev*/)
{
    if (s_confirm_idx < 0 || s_confirm_idx >= (int)store_catalog_count()) {
        confirm_cancel_cb(NULL);
        return;
    }
    const store_app_entry_t* e = store_catalog_at((size_t)s_confirm_idx);
    if (e && !e->is_system) {
        ESP_LOGI(TAG, "Uninstall confirmed: id=%d (%s)", (int)e->id, e->name);
        store_set_installed(e->id, false);
    }

    int idx = s_confirm_idx;
    confirm_cancel_cb(NULL);   /* overlay kapat */

    /* Detail sayfasını yenile — Open + Uninstall kalkar, Install gelir */
    show_app_detail(idx);

    /* Listeleri refresh et */
    rebuild_apps_list();
    rebuild_faces_panel();
}

/* Apple Watch tarzı confirmation modal — kazara silinmeyi önle */
static void show_uninstall_confirm(int catalog_idx)
{
    if (catalog_idx < 0 || catalog_idx >= (int)store_catalog_count()) return;
    const store_app_entry_t* e = store_catalog_at((size_t)catalog_idx);
    if (!e || e->is_system) return;
    s_confirm_idx = catalog_idx;

    /* Tam ekran modal overlay */
    s_confirm_overlay = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_confirm_overlay);
    lv_obj_set_size(s_confirm_overlay, 410, 502);
    lv_obj_set_pos(s_confirm_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_confirm_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_confirm_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_confirm_overlay, 0, 0);
    lv_obj_clear_flag(s_confirm_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_confirm_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_confirm_overlay);

    /* Başlık */
    char title_buf[80];
    snprintf(title_buf, sizeof(title_buf), "Uninstall %s?", e->name);
    lv_obj_t* title = lv_label_create(s_confirm_overlay);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, 360);
    lv_label_set_text(title, title_buf);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    /* Açıklama */
    lv_obj_t* desc = lv_label_create(s_confirm_overlay);
    lv_obj_set_style_text_color(desc, lv_color_make(0xC7, 0xC7, 0xCC), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(desc, 4, 0);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, 340);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(desc,
        "This will remove the app from\n"
        "your launcher. You can reinstall\n"
        "it later from the Store.");
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_clear_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    /* Cancel butonu (gri, sol) */
    lv_obj_t* cancel = lv_label_create(s_confirm_overlay);
    lv_obj_set_style_text_color(cancel, lv_color_make(0xC7, 0xC7, 0xCC), 0);
    lv_obj_set_style_text_font(cancel, &lv_font_montserrat_24, 0);
    lv_label_set_text(cancel, "Cancel");
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 60, -50);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(cancel, 30);
    lv_obj_add_event_cb(cancel, confirm_cancel_cb, LV_EVENT_CLICKED, NULL);

    /* Delete butonu (kırmızı, sağ) */
    lv_obj_t* del = lv_label_create(s_confirm_overlay);
    lv_obj_set_style_text_color(del, lv_color_make(0xFF, 0x3B, 0x30), 0);
    lv_obj_set_style_text_font(del, &lv_font_montserrat_24, 0);
    lv_label_set_text(del, "Delete");
    lv_obj_align(del, LV_ALIGN_BOTTOM_RIGHT, -60, -50);
    lv_obj_add_flag(del, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(del, 30);
    lv_obj_add_event_cb(del, confirm_delete_cb, LV_EVENT_CLICKED, NULL);
}

static void tab_apps_clicked_cb(lv_event_t* /*e*/)
{
    switch_to_tab(STORE_TAB_APPS);
}

static void tab_faces_clicked_cb(lv_event_t* /*e*/)
{
    switch_to_tab(STORE_TAB_FACES);
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

/* v1.2.7: backend catalog refresh tamamlanınca tetiklenir.
 * Cache güncel → eğer Faces tab aktif ise listeyi yeniden çiz. */
static void on_catalog_refresh_done(esp_err_t err, int count, void* /*ud*/)
{
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Catalog refresh OK — %d remote apps", count);
    } else {
        ESP_LOGW(TAG, "Catalog refresh fail — %s (using cached/fallback)",
                 esp_err_to_name(err));
    }
    /* UI tarafında refresh — lv_async_call ile güvenli thread switch (HTTP
     * task'tan LVGL'e dokunmayalım).
     *
     * v1.4.61: Tab check kaldırıldı. Kullanıcı Apps tab'inde olsa bile Faces
     * panel'i (hidden objeler) güncellensin; sonra Faces'e geçince hazır olur.
     * switch_to_tab() da artık geçişte rebuild ediyor — bu iki yol birbirini
     * tamamlar (kullanıcı Faces'teyken refresh tamamlanırsa anında görür). */
    lv_async_call([](void* /*arg*/) {
        if (s_faces_panel) {
            rebuild_faces_panel();
        }
    }, NULL);
}

static void on_start(lv_obj_t* screen)
{
    ESP_LOGI(TAG, "Store starting");
    s_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    s_current_tab = STORE_TAB_APPS;
    build_header(screen);
    build_tab_bar(screen);
    build_content(screen);
    apply_tab_visual();

    app_manager_enable_gestures(screen);

    /* v1.2.7: Backend catalog'u arka planda çek. İlk açılışta cache boş,
     * Faces tab compile-time fallback gösterir; refresh tamamlanınca async
     * callback ile remote listesi yerleşir. WiFi yoksa sessizce skip. */
    store_service_init();
    store_service_refresh_async(on_catalog_refresh_done, NULL);
}

static void on_resume(void)
{
    /* v1.2.7: Store her açılışında catalog refresh (Apple App Store pattern).
     * WiFi yok / cache fresh / in-flight ise sessizce skip eder. */
    store_service_refresh_async(on_catalog_refresh_done, NULL);
}

static void on_pause(void) {}

static void on_stop(void)
{
    ESP_LOGI(TAG, "Store stopping");
    /* Pending install timer varsa iptal et — screen siliniyor */
    if (s_install_timer) {
        lv_timer_delete(s_install_timer);
        s_install_timer = NULL;
    }
    s_screen        = NULL;
    s_header_lbl    = NULL;
    s_tab_apps      = NULL;
    s_tab_faces     = NULL;
    s_content       = NULL;
    s_apps_list     = NULL;
    s_faces_panel   = NULL;
    s_detail_view   = NULL;
    s_detail_entry_idx = -1;
    s_current_tab   = STORE_TAB_APPS;
    s_confirm_overlay = NULL;
    s_confirm_idx   = -1;
    s_no_internet_modal = NULL;
    s_update_required_modal = NULL;   /* v1.2.7 */
    s_detail_state  = DETAIL_STATE_NORMAL;
}

static bool on_event(const os_event_t* /*ev*/) { return false; }

static const app_interface_t s_iface = {
    .on_start  = on_start,
    .on_resume = on_resume,
    .on_pause  = on_pause,
    .on_stop   = on_stop,
    .on_event  = on_event,
    .name      = "Store",
};

const app_interface_t* store_app_get_interface(void) { return &s_iface; }
