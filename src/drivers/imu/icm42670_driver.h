#pragma once
/**
 * @brief ICM-42670-P IMU Driver (6-axis: Accel + Gyro)
 *
 * Interface: I2C (shared bus, addr 0x68)
 * Used for: Step counting, wrist-up gesture detection
 *
 * Strategy:
 *  - WOM (Wake-on-Motion) interrupt → wakes display on wrist movement
 *  - Pedometer: hardware step counter (INT2 on step) — reduces CPU load
 *  - Low-power accel mode at 50Hz for step counting
 *  - Gyro disabled when only step counting is needed (saves ~1mA)
 */

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

// ─── Accel Scale Options ──────────────────────────────────────────────────────
typedef enum {
    ICM42670_ACCEL_2G  = 0,
    ICM42670_ACCEL_4G  = 1,
    ICM42670_ACCEL_8G  = 2,
    ICM42670_ACCEL_16G = 3,
} icm42670_accel_fsr_t;

// ─── Gyro Scale Options ───────────────────────────────────────────────────────
typedef enum {
    ICM42670_GYRO_250DPS  = 0,
    ICM42670_GYRO_500DPS  = 1,
    ICM42670_GYRO_1000DPS = 2,
    ICM42670_GYRO_2000DPS = 3,
} icm42670_gyro_fsr_t;

// ─── IMU Data ─────────────────────────────────────────────────────────────────
typedef struct {
    float accel_x, accel_y, accel_z;   // in g
    float gyro_x,  gyro_y,  gyro_z;    // in deg/s
    int16_t temp_raw;                   // raw temperature
} icm42670_data_t;

// ─── APEX (DS-000451 rev 1.2) ────────────────────────────────────────────────
// ICM-42670-P donanım hareket motoru: pedometre, tilt, significant motion, freefall, low-g, WOM.
// (Raise-to-Wake ve Tap bu çipte YOK.) Hepsi INT1'e (NAR: GPIO21) yönlendirilir.
typedef enum {
    IMU_APEX_PED  = 1 << 0,   // adım sayma + STEP_DET kesmesi
    IMU_APEX_TILT = 1 << 1,   // 35° eğim, TILT_WAIT süre boyunca
    IMU_APEX_SMD  = 1 << 2,   // significant motion (pedometre açık ister)
    IMU_APEX_FF   = 1 << 3,   // serbest düşüş (FF_DUR ile mesafe)
    IMU_APEX_LOWG = 1 << 4,   // low-g (|a| eşiğin altında)
    IMU_APEX_WOM  = 1 << 5,   // wake-on-motion (eşik icm42670_enable_wom)
} imu_apex_t;

typedef struct {
    uint16_t steps;        // APEX_DATA0/1 (16 bit, taşar)
    uint8_t  cadence_u62;  // APEX_DATA2: adım periyodu, DMP örneği, u6.2 (50 Hz'de 100 = 2 Hz)
    uint8_t  activity;     // APEX_DATA3[1:0]: 0 bilinmiyor, 1 yürüme, 2 koşu
    bool     dmp_idle;     // APEX_DATA3[2]
} imu_apex_data_t;

typedef struct {
    bool     step, step_ovf, tilt, ff, lowg, smd, wom;
    uint8_t  wom_axes;     // bit2 X, bit1 Y, bit0 Z
    uint16_t ff_dur;       // APEX_DATA4/5 (DMP örneği); mesafe = 0,5·9,81·(ff_dur/ODR)²
} imu_apex_events_t;

/** APEX seçimi (bit maskesi); route_int1: olaylar INT1'e yönlendirilsin mi. On-the-fly yapılabilir. */
esp_err_t icm42670_apex_enable(uint32_t mask, bool route_int1);
uint32_t  icm42670_apex_mask(void);
/** Adım, kadans, aktivite sınıfı. */
esp_err_t icm42670_apex_read(imu_apex_data_t* out);
/** INT_STATUS2/3 oku (okuyunca temizlenir → latched INT1 kalkar) ve olaylara ayır. */
esp_err_t icm42670_apex_events(imu_apex_events_t* out);
/** ISR'de set edilen "INT1 geldi" bayrağını al ve temizle. */
bool      icm42670_int_take(void);
/** INT1 pini seviyesi (-1: pin yok). Aktif-düşük: 0 = kesme bekliyor. */
int       icm42670_int_level(void);
/** INT1: aktif-düşük, push-pull; latched=1 → status okunana kadar düşük kalır (EXT1 için). */
esp_err_t icm42670_int1_config(bool latched);
/** Parametreler (MREG1). sel değerleri datasheet tablolarındaki ham alanlar. */
esp_err_t icm42670_set_tilt_wait(uint8_t sel);                      // 0:0 s 1:2 s 2:4 s 3:6 s
esp_err_t icm42670_set_smd_sensitivity(uint8_t sel);                // 0..7
esp_err_t icm42670_set_ff_duration(uint8_t min_sel, uint8_t max_sel);
esp_err_t icm42670_set_lowg(uint8_t peak_sel, uint8_t time_sel);
/** Derin uyku öncesi: accel LP + seçili APEX + WOM açık, DMP güç tasarrufu, status temizle.
 *  (icm42670_suspend'in tersi: çip uykuda çalışmaya devam eder, INT1 MCU'yu uyandırır.) */
esp_err_t icm42670_arm_for_sleep(void);
/** Derin uykudan uyanışta init sırasında yakalanan olaylar (varsa). */
bool      icm42670_wake_events(imu_apex_events_t* out);

// ─── API ──────────────────────────────────────────────────────────────────────

/** Initialize IMU, configure for low-power step counting mode. */
esp_err_t icm42670_init(void);

/** Read accelerometer + gyroscope data. */
esp_err_t icm42670_read(icm42670_data_t* out);

/** Get hardware step count (resets on power cycle; use NVS for persistence). */
esp_err_t icm42670_get_steps(uint32_t* out_steps);

/**
 * Enable Wake-on-Motion interrupt.
 * Fires PIN_IMU_INT1 when accel delta > threshold (mg).
 * Typical threshold: 100–200mg for wrist-up detection.
 */
esp_err_t icm42670_enable_wom(uint16_t threshold_mg);

/** Set gyro on/off (save power when only steps needed). */
esp_err_t icm42670_set_gyro_enable(bool enable);

/**
 * Suspend IMU (deep sleep prep): both Accel and Gyro → OFF.
 * Idle quiescent ~3.5µA per datasheet (vs ~25µA in pedometer mode).
 * Used before ESP32 deep sleep to minimize ALDO1 load (PCF85063 RTC stays
 * powered on the same rail). On next boot icm42670_init() re-arms accel +
 * APEX pedometer.
 */
esp_err_t icm42670_suspend(void);
