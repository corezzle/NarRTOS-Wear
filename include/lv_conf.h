/**
 * lv_conf.h - LVGL 9.x configuration
 * Optimized for ESP32-S3 + PSRAM + CO5300 AMOLED
 */

#if 1  /* Set to "1" to enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Color depth: 16 (RGB565) matches CO5300 native format */
#define LV_COLOR_DEPTH 16

/* Swap bytes for SPI displays (big-endian on wire) */
#define LV_COLOR_16_SWAP 1

/* LVGL 9.x: builtin TLSF allocator with a 1 MB pool backed by PSRAM.
 * `LV_MEM_CUSTOM` is an LVGL 8.x macro — ignored in 9.x. Default 64 KB
 * internal-SRAM pool fragmente olunca lv_draw_sw_img.c'deki per-frame
 * blend buffer mallocu ve lv_draw_buf_create_ex layer buffer'ları OOM
 * verip render task'ını donduruyordu. PSRAM (8 MB) üzerinde 1 MB'lik
 * havuz ile her ikisi de kök çözüldü. Internal SRAM, DMA framebuffer
 * (ui_service.cpp) ve FreeRTOS stack'leri için saklanır. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_MEM_SIZE             (1024U * 1024U)
#define LV_MEM_POOL_INCLUDE     <esp_heap_caps.h>
#define LV_MEM_POOL_ALLOC(size) heap_caps_malloc((size), MALLOC_CAP_SPIRAM)

/* Tick: use our os_tick_get_ms() */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "core/os_tick.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (os_tick_get_ms())

/* Display resolution (set at runtime via lv_display_set_resolution, but defaults here) */
#define LV_HOR_RES_MAX 410
#define LV_VER_RES_MAX 502

/* DPI: ~320 DPI for 2.06" AMOLED */
#define LV_DPI_DEF 320

/* Refresh + input rates — Apple Watch tarzı pürüzsüzlük için 60 FPS render
 * + 100 Hz touch sampling. Default değerler (33 ms / 30 ms) saatte gözle
 * görünür "31 FPS" hissi yaratıyordu (selector slide ve swipe stutter
 * raporları). Değerler ile birlikte src/main.cpp:task_ui adaptive delay
 * 5..16 ms aralığına çekildi (eski 5..30) — touch input lag'ı 30 ms → 16 ms.
 * Render bütçesi: 60 FPS = 16.6 ms/frame; ESP32-S3 + LVGL software renderer
 * 410×502 RGB565'i ~12 ms'de bitiriyor → marjın yeterli. */
#define LV_DEF_REFR_PERIOD       16
#define LV_INDEV_DEF_READ_PERIOD 10
#define LV_USE_FONT_COMPRESSED 1
/* Fonts */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_38 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_42 1
#define LV_FONT_MONTSERRAT_44 1
#define LV_FONT_MONTSERRAT_46 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Animations */
#define LV_USE_ANIM 1

/* Widgets needed */
#define LV_USE_LABEL  1
#define LV_USE_BTN    1
#define LV_USE_BAR    1
#define LV_USE_ARC    1
#define LV_USE_IMG    1
#define LV_USE_LINE   1
#define LV_USE_CHART  0  /* Not needed initially */
#define LV_USE_TABLE  0
#define LV_USE_LIST     1
#define LV_USE_ROLLER   1
#define LV_USE_SWITCH   1
#define LV_USE_SLIDER   1
#define LV_USE_SPINNER  1   /* Boot loading spinner için */
#define LV_USE_TEXTAREA 1
#define LV_USE_BTNMATRIX 1
#define LV_USE_KEYBOARD 1
#define LV_USE_SNAPSHOT 1
#define LV_USE_CANVAS   1

/* Performance: disable unused extras */
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#define LV_USE_REFR_DEBUG   0


/* GPU: ESP32-S3 has no dedicated GPU, use software renderer */
#define LV_USE_GPU_ESP32S3_ROTATE 0
#define LV_DRAW_SW_COMPLEX        1

/* File system (for loading fonts/images from SPIFFS) */
#define LV_USE_FS_POSIX 1
#define LV_FS_POSIX_LETTER 'S'   /* S:/fonts/my_font.bin */

/* Log */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* Assert */
#define LV_USE_ASSERT_NULL    1
#define LV_USE_ASSERT_MEM     1
#define LV_USE_ASSERT_OBJ     0  /* Disable in production */
#define LV_USE_ASSERT_STYLE   0

#endif /* LV_CONF_H */
#endif /* Enable content */
