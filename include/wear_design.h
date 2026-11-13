/**
 * @file wear_design.h
 * @brief Apple Watch tarzı merkezi design tokens (color/spacing/radius/font).
 *
 * Apple HIG'in "iOS dark + watchOS palette" uyarlaması. Tüm app'lerin
 * tutarlı görsel dile sahip olmasını sağlar — şu an her app kendi
 * hex'lerini ve padding'lerini kullanıyor → mikro tutarsızlıklar göze
 * çarpıyor (corner radius 12 vs 16, padding 14 vs 18 vs 20). Yeni kod
 * bu makroları kullanırsa OS genelinde tutarlılık otomatik gelir.
 *
 * Geriye dönük uyumluluk: var olan kod hex/sayı kullanmaya devam edebilir,
 * bu header sadece yeni iş için referans + gradual refactor base'i.
 *
 * Naming: WD_<category>_<variant>
 *   WD_BG_*       → background tonları (siyah → açık gri elevation)
 *   WD_TEXT_*     → metin renk hiyerarşisi (Apple "Label" sistemi)
 *   WD_ACCENT_*   → vurgu renkleri (action button, status, semantik)
 *   WD_PAD_*      → padding/spacing (4-pt grid)
 *   WD_RAD_*      → corner radius
 *   WD_FONT_*     → font referansı (LVGL pointer)
 */

#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Background hierarchy (Apple "Materials") ────────────────────────────── */
#define WD_BG_BASE       lv_color_black()                          /* ana ekran */
#define WD_BG_ELEVATED   lv_color_make(0x1C, 0x1C, 0x1E)           /* row, card */
#define WD_BG_CARD       lv_color_make(0x18, 0x18, 0x1E)           /* gallery thumb */
#define WD_BG_PRESSED    lv_color_make(0x2C, 0x2C, 0x2E)           /* press feedback */
#define WD_BG_OVERLAY    lv_color_make(0x10, 0x10, 0x12)           /* modal scrim */

/* ─── Text hierarchy (Apple "Label" levels) ───────────────────────────────── */
#define WD_TEXT_PRIMARY    lv_color_white()                        /* başlık, ana metin */
#define WD_TEXT_SECONDARY  lv_color_make(0xCC, 0xCC, 0xD0)         /* alt başlık */
#define WD_TEXT_TERTIARY   lv_color_make(0x8E, 0x8E, 0x93)         /* ikincil bilgi */
#define WD_TEXT_QUATERNARY lv_color_make(0x55, 0x55, 0x5C)         /* placeholder, hint */

/* ─── Accent colors (semantic) ────────────────────────────────────────────── */
#define WD_ACCENT_BLUE    lv_color_make(0x0A, 0x84, 0xFF)          /* primary action */
#define WD_ACCENT_GREEN   lv_color_make(0x30, 0xD1, 0x58)          /* success, on, healthy */
#define WD_ACCENT_ORANGE  lv_color_make(0xFF, 0x9F, 0x0A)          /* alarm, warning */
#define WD_ACCENT_RED     lv_color_make(0xFF, 0x45, 0x3A)          /* destructive, error */
#define WD_ACCENT_YELLOW  lv_color_make(0xFF, 0xD6, 0x0A)          /* attention */
#define WD_ACCENT_PURPLE  lv_color_make(0xBF, 0x5A, 0xF2)          /* settings sembolü */
#define WD_ACCENT_CYAN    lv_color_make(0x00, 0xC7, 0xBE)          /* aktivite, fitness */

/* ─── Spacing (4-pt grid) ─────────────────────────────────────────────────── */
#define WD_PAD_XS   4
#define WD_PAD_S    8
#define WD_PAD_M    12
#define WD_PAD_L    16
#define WD_PAD_XL   20
#define WD_PAD_2XL  24
#define WD_PAD_3XL  32

/* ─── Corner radius ───────────────────────────────────────────────────────── */
#define WD_RAD_S      8
#define WD_RAD_M      12
#define WD_RAD_L      16
#define WD_RAD_XL     20
#define WD_RAD_2XL    24
#define WD_RAD_PILL   28        /* toggle, pill-button */
#define WD_RAD_CIRCLE LV_RADIUS_CIRCLE

/* ─── Standard component sizes ────────────────────────────────────────────── */
#define WD_ROW_H        90       /* settings row, list item */
#define WD_ICON_D       40       /* nav row icon container */
#define WD_BTN_PILL_H   56       /* primary action button */
#define WD_TOGGLE_W     64
#define WD_TOGGLE_H     34
#define WD_TOGGLE_KNOB  30

/* ─── Border widths ───────────────────────────────────────────────────────── */
#define WD_BORDER_HAIR  1        /* row separator, subtle outline */

/* ─── Safe area (round display) ───────────────────────────────────────────── */
#define WD_SAFE_X       12       /* sol/sağ kenar margin */
#define WD_SAFE_W       386      /* HOR_RES - 2*SAFE_X */
#define WD_HEADER_H     86       /* status bar + back button alanı */

#ifdef __cplusplus
}
#endif
