#pragma once
#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t x, y;
    bool     pressed;
} ft3168_point_t;

esp_err_t ft3168_init(void);
esp_err_t ft3168_read(ft3168_point_t* out);

/**
 * Touch IC'yi düşük güç moduna al (Hibernate / Deep Sleep).
 * FT3168 Power Mode register'ı (0xA5) = 0x03 → tarama durur, tüketim
 * ~5-10 mA'dan ~5 µA'ya iner.
 *
 * Wake: ft3168_init() RESET pin'ini toggle ederek chip'i yeniden başlatır.
 * Sync wake'te (task_ui spawn edilmemiş) chip Hibernate'te kalmaya devam eder
 * → ALDO1'i boşa harcamayız.
 *
 * Saat deep sleep prep'inde icm42670_suspend ile birlikte çağrılır.
 */
esp_err_t ft3168_suspend(void);
