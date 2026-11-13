/**
 * @file telemetry_service.h
 * @brief Periyodik telemetri raporu (backend /api/wear-os/telemetry).
 * Periyot NVS'de kalıcı; `set telemetry period <sn>` komutuyla değişir.
 * Konum: GPS fix varsa payload'a "gps" eklenir, WiFi tarama listesi her zaman gider.
 */
#pragma once
#include <esp_err.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t telemetry_service_init(void);
/* Rapor periyodu (saniye). 10..3600 aralığına sıkıştırılır, NVS'e yazılır. */
esp_err_t telemetry_service_set_period(uint32_t seconds);
/* Sunucu adresi (http://host:port) — NVS'de kalıcı; varsayılan OTA_BACKEND_HOST */
esp_err_t   telemetry_service_set_backend(const char* base_url);
const char* telemetry_service_get_backend(void);
uint32_t  telemetry_service_get_period(void);
/* Periyodu beklemeden hemen bir rapor gönder (komut/test). WiFi yoksa ESP_ERR_INVALID_STATE. */
esp_err_t telemetry_service_send_now(void);
/* Tracker modu — IMU hareketine göre otomatik:
 *   ACTIVE: hareket var → GPS açık, period_active (vars. 30 s)
 *   REST  : rest_after s hareketsiz → GPS kapalı, period_rest (vars. 600 s)
 * Komutla zorlanabilir (auto/active/rest). Ayarlar NVS'de kalıcı. */
typedef enum { TRK_MODE_REST = 0, TRK_MODE_ACTIVE = 1 } tracker_mode_t;
esp_err_t telemetry_service_set_rest_period(uint32_t seconds);
esp_err_t telemetry_service_set_rest_after(uint32_t seconds);
uint32_t  telemetry_service_get_rest_period(void);
uint32_t  telemetry_service_get_rest_after(void);
tracker_mode_t telemetry_service_get_mode(void);
/* force: -1 = auto, 0 = rest, 1 = active */
void      telemetry_service_force_mode(int force);
int       telemetry_service_get_force(void);
/* Son gönderim bilgisi (komut 'get telemetry' için) */
uint32_t  telemetry_service_sent_count(void);
int       telemetry_service_last_http(void);

#ifdef __cplusplus
}
#endif
