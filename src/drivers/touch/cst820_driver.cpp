/**
 * @file cst820_driver.cpp
 * @brief CST820 kapasitif dokunmatik denetleyicisi — NAR-ESP32S3 board
 *        (I2C addr 0x15, ortak I2C 15/14, RST=GPIO9, INT=GPIO38)
 *
 * Varyant kalıbı cst816s_128_driver.cpp ile aynı: dosyanın tamamı
 * `#ifdef BOARD_VARIANT_NAR` içinde ve **ft3168_* fonksiyon imzalarını**
 * export eder → main.cpp / ui_service.cpp değişmez. ft3168_driver.cpp NAR
 * build'ında boş translation unit olur.
 *
 * CST820 (Hynitron) CST816 ailesinin register yerleşimini kullanır:
 *   0x02 FingerNum, 0x03 XposH ([7:6]=event, [3:0]=X[11:8]), 0x04 XposL,
 *   0x05 YposH, 0x06 YposL; 0xA7 ChipID, 0xA9 FwVer, 0xA5 SleepMode,
 *   0xFA IrqCtl, 0xFE DisAutoSleep.
 *
 * CST816S ile aynı iki kritik nokta:
 *   1. AUTO-SLEEP: dokunuşsuz ~2 sn sonra chip uyur ve I2C'ye cevap vermez →
 *      init'te 0xFE ile kapatılır (polling düzeni için şart).
 *   2. INT sürekli LOW değil, pulse → ui_service NAR'da da "parmak var"
 *      bilgisini pinden değil chip raporundan okur (LCD128 yolu).
 */

#ifdef BOARD_VARIANT_NAR

#include "ft3168_driver.h"
#include "../../core/os_config.h"
#include "../../hal/i2c_bus.h"
#include "../display/co5300_driver.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "CST820";

/* Çip init'te cevap vermediyse her döngüde okumak I2C zaman aşımıyla UI'ı
 * ~104 ms kilitliyordu (NAR ilk testi). Yokken saniyede bir yoklanır. */
static bool    s_present      = false;
static int64_t s_next_probe_us = 0;

#define CST_REG_FINGER_NUM    0x02
#define CST_REG_CHIP_ID       0xA7
#define CST_REG_FW_VER        0xA9
#define CST_REG_SLEEP_MODE    0xA5   // 0x03 yaz → standby; uyanış = RST pulse
#define CST_REG_IRQ_CTL       0xFA
#define CST_REG_DIS_AUTOSLEEP 0xFE   // !=0 yaz → auto-sleep KAPALI

#define CST_IRQ_EN_TOUCH      0x40
#define CST_IRQ_EN_CHANGE     0x20
#define CST_SLEEP_CMD         0x03

static inline esp_err_t cst_read(uint8_t reg, uint8_t* buf, size_t len)
{
    return i2c_bus_read_reg(I2C_ADDR_CST820, reg, buf, len);
}

static inline esp_err_t cst_write(uint8_t reg, uint8_t val)
{
    return i2c_bus_write_reg(I2C_ADDR_CST820, reg, val);
}

