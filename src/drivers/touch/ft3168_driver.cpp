/**
 * @file ft3168_driver.cpp
 * @brief FT3168 kapasitif dokunmatik ekran denetleyicisi (I2C, addr 0x38)
 *
 * Protokol: FocalTech FT5x06 serisiyle uyumlu.
 * Tek parmak dokunuş: register 0x03 (gesture) + 0x06 (touch count) + 0x07-0x0A (XY)
 */

/* v1.2.9b — Touch varyant guard'ı (display sürücüsü kalıbının aynısı).
 * esp32s3_lcd128 build'ında bu dosya boş TU olur; cst816s_128_driver.cpp
 * aynı ft3168_* sembollerini sağlar. Default AMOLED build'ı değişmez. */
#if !defined(DISPLAY_VARIANT_LCD128) && !defined(BOARD_VARIANT_NAR)   /* NAR: cst820_driver.cpp */

#include "ft3168_driver.h"
#include "../../core/os_config.h"
#include "../../hal/i2c_bus.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "FT3168";

#define FT_REG_GESTURE      0x01
#define FT_REG_TOUCH_COUNT  0x02
#define FT_REG_TOUCH1_XH    0x03   // [7:6]=event, [3:0]=X high
#define FT_REG_TOUCH1_XL    0x04
#define FT_REG_TOUCH1_YH    0x05   // [7:4]=touch ID, [3:0]=Y high
#define FT_REG_TOUCH1_YL    0x06

/* Sensitivity tuning registers (FocalTech FT5x06/FT3168 protokolü):
 *   0x80 THGROUP   — touch detection threshold. Düşük değer = daha hassas
 *                    detection. Default ~22-32 (kalın cam + glove için
 *                    tasarlandı); ince AMOLED + parmak için 8-12 ideal.
 *   0x85 TH_PEAK   — minimum signal peak threshold. Düşük = hafif tap algılar.
 *   0x88 PERIODACTIVE — active mode scan period (ms). Düşük = hızlı scan,
 *                       düşük input latency. Default ~10-12 ms; 6 ms snappier.
 *
 * Kullanıcı raporu 2026-05-05: "selector için basılı tutuyorum, ilkinde
 * algılamıyor, sert basmak gerekiyor" → threshold çok yüksek; düşürüldü. */
#define FT_REG_THGROUP      0x80
#define FT_REG_THPEAK       0x85
#define FT_REG_PERIODACTIVE 0x88

#define FT_THGROUP_VAL      8     /* default ~22 → 8: çok hassas */
#define FT_THPEAK_VAL       50    /* default ~60 → 50: hafif tap'leri yakala */
#define FT_PERIODACTIVE_VAL 6     /* default ~12 ms → 6 ms: 2× scan rate */

/* Power mode register — FocalTech FT5x06/FT3168 G_MODE.
 *   0x00 = ACTIVE   (full scan, ~5-10 mA)
 *   0x01 = MONITOR  (slow scan, ~1 mA)
 *   0x03 = HIBERNATE (full power-down, ~5 µA, wake = RESET toggle)
 */
#define FT_REG_POWER_MODE   0xA5
#define FT_PWR_HIBERNATE    0x03

static inline esp_err_t ft_read(uint8_t reg, uint8_t* buf, size_t len)
{
    return i2c_bus_read_reg(I2C_ADDR_FT3168, reg, buf, len);
}

static inline esp_err_t ft_write(uint8_t reg, uint8_t val)
{
    return i2c_bus_write_reg(I2C_ADDR_FT3168, reg, val);
}

