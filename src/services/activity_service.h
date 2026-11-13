/**
 * @file activity_service.h
 * @brief IMU adım sayısından hareket durumu: dinleniyor / yürüyor / koşuyor.
 *
 * Karttan bağımsız: tek girdi sensor_service_get_steps() (pedometre).
 * Çıktı telemetriye ("activity") ve tracker moduna (aktif/dinlenme) gider.
 * `activity sim rest|walk|run` ile IMU olmadan test edilir.
 */
#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ACT_REST = 0, ACT_WALK = 1, ACT_RUN = 2 } activity_state_t;

typedef struct {
    activity_state_t state;
    uint32_t steps_total;     /* pedometre toplamı */
    uint16_t steps_per_min;   /* son 60 sn */
    uint8_t  intensity;       /* 0..100 (tempoya göre) */
    uint32_t since_motion_s;  /* son hareketten beri geçen süre */
    uint16_t taps_per_min;    /* piezo darbeleri (sensör varsa), yoksa 0 */
    bool     simulated;
} activity_t;

esp_err_t   activity_service_init(void);
void        activity_service_get(activity_t* out);
const char* activity_state_name(activity_state_t s);
/* Eşikler (adım/dk): yürüyüş başlangıcı, koşu başlangıcı. Varsayılan 5 / 60. */
void        activity_service_set_thresholds(uint16_t walk_spm, uint16_t run_spm);
void        activity_service_simulate(activity_state_t s);
void        activity_service_simulate_off(void);

#ifdef __cplusplus
}
#endif
