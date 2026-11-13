#pragma once
/**
 * @brief Paylaşımlı I2C bus başlatma ve erişim katmanı.
 *
 * FT3168 (UI task / Core 0) ve ICM-42670-P / AXP2101 / PCF85063 (Sensor+Power task / Core 1)
 * aynı I2C_NUM_0 bus'ını kullanır. ESP-IDF legacy i2c_master_write_to_device()
 * dahili semaphore ile bus erişimini serialize eder, ancak bu davranış
 * dokümantasyonda garanti edilmez. Güvenli taraf: ortak mutex.
 *
 * Tüm driver'lar doğrudan i2c_master_write_read_device() yerine
 * i2c_bus_write() / i2c_bus_read() kullanmalıdır.
 */

#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>

/**
 * I2C master bus'ı başlat (bir kez, app_main'den çağrılır).
 * SDA=PIN_I2C_SDA, SCL=PIN_I2C_SCL, 400kHz
 */
esp_err_t i2c_bus_init(void);

/**
 * Mutex korumalı I2C yazma.
 * @param addr   7-bit cihaz adresi
 * @param data   gönderilecek veri tamponu
 * @param len    veri uzunluğu
 */
esp_err_t i2c_bus_write(uint8_t addr, const uint8_t* data, size_t len);

/**
 * Mutex korumalı I2C register okuma.
 * @param addr      7-bit cihaz adresi
 * @param reg       register adresi
 * @param out_buf   okunacak veri tamponu
 * @param len       okunacak byte sayısı
 */
esp_err_t i2c_bus_read_reg(uint8_t addr, uint8_t reg, uint8_t* out_buf, size_t len);

/**
 * Mutex korumalı I2C register yazma (tek byte).
 */
esp_err_t i2c_bus_write_reg(uint8_t addr, uint8_t reg, uint8_t val);

/**
 * Mutex korumalı I2C register oku-değiştir-yaz (RMW).
 * @param mask   değiştirilecek bitler
 * @param value  yeni bit değerleri (mask ile maskeli)
 */
esp_err_t i2c_bus_rmw_reg(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t value);

/* Teşhis: hattı başka görevlere kapat / aç (ped testi SDA/SCL'yi GPIO olarak sürerken). */
void i2c_bus_lock(void);
void i2c_bus_unlock(void);
/* SDA/SCL gpio_reset_pin ile bozulduysa I2C çevre birimine yeniden bağla. */
esp_err_t i2c_bus_reattach_pins(void);