esp_err_t ft3168_init(void)
{
    ESP_LOGI(TAG, "FT3168 touch init");

    /* RESET pin pulse — hibernate'ten uyandırma + boot reset.
     * Önceki boot'ta deep sleep öncesi ft3168_suspend çağrılmışsa chip
     * HIBERNATE modunda durur (sadece RESET pin'i ile uyanır). İlk boot'ta
     * da chip'i temiz state'e almak için reset zaten faydalı. */
    gpio_reset_pin((gpio_num_t)PIN_TOUCH_RST);
    gpio_set_direction((gpio_num_t)PIN_TOUCH_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));   // RESET LOW hold
    gpio_set_level((gpio_num_t)PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));   // Boot delay (FT3168 internal init)

    // INT pini: input, pull-up
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_INT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    // FT3168 boot'ta hazır olmayabilir — 3 kez retry yap
    uint8_t cnt = 0;
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        ret = ft_read(FT_REG_TOUCH_COUNT, &cnt, 1);
        if (ret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "FT3168 I2C check failed after retries — GPIO still configured");
        return ret;  // caller artık bunu görmezden geliyor
    }

    /* Sensitivity tuning — kullanıcı "sert basmak gerekiyor" raporu sonrası.
     * Default değerler kalın cam/glove için; ince AMOLED + ten için aşağı
     * çekiyoruz. ft_write fail olursa görmezden gel — sensitivity ideal
     * olmaz ama touch yine çalışır. */
    esp_err_t w;
    w = ft_write(FT_REG_THGROUP, FT_THGROUP_VAL);
    if (w != ESP_OK) ESP_LOGW(TAG, "THGROUP write fail (0x%x) — using default sensitivity", w);
    w = ft_write(FT_REG_THPEAK, FT_THPEAK_VAL);
    if (w != ESP_OK) ESP_LOGW(TAG, "THPEAK write fail (0x%x)", w);
    w = ft_write(FT_REG_PERIODACTIVE, FT_PERIODACTIVE_VAL);
    if (w != ESP_OK) ESP_LOGW(TAG, "PERIODACTIVE write fail (0x%x)", w);

    /* Doğrula — okudugumuz değerler set edildi mi? */
    uint8_t v_th = 0, v_pk = 0, v_pa = 0;
    ft_read(FT_REG_THGROUP, &v_th, 1);
    ft_read(FT_REG_THPEAK, &v_pk, 1);
    ft_read(FT_REG_PERIODACTIVE, &v_pa, 1);
    ESP_LOGI(TAG, "FT3168 OK (sensitivity: THGROUP=%u THPEAK=%u PERIOD=%ums)",
             v_th, v_pk, v_pa);
    return ESP_OK;
}

esp_err_t ft3168_read(ft3168_point_t* out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    /* Atomic read: touch_count + first touch (XH/XL/YH/YL) tek I2C transaction'da.
     * Önceden iki ayrı ft_read çağrısı vardı (önce 0x03–0x06, sonra 0x02);
     * bu iki okuma arasında FT3168 internal state güncellenebiliyordu →
     * stabil dokunuşta touch_cnt geçici 0 dönüyor, koordinatlar geçerli ama
     * pressed=false raporluyorduk → LVGL gözünde RELEASE → click/long-press yutuluyordu.
     * 0x02..0x06 = [touch_cnt, XH, XL, YH, YL] → tek burst-read ile tutarlılık. */
    uint8_t buf[5];
    esp_err_t ret = ft_read(FT_REG_TOUCH_COUNT, buf, sizeof(buf));
    if (ret != ESP_OK) {
        out->pressed = false;
        return ESP_OK;  // Okuma başarısız → dokunuş yok say, crash etme
    }

    uint8_t touch_cnt = buf[0];
    if (touch_cnt == 0) {
        out->pressed = false;
        return ESP_OK;
    }

    // buf[1]=XH (event[7:6] + X[11:8]), buf[2]=XL, buf[3]=YH, buf[4]=YL
    uint8_t event = (buf[1] >> 6) & 0x03;   // 0=down, 1=up, 2=contact
    out->x = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
    out->y = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
    out->pressed = (event == 0 || event == 2);

    return ESP_OK;
}

esp_err_t ft3168_suspend(void)
{
    /* Power Mode register'ına 0x03 (HIBERNATE) yaz.
     * Chip taramayı durdurur, ~5-10 mA tüketim ~5 µA'ya iner. Uyanmak için
     * RESET pin pulse gerekli (ft3168_init başında yapıyoruz).
     *
     * I2C write fail olabilir (chip cevap vermiyorsa) — bunu görmezden gel,
     * deep sleep yine de devam etmeli. */
    esp_err_t ret = ft_write(FT_REG_POWER_MODE, FT_PWR_HIBERNATE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Suspend write fail (0x%x) — touch IC may stay active", ret);
        return ret;
    }
    ESP_LOGI(TAG, "FT3168 → HIBERNATE (~5 uA)");
    return ESP_OK;
}

#endif /* !DISPLAY_VARIANT_LCD128 */
