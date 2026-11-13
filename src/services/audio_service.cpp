/**
 * @file audio_service.cpp
 * @brief Alarm tonu pattern engine.
 *
 * Pattern tasarımı (alarm_pattern):
 *   iPhone/Apple Watch alarm benzeri "rising arpeggio + duraklama" döngüsü.
 *   • 880 Hz (A5) — 220 ms ON
 *   • silence    —  80 ms
 *   • 988 Hz (B5) — 220 ms ON
 *   • silence    —  80 ms
 *   • 1175 Hz(D6) — 260 ms ON  ← arpeggio tepe noktası, dikkat çekici
 *   • silence    — 700 ms      ← uzun mola; insan ritm hissi için kritik
 *   Toplam döngü: ~1.56 s. Sonsuz repeat.
 *
 * Engine:
 *   • esp_timer ONE_SHOT mode: her step'te bir sonraki step için yeni
 *     interval programlanır. PERIODIC kullanmadık çünkü step süreleri
 *     farklı (220, 80, 220, 80, 260, 700 ms).
 *   • s_step_idx: pattern array'inde sıradaki adımın indeksi.
 *   • s_playing: stop çağrılınca false → pattern callback'i son round'unu
 *     bitirip kendini yeniden programlamaz.
 *
 * Thread-safety:
 *   • esp_timer callback'i kendi task'ında (esp_timer task) çalışır,
 *     pattern array'i ve s_step_idx yalnızca buradan değiştirilir.
 *   • audio_service_play_alarm UI thread'inden çağrılır → s_playing'i
 *     true yapar ve timer'ı 1 ms gecikmeyle programlar; gerçek pattern
 *     execution esp_timer task'ında başlar.
 *   • audio_service_stop UI thread'inden çağrılır → timer'ı durdurur,
 *     buzzer_stop() çağırır, s_playing = false.
 */

#include "audio_service.h"
#include "../drivers/audio/buzzer_driver.h"
#include "../core/os_config.h"

#include <esp_timer.h>
#include <esp_log.h>
#include <stdint.h>
#include <stddef.h>

static const char* TAG = "AudioSvc";

typedef struct {
    uint16_t freq_hz;   /* 0 = silence */
    uint16_t dur_ms;
    uint8_t  duty_pct;  /* 0..100, 0 ise sessiz; freq_hz=0 ise zaten sessiz */
} step_t;

/* iPhone/Apple Watch tarzı alarm arpeggio.
 * Frekanslar A5/B5/D6 — uyandırıcı ama keskin değil.
 * Duty %50 = simetrik square wave, en yüksek harmonik içerik. */
static const step_t alarm_pattern[] = {
    { 880, 220, 50 },   /* A5  */
    {   0,  80,  0 },   /* gap */
    { 988, 220, 50 },   /* B5  */
    {   0,  80,  0 },   /* gap */
    {1175, 260, 50 },   /* D6 — arpeggio tepe */
    {   0, 700,  0 },   /* uzun gap → tekrar başlamadan önce ritm boşluğu */
};
#define ALARM_PATTERN_LEN  (sizeof(alarm_pattern) / sizeof(alarm_pattern[0]))

static esp_timer_handle_t s_timer    = NULL;
static volatile bool      s_playing  = false;
static volatile bool      s_inited   = false;
static int                s_step_idx = 0;

/* Forward decl */
static void schedule_next_step(uint64_t delay_us);

static void apply_step(int idx)
{
    const step_t* st = &alarm_pattern[idx];
    if (st->freq_hz == 0 || st->duty_pct == 0) {
        buzzer_stop();
    } else {
        buzzer_tone(st->freq_hz, st->duty_pct);
    }
}

static void pattern_cb(void* arg)
{
    (void)arg;

    if (!s_playing) {
        /* Stop yarış: pattern_cb'nin esp_timer task'ında çalışması ile
         * audio_service_stop'un UI task'ında çağrılması arasında ufak bir
         * yarış olabilir. Defensive guard: playing değilse buzzer'ı kes
         * ve döngüden çık. */
        buzzer_stop();
        return;
    }

    /* Bu callback ÇAĞRILMADAN ÖNCE programlanan step'i uygula:
     * audio_service_play_alarm 1 ms ile timer'ı arm ediyor; ilk callback'te
     * step 0 başlar. Sonraki callback'te step ilerler. */
    apply_step(s_step_idx);

    /* Bir sonraki step'in süresine göre kendini yeniden programla. */
    uint64_t dur_us = (uint64_t)alarm_pattern[s_step_idx].dur_ms * 1000ULL;

    /* İlerle (sonsuz loop). */
    s_step_idx = (s_step_idx + 1) % (int)ALARM_PATTERN_LEN;

    schedule_next_step(dur_us);
}

static void schedule_next_step(uint64_t delay_us)
{
    if (!s_timer || !s_playing) return;
    /* esp_timer_start_once: zaten armed ise hata döner, ama bu callback'in
     * tek tetikleyicisi pattern_cb'nin sonu olduğu için yarış yok. */
    esp_err_t err = esp_timer_start_once(s_timer, delay_us);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_timer_start_once failed: 0x%x", err);
    }
}

void audio_service_init(void)
{
    if (s_inited) return;

    esp_err_t err = buzzer_init((gpio_num_t)PIN_BUZZER);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "buzzer_init failed (0x%x) — audio disabled", err);
        return;   /* s_inited=false, play_alarm no-op olacak */
    }

    esp_timer_create_args_t targs = {};
    targs.callback        = pattern_cb;
    targs.arg             = NULL;
    targs.dispatch_method = ESP_TIMER_TASK;
    targs.name            = "alarm_pat";

    err = esp_timer_create(&targs, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: 0x%x", err);
        s_timer = NULL;
        return;
    }

    s_inited = true;
    ESP_LOGI(TAG, "audio service ready (buzzer GPIO %d)", PIN_BUZZER);
}

void audio_service_play_alarm(void)
{
    if (!s_inited) {
        ESP_LOGW(TAG, "play requested but service not inited");
        return;
    }
    if (s_playing) return;   /* idempotent */

    s_step_idx = 0;
    s_playing  = true;
    /* 1 ms sonra ilk step → audio_service_play_alarm hemen geri döner,
     * gerçek tone esp_timer task'ında başlar (UI thread'i bloklamaz). */
    schedule_next_step(1000);
    ESP_LOGI(TAG, "alarm pattern started");
}

void audio_service_stop(void)
{
    if (!s_inited) return;
    if (!s_playing) {
        /* zaten kapalı — buzzer'ı yine de defensive olarak kes.
         * (örn: birisi doğrudan buzzer_tone çağırdıysa). */
        buzzer_stop();
        return;
    }

    s_playing = false;
    /* Timer pending bir start_once'i varsa iptal et. esp_timer_stop sıfır
     * kez tetiklenmiş timer'lar için ESP_ERR_INVALID_STATE döner — bu
     * normal, log etmiyoruz. */
    if (s_timer) {
        esp_timer_stop(s_timer);
    }
    buzzer_stop();
    ESP_LOGI(TAG, "alarm pattern stopped");
}

bool audio_service_is_playing(void)
{
    return s_playing;
}
