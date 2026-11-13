/**
 * @file buzzer_driver.cpp
 * @brief LEDC PWM tabanlı square-wave tone üretici.
 *
 * Yapı:
 *   • LEDC_TIMER_0  : duty resolution 10-bit (0..1023). 10-bit yeterli;
 *                     50 Hz - 78 kHz bandında çalışır (APB clock 80 MHz).
 *                     13-bit kullanırsak alt sınır 9.7 kHz'e iner ki 880 Hz
 *                     gibi alarm frekansları girmez. 10-bit 76 Hz alt sınır
 *                     verir → 100 Hz üzerindeki tüm tonlar OK.
 *   • LEDC_CHANNEL_0: tek bir GPIO'ya bağlı. 4 farklı tone üretici
 *                     (channel 0..3) gerekirse bu yapı çoğaltılabilir.
 *   • LEDC_LOW_SPEED_MODE: ESP32-S3'te HIGH_SPEED_MODE yok, sadece
 *                          LOW_SPEED_MODE var. Donanım ismi yanıltıcı —
 *                          aynı APB clock kullanılır, sadece register
 *                          dolaplaması farklı.
 *
 * Frekans değiştirme:
 *   `ledc_set_freq` timer'ın frekansını canlı günceller; aynı timer'ı
 *   kullanan tüm kanallar etkilenir. duty register'ı normalize edilir,
 *   buzzer_tone çağrısında duty'yi yeniden set ediyoruz → frekans + duty
 *   her seferinde tutarlı.
 */

#include "buzzer_driver.h"
#include <driver/ledc.h>
#include <esp_log.h>

static const char* TAG = "Buzzer";

#define BUZZ_TIMER       LEDC_TIMER_0
#define BUZZ_CHANNEL     LEDC_CHANNEL_0
#define BUZZ_MODE        LEDC_LOW_SPEED_MODE
#define BUZZ_RES_BITS    LEDC_TIMER_10_BIT          /* 1024 step */
#define BUZZ_DUTY_MAX    ((1u << 10) - 1u)          /* 1023 */
#define BUZZ_DEFAULT_HZ  1000u                       /* init başlangıç frekansı */

static bool         s_inited = false;
static gpio_num_t   s_pin    = GPIO_NUM_NC;

esp_err_t buzzer_init(gpio_num_t pin)
{
    if (s_inited) {
        ESP_LOGW(TAG, "already inited on GPIO %d", (int)s_pin);
        return ESP_OK;
    }

    /* 1) Timer yapılandırması — duty resolution + initial frekans. */
    ledc_timer_config_t tcfg = {};
    tcfg.speed_mode      = BUZZ_MODE;
    tcfg.timer_num       = BUZZ_TIMER;
    tcfg.duty_resolution = BUZZ_RES_BITS;
    tcfg.freq_hz         = BUZZ_DEFAULT_HZ;
    tcfg.clk_cfg         = LEDC_AUTO_CLK;

    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timer_config failed: 0x%x", err);
        return err;
    }

    /* 2) Channel — GPIO bağla, duty=0 ile sessiz başla. */
    ledc_channel_config_t ccfg = {};
    ccfg.gpio_num   = pin;
    ccfg.speed_mode = BUZZ_MODE;
    ccfg.channel    = BUZZ_CHANNEL;
    ccfg.intr_type  = LEDC_INTR_DISABLE;
    ccfg.timer_sel  = BUZZ_TIMER;
    ccfg.duty       = 0;
    ccfg.hpoint     = 0;

    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "channel_config failed: 0x%x", err);
        return err;
    }

    s_pin    = pin;
    s_inited = true;
    ESP_LOGI(TAG, "init OK on GPIO %d (LEDC timer=%d, ch=%d, %d-bit)",
             (int)pin, BUZZ_TIMER, BUZZ_CHANNEL, BUZZ_RES_BITS);
    return ESP_OK;
}

esp_err_t buzzer_tone(uint32_t freq_hz, uint8_t duty_pct)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (freq_hz < 50 || freq_hz > 20000) return ESP_ERR_INVALID_ARG;
    if (duty_pct > 100) duty_pct = 100;

    /* Frekansı güncelle. duty=0 yapmadan önce frekansı set ediyoruz —
     * bir tick için yanlış frekanslı duty olabilir, ama 80 ms'lik step'lerde
     * insan kulağı algılayamaz. */
    esp_err_t err = ledc_set_freq(BUZZ_MODE, BUZZ_TIMER, freq_hz);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_freq(%lu) failed: 0x%x", (unsigned long)freq_hz, err);
        return err;
    }

    /* Duty hesapla. duty=0 → pin LOW (sessiz). */
    uint32_t duty = (BUZZ_DUTY_MAX * (uint32_t)duty_pct) / 100u;
    err = ledc_set_duty(BUZZ_MODE, BUZZ_CHANNEL, duty);
    if (err != ESP_OK) return err;
    return ledc_update_duty(BUZZ_MODE, BUZZ_CHANNEL);
}

esp_err_t buzzer_stop(void)
{
    if (!s_inited) return ESP_OK;   /* idempotent */
    /* duty=0 → pin LOW. ledc_stop pin'i tamamen idle yapar; bunun yerine
     * duty=0 update ediyoruz — bir sonraki tone() çağrısı anında devam eder. */
    esp_err_t err = ledc_set_duty(BUZZ_MODE, BUZZ_CHANNEL, 0);
    if (err != ESP_OK) return err;
    return ledc_update_duty(BUZZ_MODE, BUZZ_CHANNEL);
}

bool buzzer_is_inited(void)
{
    return s_inited;
}
