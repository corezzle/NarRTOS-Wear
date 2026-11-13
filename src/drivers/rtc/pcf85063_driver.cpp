/**
 * @file pcf85063_driver.cpp
 * @brief PCF85063 RTC sürücüsü (I2C, addr 0x51)
 *
 * NXP PCF85063A/TL — ±3.5ppm doğruluk, dahili osilatör düzeltme kaydı.
 *
 * Saat verisi BCD formatında saklanır. Tüm okuma/yazma işlemleri
 * bcd_to_dec() / dec_to_bcd() dönüşümleriyle normalize edilir.
 *
 * Alarm & Timer: Bu projede kullanılmıyor (PCF85063 alarmı opsiyonel).
 * Deep sleep wakeup için RTC alarm yerine AXP2101 power button + IMU WOM tercih edilir.
 *
 * Register map: PCF85063A datasheet Rev 7 (2021)
 */

#include "pcf85063_driver.h"
#include "../../core/os_config.h"
#include "../../hal/i2c_bus.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <stdlib.h>

static const char* TAG = "PCF85063";

// ─── Register Map ─────────────────────────────────────────────────────────────
#define PCF_REG_CTRL1       0x00   // Control_1: CAP_SEL, STOP, CIE, 12_24, TESTC, EXT_TEST
#define PCF_REG_CTRL2       0x01   // Control_2: AIE, AF, MI, HMI, TF, COF[2:0]
#define PCF_REG_OFFSET      0x02   // Offset: OOZE, offset[6:0]
#define PCF_REG_RAM_BYTE    0x03   // Scratchpad RAM byte
#define PCF_REG_SECONDS     0x04   // OS (oscillator stop flag) + seconds BCD
#define PCF_REG_MINUTES     0x05
#define PCF_REG_HOURS       0x06   // [5] = 12/24 mode select
#define PCF_REG_DAYS        0x07
#define PCF_REG_WEEKDAYS    0x08   // 0=Sun ... 6=Sat
#define PCF_REG_MONTHS      0x09
#define PCF_REG_YEARS       0x0A   // 0–99 (offset from 2000)

// Control_1 bitleri
#define CTRL1_STOP          (1 << 5)   // 1=stop clock
#define CTRL1_12_24         (1 << 1)   // 0=24h, 1=12h (biz 24h kullanıyoruz)
#define CTRL1_CAP_SEL       (1 << 0)   // 0=7pF, 1=12.5pF kristal kapasitesi

// Seconds registerındaki OS (Oscillator Stop) biti
#define SECONDS_OS_FLAG     (1 << 7)

// ─── BCD dönüşümleri ─────────────────────────────────────────────────────────

static inline uint8_t bcd_to_dec(uint8_t bcd)
{
    return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F));
}

static inline uint8_t dec_to_bcd(uint8_t dec)
{
    return (uint8_t)(((dec / 10) << 4) | (dec % 10));
}

// ─── pcf85063_init ───────────────────────────────────────────────────────────

esp_err_t pcf85063_init(void)
{
    ESP_LOGI(TAG, "Initializing PCF85063 RTC...");

    // Control_1: 24h mode, 7pF cap, clock running (STOP=0)
    // CAP_SEL=0 (7pF) — donanıma göre ayarlanmalı; çoğu AMOLED saatte 7pF
    esp_err_t ret = i2c_bus_write_reg(I2C_ADDR_PCF85063, PCF_REG_CTRL1, 0x00);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCF85063 not responding: %s", esp_err_to_name(ret));
        return ret;
    }

    // Oscillator stop flag kontrolü
    uint8_t sec_reg = 0;
    ret = i2c_bus_read_reg(I2C_ADDR_PCF85063, PCF_REG_SECONDS, &sec_reg, 1);
    if (ret != ESP_OK) return ret;

    if (sec_reg & SECONDS_OS_FLAG) {
        // Osilatör durmuş → derleme zamanını varsayılan olarak yaz
        // (time_service_init zaten daha sonra geçerliyse RTC'den okur / geçersizse üzerine yazar)
        ESP_LOGW(TAG, "Oscillator stop detected, using compile timestamp as initial time");
        const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                 "Jul","Aug","Sep","Oct","Nov","Dec"};
        struct tm ct = {};
        for (int i = 0; i < 12; i++) {
            if (strncmp(__DATE__, months[i], 3) == 0) { ct.tm_mon = i; break; }
        }
        ct.tm_mday = atoi(__DATE__ + 4);
        ct.tm_year = atoi(__DATE__ + 7) - 1900;
        ct.tm_hour = atoi(__TIME__);
        ct.tm_min  = atoi(__TIME__ + 3);
        ct.tm_sec  = atoi(__TIME__ + 6);
        pcf85063_set_time(&ct);
    }

    // Control_2: Timer ve alarm kesmeleri kapalı, CLKOUT frekansı 32kHz (COF=7)
    i2c_bus_write_reg(I2C_ADDR_PCF85063, PCF_REG_CTRL2, 0x07);

    ESP_LOGI(TAG, "PCF85063 init OK%s",
        (sec_reg & SECONDS_OS_FLAG) ? " (time was reset)" : "");
    return ESP_OK;
}

