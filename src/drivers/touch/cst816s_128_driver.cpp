/**
 * @file cst816s_128_driver.cpp
 * @brief CST816S kapasitif dokunmatik denetleyicisi — ESP32-S3-Touch-LCD-1.28
 *        board varyantı (I2C addr 0x15, INT=GPIO5, RST=GPIO13)
 *
 * Display sürücüsüyle aynı varyant kalıbı: bu dosyanın tamamı
 * `#ifdef DISPLAY_VARIANT_LCD128` içinde ve **ft3168_* fonksiyon imzalarını**
 * export eder → main.cpp / ui_service.cpp değişmez. Default AMOLED build'ında
 * boş translation unit; ft3168_driver.cpp ise LCD128 build'ında boşalır.
 *
 * CST816S (Hynitron), FocalTech FT5x06 register yerleşiminin neredeyse
 * birebir kopyasını kullanır:
 *   0x02 FingerNum, 0x03 XposH ([7:6]=event, [3:0]=X[11:8]), 0x04 XposL,
 *   0x05 YposH, 0x06 YposL → ft3168_read ile AYNI parse mantığı.
 *
 * Kritik CST816S farkları:
 *   1. AUTO-SLEEP: chip default'ta ~2 sn dokunuşsuzluktan sonra kendini
 *      uyutur ve I2C'ye CEVAP VERMEZ (sadece dokunuşla uyanır). Polling
 *      tabanlı okuma düzenimizde bu ölümcül → init'te 0xFE (DisAutoSleep)
 *      ile kapatılır.
 *   2. INT davranışı: FT gibi "parmak basılıyken sürekli LOW" değil; her
 *      touch raporunda kısa LOW pulse üretir. 0xFA IrqCtl'e EnTouch|EnChange
 *      yazarak pulse frekansı rapor hızına (~10-15 ms) çıkarılır —
 *      ui_service'in mevcut debounce mantığı (kısa HIGH'ları "parmak hâlâ
 *      basılı" sayar) bu pulse'ları FT'nin geçici HIGH'ları gibi tolere
 *      eder. Cihaz üstü testte gerekirse debounce süresi ayarlanır.
 *   3. Sensitivity register'ları (THGROUP vs.) YOK — atlanır.
 */

#ifdef DISPLAY_VARIANT_LCD128

#include "ft3168_driver.h"
#include "../../core/os_config.h"
#include "../../hal/i2c_bus.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

static const char* TAG = "CST816S";

#define CST_REG_GESTURE     0x01
#define CST_REG_FINGER_NUM  0x02
#define CST_REG_XPOS_H      0x03   // [7:6]=event, [3:0]=X high
#define CST_REG_XPOS_L      0x04
#define CST_REG_YPOS_H      0x05   // [3:0]=Y high
#define CST_REG_YPOS_L      0x06

#define CST_REG_CHIP_ID     0xA7   // CST816S=0xB4, CST816T=0xB5, CST816D=0xB6
#define CST_REG_FW_VER      0xA9
#define CST_REG_SLEEP_MODE  0xA5   // 0x03 yaz → standby (~µA); wake = RST pulse
#define CST_REG_IRQ_CTL     0xFA
#define CST_REG_DIS_AUTOSLEEP 0xFE // !=0 yaz → auto-sleep KAPALI

#define CST_IRQ_EN_TOUCH    0x40   // dokunuş algılandıkça periyodik pulse
#define CST_IRQ_EN_CHANGE   0x20   // dokunuş durumu değişince pulse

#define CST_SLEEP_CMD       0x03

static inline esp_err_t cst_read(uint8_t reg, uint8_t* buf, size_t len)
{
    return i2c_bus_read_reg(I2C_ADDR_CST816S, reg, buf, len);
}

static inline esp_err_t cst_write(uint8_t reg, uint8_t val)
{
    return i2c_bus_write_reg(I2C_ADDR_CST816S, reg, val);
}

