#pragma once
/**
 * @file buzzer_driver.h
 * @brief LEDC PWM tabanlı square-wave tone driver (ESP32-S3'te DAC yok).
 *
 * Mimari:
 *   • ESP32-S3 LEDC modülü → bir GPIO'ya programlanabilir frekansta /
 *     duty cycle'da PWM üretir.
 *   • duty=50% (4096/8192) ile tam square wave → maksimum işitilebilir ses.
 *   • duty=0 ile susturma (pin LOW kalır).
 *   • Ton frekansı runtime'da `buzzer_tone(freq_hz, duty_pct)` ile değişir;
 *     LEDC timer'ın frekansı atomik olmayan bir şekilde güncellenir, fakat
 *     bu uygulama için (alarm pattern step'leri 80-220 ms) sorun değil.
 *
 * Kullanım:
 *   buzzer_init(GPIO_NUM_21);          // bir kez, boot'ta
 *   buzzer_tone(880, 50);              // 880 Hz, %50 duty
 *   ...
 *   buzzer_stop();                     // sessize al (duty=0)
 *
 * Thread-safety:
 *   LEDC API'leri kendi içinde mutex'lidir; çağrılar herhangi bir task'tan
 *   yapılabilir. ISR context'te ÇAĞIRMAYIN — `_async` API'leri tercih edilir.
 */

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include <driver/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LEDC timer + kanalını verilen GPIO için yapılandırır.
 *
 * @param pin LEDC PWM çıkışı verilecek GPIO. Octal PSRAM kullanılan
 *            board'larda 33..37 KULLANILMAZ; 21 gibi boş pinler tercih
 *            edilmeli.
 * @return ESP_OK başarı, hatalı durumda LEDC API'sinin döndürdüğü kod.
 *
 * Init sonrası buzzer susturulmuş (duty=0) durumdadır. `buzzer_tone`
 * çağrılana kadar pin LOW kalır.
 */
esp_err_t buzzer_init(gpio_num_t pin);

/**
 * @brief Verilen frekansta tone üretmeye başlar.
 *
 * @param freq_hz 50..20000 Hz aralığı önerilir; LEDC kararlılığı için
 *                resolution=13-bit ile 50 Hz altında çalışmaz.
 * @param duty_pct 0..100. 50 = simetrik square wave (en yüksek ses),
 *                 100 = sürekli HIGH (ses yok), 0 = sessizlik.
 * @return ESP_OK başarı; freq_hz LEDC sınırları dışındaysa ESP_ERR_INVALID_ARG.
 */
esp_err_t buzzer_tone(uint32_t freq_hz, uint8_t duty_pct);

/** @brief Tone'u kapatır (duty=0, pin LOW). */
esp_err_t buzzer_stop(void);

/** @brief Init edildi mi? (test ve defensive guard'lar için.) */
bool buzzer_is_inited(void);

#ifdef __cplusplus
}
#endif
