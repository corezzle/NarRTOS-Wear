#ifdef TRACKER_FEATURES
#include "activity_service.h"
#include "sensor_service.h"
#include "vibration_service.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "ActivitySvc";

#define WINDOW_S 60                       /* tempo penceresi */
static uint32_t s_ring[WINDOW_S];         /* saniyelik adım toplamları */
static uint8_t  s_ring_i;
static uint32_t s_last_total;
static activity_t s_cur;
static int64_t  s_last_motion_us;
static uint16_t s_walk_spm = 5, s_run_spm = 60;
static bool s_sim_on; static activity_state_t s_sim_state;
static bool s_pedometer_seen;             /* pedometre hiç sayı verdi mi */

const char* activity_state_name(activity_state_t s)
{ return s == ACT_RUN ? "run" : s == ACT_WALK ? "walk" : "rest"; }

static void tick(void)
{
    uint32_t total = sensor_service_get_steps();
    if (total && !s_pedometer_seen) s_pedometer_seen = true;
    uint32_t delta = total >= s_last_total ? total - s_last_total : 0;
    s_last_total = total;
    s_ring[s_ring_i] = delta; s_ring_i = (s_ring_i + 1) % WINDOW_S;
    uint32_t spm = 0; for (int i = 0; i < WINDOW_S; i++) spm += s_ring[i];

    /* Piezo: kedi adım saydırmayabilir; dakikada >=VIB_WALK_TPM darbe varsa en az
     * 'yürüyor' say. Darbe geldikçe hareketsizlik sayacı sıfırlanır. */
    #define VIB_WALK_TPM 8
    uint16_t tpm = vibration_service_taps_per_min();
    activity_state_t st = spm >= s_run_spm ? ACT_RUN : spm >= s_walk_spm ? ACT_WALK : ACT_REST;
    if (st == ACT_REST && tpm >= VIB_WALK_TPM) st = ACT_WALK;
    if (s_sim_on) st = s_sim_state;
    if (st != ACT_REST || delta || vibration_service_since_last_s() < 2) s_last_motion_us = esp_timer_get_time();

    if (st != s_cur.state) ESP_LOGI(TAG, "state %s -> %s (spm=%u)", activity_state_name(s_cur.state), activity_state_name(st), (unsigned)spm);
    s_cur.state = st;
    s_cur.steps_total = total;
    s_cur.steps_per_min = (uint16_t)(spm > 65535 ? 65535 : spm);
    /* yoğunluk: 0 adım → 0, koşu eşiği×2 → 100 */
    uint32_t inten = (spm * 100) / (s_run_spm * 2); if (inten > 100) inten = 100;
    if (s_sim_on) inten = st == ACT_RUN ? 85 : st == ACT_WALK ? 35 : 0;
    s_cur.intensity = (uint8_t)inten;
    s_cur.since_motion_s = (uint32_t)((esp_timer_get_time() - s_last_motion_us) / 1000000);
    s_cur.simulated = s_sim_on;
    s_cur.taps_per_min = tpm;
}

static void activity_task(void* arg)
{ (void)arg; for (;;) { tick(); vTaskDelay(pdMS_TO_TICKS(1000)); } }

esp_err_t activity_service_init(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_last_total = sensor_service_get_steps();
    s_last_motion_us = esp_timer_get_time();
    if (xTaskCreatePinnedToCore(activity_task, "activity", 2560, NULL, 2, NULL, 1) != pdPASS) return ESP_FAIL;
    ESP_LOGI(TAG, "init OK (walk>=%u spm, run>=%u spm)", s_walk_spm, s_run_spm);
    return ESP_OK;
}

void activity_service_get(activity_t* out) { if (out) *out = s_cur; }
void activity_service_set_thresholds(uint16_t w, uint16_t r) { if (w) s_walk_spm = w; if (r > w) s_run_spm = r; }
void activity_service_simulate(activity_state_t s) { s_sim_on = true; s_sim_state = s; ESP_LOGW(TAG, "SIM %s", activity_state_name(s)); }
void activity_service_simulate_off(void) { s_sim_on = false; ESP_LOGW(TAG, "sim off"); }
#endif /* TRACKER_FEATURES */
