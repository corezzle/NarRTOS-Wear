/**
 * @file gps_service.h
 * @brief Harici GPS/GNSS modülü (UART, NMEA-0183) — konum fix'i sağlar.
 *
 * Pinler/baud derleme bayrağıyla: GPS_UART_RX (varsayılan 17), GPS_UART_TX (18),
 * GPS_UART_BAUD (9600). Fix yoksa (kapalı alan) telemetri WiFi konumuna düşer.
 * `gps sim <lat> <lng>` komutuyla donanım olmadan test edilebilir.
 */
#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     valid;      /* geçerli fix (RMC 'A' / GGA quality>0) ve taze */
    double   lat, lng;
    float    hdop;       /* yatay seyreltme; ~m hata ≈ hdop × 5 */
    uint8_t  sats;
    float    speed_kmh;
    uint32_t age_s;      /* son fix'ten bu yana geçen süre */
    bool     simulated;
} gps_fix_t;

esp_err_t gps_service_init(void);
/* Güç tasarrufu: dinlenme modunda kapat (GPS_EN_PIN varsa pin sürülür, yoksa
 * okuma durdurulur). Açınca fix yeniden aranır. */
void      gps_service_set_enabled(bool on);
bool      gps_service_is_enabled(void);
/* Taze ve geçerli fix varsa true döner ve out'u doldurur. */
bool      gps_service_get_fix(gps_fix_t* out);
/* Modülden en az bir NMEA cümlesi geldi mi (donanım var mı)? */
bool      gps_service_is_present(void);
uint32_t  gps_service_sentences(void);
/* Test: sahte fix ver (lat/lng/hdop); lat=NAN ile kapat. */
void      gps_service_simulate(double lat, double lng, float hdop);
void      gps_service_simulate_off(void);

#ifdef __cplusplus
}
#endif
