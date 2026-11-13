#pragma once
/**
 * @file audio_service.h
 * @brief Yüksek seviye ses servisi — alarm tonu loop'unu yönetir.
 *
 * Sorumluluk:
 *   • buzzer_driver üzerinden alarm pattern'ini loop eder.
 *   • Pattern step'leri esp_timer (one-shot) ile programlanır → UI
 *     thread'i bloklamaz; LVGL render'a paralel çalar.
 *   • İdempotent start/stop API'si.
 *
 * Şu an tek pattern (alarm_pattern) tanımlı. Gelecekte birden fazla
 * notifikasyon için "pattern enum'u" parametre olarak eklenir.
 *
 * Thread-safety:
 *   start/stop UI thread'inden çağrılır. Pattern step callback'i
 *   esp_timer task'ında çalışır → s_playing flag volatile + s_step_idx
 *   sadece pattern callback içinde değiştirilir.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Buzzer driver'ı init eder + state machine için esp_timer hazırlar.
 *  Boot sırasında bir kez çağrılmalı. */
void audio_service_init(void);

/** Alarm tone pattern'ini sonsuz döngüde başlatır.
 *  audio_service_stop() çağrılana kadar çalar.
 *  İdempotent: zaten çalıyorsa no-op. */
void audio_service_play_alarm(void);

/** Şu an çalan pattern'i durdurur (buzzer susar).
 *  İdempotent: çalmıyorsa no-op. */
void audio_service_stop(void);

/** Pattern aktif mi? (UI'da "muted" göstergeleri için.) */
bool audio_service_is_playing(void);

#ifdef __cplusplus
}
#endif
