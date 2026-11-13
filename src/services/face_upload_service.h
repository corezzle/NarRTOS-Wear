#pragma once
/**
 * face_upload_service — WiFi AP + HTTP server for custom watch face image upload.
 *
 * Flow:
 *   1. face_upload_load_persisted() at boot → restores any previously uploaded faces
 *      from SPIFFS ("assets" partition, /faces/face_N.bin) into PSRAM.
 *   2. face_upload_service_start() → ESP32 becomes "EmbedWatch" WiFi AP (192.168.4.1)
 *   3. User connects phone/PC to "EmbedWatch", opens browser at 192.168.4.1
 *   4. Browser shows upload page; JS converts any image to 410×502 RGB565 binary
 *   5. Upload hits POST /upload?slot=0 or /upload?slot=1
 *      → stored in PSRAM AND persisted to SPIFFS immediately
 *   6. on_done callback fires (from HTTP task — use lv_async_call for LVGL access)
 *   7. face_upload_service_stop() tears down AP + HTTP server
 *
 * Persistence: images survive reboots via the "assets" SPIFFS partition (1.9 MB).
 */

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#define FACE_UPLOAD_MAX_SLOTS 12
#define FACE_UPLOAD_W         410
#define FACE_UPLOAD_H         502
#define FACE_UPLOAD_BUF_BYTES ((uint32_t)(FACE_UPLOAD_W) * (FACE_UPLOAD_H) * 2u)

typedef struct {
    void*    buf;    /* PSRAM: FACE_UPLOAD_BUF_BYTES of RGB565 (LV_COLOR_16_SWAP byte order) */
    bool     valid;
} face_upload_slot_t;

/**
 * Called from the HTTP server FreeRTOS task after a successful upload.
 * Use lv_async_call() inside to touch LVGL objects safely.
 */
typedef void (*face_upload_done_cb_t)(int slot);

/**
 * Call once at boot (before UI starts) to restore previously uploaded faces
 * from SPIFFS into PSRAM. Safe to call even on a fresh/empty partition.
 */
esp_err_t face_upload_load_persisted(void);

esp_err_t face_upload_service_start(face_upload_done_cb_t on_done);
void      face_upload_service_stop(void);
bool      face_upload_service_is_running(void);

const face_upload_slot_t* face_upload_get_slot(int slot);
int                        face_upload_slot_count(void);
esp_err_t                  face_upload_delete_slot(int slot);