// ─── pcf85063_get_time ───────────────────────────────────────────────────────

esp_err_t pcf85063_get_time(struct tm* out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    // 7 register'ı tek okumada al: SEC, MIN, HOUR, DAY, WEEKDAY, MONTH, YEAR
    uint8_t raw[7];
    esp_err_t ret = i2c_bus_read_reg(I2C_ADDR_PCF85063, PCF_REG_SECONDS, raw, sizeof(raw));
    if (ret != ESP_OK) return ret;

    // OS flag aktifse saat güvenilmez
    if (raw[0] & SECONDS_OS_FLAG) {
        ESP_LOGW(TAG, "get_time: oscillator stop flag set, time unreliable");
    }

    out->tm_sec  = bcd_to_dec(raw[0] & 0x7F);   // Bit 7 = OS flag, maskele
    out->tm_min  = bcd_to_dec(raw[1] & 0x7F);
    out->tm_hour = bcd_to_dec(raw[2] & 0x3F);   // 24h: bit[5:0]
    out->tm_mday = bcd_to_dec(raw[3] & 0x3F);
    out->tm_wday = bcd_to_dec(raw[4] & 0x07);
    out->tm_mon  = bcd_to_dec(raw[5] & 0x1F) - 1;  // PCF: 1–12 → tm: 0–11
    out->tm_year = bcd_to_dec(raw[6]) + 100;         // PCF: 0–99 (2000+) → tm: 100–199 (1900+)

    out->tm_isdst = -1;  // DST bilgisi yok

    return ESP_OK;
}

// ─── pcf85063_set_time ───────────────────────────────────────────────────────

esp_err_t pcf85063_set_time(const struct tm* t)
{
    if (!t) return ESP_ERR_INVALID_ARG;

    // Saati durdur (STOP=1) → yaz → yeniden başlat (STOP=0)
    // Bu sayede yazma sırasında yarım saniye geçişi yakalanmaz.
    ESP_ERROR_CHECK(i2c_bus_write_reg(I2C_ADDR_PCF85063, PCF_REG_CTRL1, CTRL1_STOP));
    vTaskDelay(1);  // 1ms: saatin tam durması için

    uint8_t raw[8];
    raw[0] = PCF_REG_SECONDS;                              // register adresi (başlangıç)
    raw[1] = dec_to_bcd((uint8_t)t->tm_sec)  & 0x7F;      // OS flag temizle
    raw[2] = dec_to_bcd((uint8_t)t->tm_min)  & 0x7F;
    raw[3] = dec_to_bcd((uint8_t)t->tm_hour) & 0x3F;
    raw[4] = dec_to_bcd((uint8_t)t->tm_mday) & 0x3F;
    raw[5] = dec_to_bcd((uint8_t)t->tm_wday) & 0x07;
    raw[6] = dec_to_bcd((uint8_t)(t->tm_mon + 1)) & 0x1F; // tm: 0–11 → PCF: 1–12
    raw[7] = dec_to_bcd((uint8_t)(t->tm_year - 100));      // tm: 100–199 → PCF: 0–99

    ESP_ERROR_CHECK(i2c_bus_write(I2C_ADDR_PCF85063, raw, sizeof(raw)));

    // Saati yeniden başlat (STOP=0, CAP_SEL=0, 24h)
    ESP_ERROR_CHECK(i2c_bus_write_reg(I2C_ADDR_PCF85063, PCF_REG_CTRL1, 0x00));

    ESP_LOGI(TAG, "Time set: %04d-%02d-%02d %02d:%02d:%02d",
        t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
        t->tm_hour, t->tm_min, t->tm_sec);

    return ESP_OK;
}