esp_err_t ft3168_init(void)
{
    ESP_LOGI(TAG, "CST820 touch init (NAR board)");

    /* RESET pulse — standby'dan uyandırmanın da tek yolu (I2C ile uyanmaz). */
    gpio_reset_pin((gpio_num_t)PIN_TOUCH_RST);
    gpio_set_direction((gpio_num_t)PIN_TOUCH_RST, GPIO_MODE_INPUT_OUTPUT);   /* seviye okunabilsin (teşhis) */
    /* Session 46f teşhis: reset ÖNCESİ, reset LOW ve reset sonrası 0–600 ms
     * boyunca 10 ms'de bir yoklanır; çip geç açılıyor mu, başka adreste mi? */
    {
        uint8_t v = 0;
        bool pre = (i2c_bus_read_reg(I2C_ADDR_CST820, CST_REG_CHIP_ID, &v, 1) == ESP_OK);
        ESP_LOGW(TAG, "[touch-diag] reset oncesi 0x15: %s", pre ? "CEVAP VAR" : "yok");
    }
    gpio_set_level((gpio_num_t)PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)PIN_TOUCH_RST, 1);
    {
        int64_t t0 = esp_timer_get_time();
        int first_ms = -1;
        uint8_t v = 0;
        while (esp_timer_get_time() - t0 < 600000) {
            if (i2c_bus_read_reg(I2C_ADDR_CST820, CST_REG_CHIP_ID, &v, 1) == ESP_OK) {
                first_ms = (int)((esp_timer_get_time() - t0) / 1000);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (first_ms >= 0) ESP_LOGW(TAG, "[touch-diag] 0x15 reset'ten %d ms sonra cevap verdi (chip_id=0x%02X)", first_ms, v);
        else               ESP_LOGW(TAG, "[touch-diag] 0x15 reset sonrasi 600 ms boyunca HIC cevap vermedi");

        /* Dokunmatik denetleyicilerin olası diğer adresleri */
        static const uint8_t alt[] = { 0x14, 0x1A, 0x2E, 0x38, 0x48, 0x5A, 0x5D };
        char found[64] = "";
        for (size_t i = 0; i < sizeof(alt); i++) {
            uint8_t x = 0;
            if (i2c_bus_read_reg(alt[i], 0x00, &x, 1) == ESP_OK) {
                char t[8]; snprintf(t, sizeof(t), " %02X", alt[i]); strlcat(found, t, sizeof(found));
            }
        }
        ESP_LOGW(TAG, "[touch-diag] alternatif dokunmatik adresleri:%s", found[0] ? found : " hicbiri yok");
        gpio_set_direction((gpio_num_t)PIN_TOUCH_INT, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)PIN_TOUCH_INT, GPIO_FLOATING);   /* dış seviye görünsün */
        vTaskDelay(pdMS_TO_TICKS(5));
        ESP_LOGW(TAG, "[touch-diag] RST(GPIO%d)=%d INT(GPIO%d, pull yok)=%d", PIN_TOUCH_RST,
                 gpio_get_level((gpio_num_t)PIN_TOUCH_RST), PIN_TOUCH_INT,
                 gpio_get_level((gpio_num_t)PIN_TOUCH_INT));
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_INT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    uint8_t chip_id = 0;
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        ret = cst_read(CST_REG_CHIP_ID, &chip_id, 1);
        if (ret == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CST820 I2C check failed after retries — GPIO still configured");
        s_present = false;
        s_next_probe_us = esp_timer_get_time() + 1000000;
        return ret;   // main.cpp tolere ediyor (FT ile aynı sözleşme)
    }

    esp_err_t w = cst_write(CST_REG_DIS_AUTOSLEEP, 0x01);
    if (w != ESP_OK) ESP_LOGW(TAG, "DisAutoSleep write fail (0x%x)", w);

    w = cst_write(CST_REG_IRQ_CTL, CST_IRQ_EN_TOUCH | CST_IRQ_EN_CHANGE);
    if (w != ESP_OK) ESP_LOGW(TAG, "IrqCtl write fail (0x%x)", w);

    uint8_t fw = 0;
    cst_read(CST_REG_FW_VER, &fw, 1);
    s_present = true;
    ESP_LOGI(TAG, "CST820 OK (chip_id=0x%02X fw=%u, auto-sleep off)", chip_id, fw);
    return ESP_OK;
}

esp_err_t ft3168_read(ft3168_point_t* out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    if (!s_present) {
        out->pressed = false;
        int64_t now = esp_timer_get_time();
        if (now < s_next_probe_us) return ESP_OK;
        s_next_probe_us = now + 1000000;
        uint8_t id = 0;
        if (cst_read(CST_REG_CHIP_ID, &id, 1) != ESP_OK) return ESP_OK;
        /* Sonradan cevap verdi (ör. flex yeniden oturdu) → uyku kapat, devam */
        cst_write(CST_REG_DIS_AUTOSLEEP, 0x01);
        cst_write(CST_REG_IRQ_CTL, CST_IRQ_EN_TOUCH | CST_IRQ_EN_CHANGE);
        s_present = true;
        ESP_LOGI(TAG, "CST820 gec cevap verdi (chip_id=0x%02X) — dokunmatik aktif", id);
    }

    /* Tek transaction burst read 0x02..0x06 = [finger_num, XH, XL, YH, YL] */
    uint8_t buf[5];
    esp_err_t ret = cst_read(CST_REG_FINGER_NUM, buf, sizeof(buf));

    if (ret != ESP_OK || buf[0] == 0) {
        out->pressed = false;
        return ESP_OK;   // okuma hatası = dokunuş yok say (FT sözleşmesi)
    }

    uint8_t event = (buf[1] >> 6) & 0x03;   // 0=down, 1=up, 2=contact
    out->x = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
    out->y = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
    out->pressed = (event == 0 || event == 2);

    /* Panel koordinatları native kabul edilir; defansif clamp */
    if (out->x >= CO5300_H_RES) out->x = CO5300_H_RES - 1;
    if (out->y >= CO5300_V_RES) out->y = CO5300_V_RES - 1;

    return ESP_OK;
}

esp_err_t ft3168_suspend(void)
{
    esp_err_t ret = cst_write(CST_REG_SLEEP_MODE, CST_SLEEP_CMD);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Suspend write fail (0x%x) — touch IC may stay active", ret);
        return ret;
    }
    ESP_LOGI(TAG, "CST820 → STANDBY (~uA)");
    return ESP_OK;
}

#endif /* BOARD_VARIANT_NAR */