esp_err_t ft3168_init(void)
{
    ESP_LOGI(TAG, "CST816S touch init (LCD128 board)");

    /* RESET pulse — standby'dan uyandırmanın da tek yolu (I2C ile uyanmaz). */
    gpio_reset_pin((gpio_num_t)PIN_TOUCH_RST);
    gpio_set_direction((gpio_num_t)PIN_TOUCH_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));   // CST816S boot süresi (~50-100 ms, datasheet)

    /* INT pini: input + pull-up (idle HIGH, pulse LOW) */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_INT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* Chip ID doğrulama — 3 retry (boot sonrası ilk okuma gecikebilir) */
    uint8_t chip_id = 0;
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        ret = cst_read(CST_REG_CHIP_ID, &chip_id, 1);
        if (ret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CST816S I2C check failed after retries — GPIO still configured");
        return ret;   // main.cpp tolere ediyor (FT ile aynı sözleşme)
    }

    /* 1. Auto-sleep KAPAT — polling düzeninde chip hep ayakta kalmalı.
     *    (Pil optimizasyonu Faz 3'te: suspend/resume akışına bağlanır.) */
    esp_err_t w = cst_write(CST_REG_DIS_AUTOSLEEP, 0x01);
    if (w != ESP_OK) ESP_LOGW(TAG, "DisAutoSleep write fail (0x%x)", w);

    /* 2. INT pulse modu: dokunuş boyunca periyodik + değişimde pulse. */
    w = cst_write(CST_REG_IRQ_CTL, CST_IRQ_EN_TOUCH | CST_IRQ_EN_CHANGE);
    if (w != ESP_OK) ESP_LOGW(TAG, "IrqCtl write fail (0x%x)", w);

    uint8_t fw = 0;
    cst_read(CST_REG_FW_VER, &fw, 1);
    ESP_LOGI(TAG, "CST816S OK (chip_id=0x%02X fw=%u, auto-sleep off)", chip_id, fw);
    return ESP_OK;
}

esp_err_t ft3168_read(ft3168_point_t* out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    /* Atomic burst read 0x02..0x06 = [finger_num, XH, XL, YH, YL] —
     * ft3168_read ile aynı tutarlılık gerekçesi (iki ayrı okuma arasında
     * chip state değişirse pressed/koordinat tutarsızlaşır). */
    /* Burst read 0x02..0x06 = [finger_num, XH, XL, YH, YL] — ft3168_read
     * ile aynı tutarlılık gerekçesi (tek transaction, state kayması yok). */
    uint8_t buf[5];
    esp_err_t ret = cst_read(CST_REG_FINGER_NUM, buf, sizeof(buf));

    if (ret != ESP_OK) {
        out->pressed = false;
        return ESP_OK;   // okuma hatası = dokunuş yok say (FT sözleşmesi)
    }

    if (buf[0] == 0) {
        out->pressed = false;
        return ESP_OK;
    }

    uint8_t event = (buf[1] >> 6) & 0x03;   // 0=down, 1=up, 2=contact
    out->x = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
    out->y = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
    out->pressed = (event == 0 || event == 2);

    /* 240×240 panel — koordinatlar native, ölçekleme gerekmez. Defansif clamp. */
    if (out->x > 239) out->x = 239;
    if (out->y > 239) out->y = 239;

    return ESP_OK;
}

esp_err_t ft3168_suspend(void)
{
    /* Standby komutu — tüketim ~µA seviyesine iner. Uyanış SADECE RST
     * pulse ile (ft3168_init). I2C hatasını görmezden gel (FT sözleşmesi). */
    esp_err_t ret = cst_write(CST_REG_SLEEP_MODE, CST_SLEEP_CMD);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Suspend write fail (0x%x) — touch IC may stay active", ret);
        return ret;
    }
    ESP_LOGI(TAG, "CST816S → STANDBY (~uA)");
    return ESP_OK;
}

#endif /* DISPLAY_VARIANT_LCD128 */
