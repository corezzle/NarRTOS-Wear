#pragma once
/**
 * @file os_config_lcd128_pins.h
 * @brief GPIO pin map — ESP32-S3-Touch-LCD-1.28 board varyantı
 *
 * Bu dosya SADECE `esp32s3_lcd128` env'inde include edilir
 * (os_config.h → #ifdef BOARD_VARIANT_LCD128). Default AMOLED build'ında
 * (esp32s3_wearable) preprocessor bu dosyayı hiç görmez — 2.06" board
 * pin haritası os_config.h içinde olduğu gibi durur.
 *
 * Kaynak: ESP32-S3-Touch-LCD-1.28 üretici belgeleri (2026-07 doğrulandı).
 * Donanım farkları (2.06" AMOLED board'a göre):
 *   - Ekran: GC9A01A 240×240 yuvarlak LCD, standart 1-bit SPI (QSPI değil),
 *     ayrı DC pini ve LEDC PWM backlight'ı var (AMOLED'de yoktu).
 *   - PMIC yok (AXP2101 probe fail olur — main.cpp tolere ediyor).
 *   - RTC yok (PCF85063 probe fail olur — internal clock'a düşer).
 *   - Dokunmatik: FT3168 değil CST816S (Faz 2'de driver yazılacak;
 *     FT3168 probe'u 0x38'de cevap alamaz, touch register yine yapılır).
 *   - Tek buton: BOOT (GPIO0). Yan buton yok — PIN_BTN_BOTTOM boşta olan
 *     GPIO16'ya map'lendi (internal pull-up ile idle HIGH → hiç basılmaz).
 */

// ─── Display (GC9A01A — 1-bit SPI) ───────────────────────────────────────────
#define PIN_DISP_CS          9
#define PIN_DISP_SCLK       10
#define PIN_DISP_MOSI       11
#define PIN_DISP_DC          8   // AMOLED'de yoktu (QSPI cmd-encoded); LCD'de zorunlu
#define PIN_DISP_RST        14
#define PIN_DISP_BL          2   // Backlight — LEDC PWM ile parlaklık kontrolü
#define PIN_DISP_TE         -1

// ─── I2C Bus (CST816S touch + QMI8658 IMU) ───────────────────────────────────
#define PIN_I2C_SDA          6
#define PIN_I2C_SCL          7
#define I2C_MASTER_FREQ_HZ  400000

// I2C adresleri — eski board çipleri bu bus'ta YOK; probe'lar fail olur ve
// ilgili servisler devre dışı kalır (main.cpp hepsini tolere ediyor).
#define I2C_ADDR_FT3168     0x38   // yok (CST816S 0x15 — Faz 2)
#define I2C_ADDR_ICM42670   0x68   // yok (QMI8658 0x6B — Faz 3'te bakılacak)
#define I2C_ADDR_AXP2101    0x34   // yok (PMIC'siz board)
#define I2C_ADDR_PCF85063   0x51   // yok (RTC'siz board)
#define I2C_ADDR_CST816S    0x15   // Faz 2: dokunmatik driver bunu kullanacak

// ─── Touch (CST816S) ─────────────────────────────────────────────────────────
#define PIN_TOUCH_RST       13
#define PIN_TOUCH_INT        5

// ─── IMU / PMIC IRQ ──────────────────────────────────────────────────────────
#define PIN_IMU_INT1        -1
#define PIN_PMIC_IRQ        -1

// ─── Buttons ─────────────────────────────────────────────────────────────────
#define PIN_BTN_TOP          0   // BOOT butonu (active LOW)
#define PIN_BTN_BOTTOM      16   // Fiziksel buton YOK — boş GPIO, idle HIGH

// ─── Battery ADC (200K/100K divider — Faz 3'te power varyantı okuyacak) ──────
#define PIN_BAT_ADC          1

// ─── TF Card — bu board'da yok ───────────────────────────────────────────────
#define PIN_SD_D0           -1
#define PIN_SD_CMD          -1
#define PIN_SD_CLK          -1

// ─── Buzzer — board'da yok; GPIO21 boşta, LEDC zararsız ──────────────────────
#ifndef PIN_BUZZER
#define PIN_BUZZER          21
#endif
