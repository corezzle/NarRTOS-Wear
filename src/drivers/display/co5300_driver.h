#pragma once
/**
 * @brief CO5300 AMOLED Display Driver
 *
 * Interface: QSPI (4-bit SPI), 410x502 resolution
 * Connected via ESP32-S3 SPI2_HOST with DMA
 *
 * Strategy:
 *  - esp_lcd panel API → portable, DMA-backed
 *  - Partial refresh via TE (Tearing Effect) pin to prevent tearing
 *  - Two PSRAM frame buffers for double-buffering (swap on flush complete)
 *  - LVGL flush callback signals DMA complete via semaphore (zero-copy)
 */

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

// ─── Display Geometry ────────────────────────────────────────────────────────
// v1.2.9: #ifndef guards — alternate display variant (sh8601_18 driver) build
// flag ile resolution override edebilsin. Default davranış değişmiyor: CO5300_*
// build flag'siz derlenince 410×502 gelir, S3 board için byte-bit aynı.
#ifndef CO5300_H_RES
#define CO5300_H_RES    410
#endif
#ifndef CO5300_V_RES
#define CO5300_V_RES    502
#endif
#define CO5300_COLOR_SIZE 2   // RGB565 = 2 bytes/pixel

// ─── CO5300 Register Definitions ─────────────────────────────────────────────
#define CO5300_CMD_SWRESET  0x01
#define CO5300_CMD_SLPIN    0x10
#define CO5300_CMD_SLPOUT   0x11
#define CO5300_CMD_NORON    0x13
#define CO5300_CMD_INVON    0x21   // AMOLED: inversion ON for correct colors
#define CO5300_CMD_DISPOFF  0x28
#define CO5300_CMD_DISPON   0x29
#define CO5300_CMD_CASET    0x2A
#define CO5300_CMD_RASET    0x2B
#define CO5300_CMD_RAMWR    0x2C
#define CO5300_CMD_MADCTL   0x36
#define CO5300_CMD_COLMOD   0x3A   // Color format: 0x55 = RGB565
#define CO5300_CMD_WRCTRLD  0x53   // Brightness control

// ─── Public API ───────────────────────────────────────────────────────────────

/**
 * Initialize CO5300: SPI bus, reset sequence, init commands, DMA buffers.
 * Must be called before LVGL init.
 */
esp_err_t co5300_init(void);

/**
 * co5300_init tamamlandı mı (QSPI hattı kuruldu, panel komutları gönderildi)?
 * Ekransız tasma derlemesinde ekran veri pinini (D2/GPIO6) başka bir girişe
 * (piezo) devretmeden önce bunun true olması beklenir; aksi hâlde ekran init'i
 * pini geri alır. Her ekran varyantı aynı sembolü verir.
 */
bool co5300_is_ready(void);

/**
 * LVGL flush callback.
 * Called by LVGL when a region is ready to be sent to the display.
 * Uses DMA; signals lv_display_flush_ready() on DMA-complete ISR.
 */
void co5300_lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);

/**
 * Set display brightness (0–255).
 */
esp_err_t co5300_set_brightness(uint8_t brightness);

/**
 * Enter/exit display sleep mode (SLPIN/SLPOUT).
 */
esp_err_t co5300_sleep(bool enable);

/**
 * Açılışta yapılan panel kimlik okumasının sonucu (Session 46e [panel-id]).
 * true = panel QSPI'de cevap verdi; false = hepsi FF/00 (cevap yok) ya da
 * henüz okunmadı. Checklist "Ekran" satırı bunu kullanır.
 */
bool co5300_panel_responded(void);
