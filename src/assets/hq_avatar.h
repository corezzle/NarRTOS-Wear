#pragma once
#include "lvgl.h"
/* HQ Mesaj ekranı dairesel avatarı — Genel Müdür fotoğrafı (v1.2.9).
 * Board başına tek boyut derlenir (flash tasarrufu):
 * 88 px → LCD128 (240), 196 px → AMOLED 2.06" (466) + 1.8" (368/448). */
#ifdef BOARD_VARIANT_LCD128
extern const lv_image_dsc_t hq_avatar_88;
#else
extern const lv_image_dsc_t hq_avatar_196;
#endif
