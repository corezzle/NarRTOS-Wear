/**
 * @file icm42670_driver.cpp
 * @brief ICM-42670-P IMU driver (I2C, 6-eksen)
 *
 * Adım Sayar Stratejisi:
 *  ICM-42670-P'nin donanım pedometre motoru kullanılır (APEX — Advanced Pedometer
 *  EXtension). Bu sayede CPU hiç adım algoritması çalıştırmaz; sadece INT2
 *  kesmesi geldiğinde veya polling ile adım sayısını okur.
 *
 *  Güç profili (sadece adım sayar modu):
 *    Accel LP mode @ 50Hz + APEX = ~25µA
 *    Gyro kapalı                 = 0µA
 *    WOM aktif (arka plan)       = +2.5µA
 *
 * WOM (Wake-on-Motion):
 *  Akselerometre delta-g değeri threshold'u aşınca INT1 tetiklenir.
 *  Wrist-up hareketi için 150mg threshold, 100ms debounce.
 *  ISR → event_bus_publish_from_isr(EVENT_GESTURE_WRIST_UP)
 *
 * Register map: ICM-42670-P datasheet Rev 1.3
 *  Banka değiştirme: MREG1/2/3 üzerinden dolaylı erişim.
 */

#include "icm42670_driver.h"
#include "../../core/os_config.h"
#include "../../core/event_bus.h"
#include "../../hal/i2c_bus.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <esp_rom_sys.h>
#include <esp_system.h>   // esp_reset_reason: derin uykudan sıcak kurulum

static const char* TAG = "ICM42670";
static bool s_imu_available = false;

// ─── Register Map (Banka 0 — doğrudan erişim) ────────────────────────────────
#define REG_MCLK_RDY            0x00  // bit3 = MCLK_RDY
#define REG_DEVICE_CONFIG       0x01  // SPI ayarı (reset DEĞİL — yazmak I2C'yi kilitliyor, bkz. init)
#define REG_SIGNAL_PATH_RESET   0x02  // bit4 = SOFT_RESET_DEVICE_CONFIG
#define REG_INT_SOURCE0         0x2B  // reset 0x10: RESET_DONE → INT1 (biz kapatıyoruz)
#define REG_INT_SOURCE1         0x2C  // bit3 SMD, bit2:0 = WOM_Z/Y/X → INT1
#define REG_APEX_DATA0          0x31  // adım sayısı düşük bayt
#define REG_APEX_DATA1          0x32  // adım sayısı yüksek bayt
#define REG_APEX_DATA2          0x33  // adım kadansı (u6.2)
#define REG_APEX_DATA3          0x34  // bit2 DMP_IDLE, bit1:0 ACTIVITY_CLASS
#define REG_APEX_DATA4          0x1D  // FF_DUR[7:0]
#define REG_APEX_DATA5          0x1E  // FF_DUR[15:8]
#define REG_INT_CONFIG          0x06
#define REG_FIFO_CONFIG         0x16
#define REG_TEMP_DATA1          0x09
#define REG_TEMP_DATA0          0x0A
#define REG_ACCEL_DATA_X1       0x0B
#define REG_ACCEL_DATA_X0       0x0C
#define REG_ACCEL_DATA_Y1       0x0D
#define REG_ACCEL_DATA_Y0       0x0E
#define REG_ACCEL_DATA_Z1       0x0F
#define REG_ACCEL_DATA_Z0       0x10
#define REG_GYRO_DATA_X1        0x11
#define REG_GYRO_DATA_X0        0x12
#define REG_GYRO_DATA_Y1        0x13
#define REG_GYRO_DATA_Y0        0x14
#define REG_GYRO_DATA_Z1        0x15
#define REG_GYRO_DATA_Z0        0x16
/* 2026-11-04: DS-000451 rev 1.2 ile düzeltildi. Eski tanımlar 0x2D/0x2E/0x2F idi; o adresler
 * bank0'da INT_SOURCE3/INT_SOURCE4 (ve MREG1'de INT_SOURCE6). Status okununca temizlenir (R/C). */
#define REG_INT_STATUS          0x3A  // bit? ST/FSYNC/PLL_RDY/RESET_DONE/DRDY
#define REG_INT_STATUS2         0x3B  // bit3 SMD, bit2 WOM_X, bit1 WOM_Y, bit0 WOM_Z
#define REG_INT_STATUS3         0x3C  // bit5 STEP_DET, bit4 STEP_CNT_OVF, bit3 TILT, bit2 FF, bit1 LOWG
#define REG_PWR_MGMT0           0x1F
#define REG_GYRO_CONFIG0        0x20
#define REG_ACCEL_CONFIG0       0x21
#define REG_GYRO_CONFIG1        0x23
#define REG_ACCEL_CONFIG1       0x24
#define REG_APEX_CONFIG0        0x25  // APEX (Pedometer/WOM) config
#define REG_APEX_CONFIG1        0x26
#define REG_WOM_CONFIG          0x27
#define REG_FIFO_CONFIG1        0x28
#define REG_WHO_AM_I            0x75

// MREG1 (dolaylı erişim bankası 1)
#define REG_BLK_SEL_W           0x79
#define REG_MADDR_W             0x7A
#define REG_M_W                 0x7B
#define REG_BLK_SEL_R           0x7C
#define REG_MADDR_R             0x7D
#define REG_M_R                 0x7E

// MREG1 adresleri (TDK inv_imu_regmap_rev_a.h ile doğrulandı — Session 46n)
#define MREG1_INT_SOURCE6       0x2F  // bit7 FF, bit6 LOWG, bit5 STEP_DET, bit4 STEP_CNT_OFL, bit3 TILT → INT1
#define MREG1_APEX_CONFIG2      0x44
#define MREG1_APEX_CONFIG3      0x45
#define MREG1_APEX_CONFIG5      0x47  // bit7:6 TILT_WAIT_TIME_SEL (0/2/4/6 s), 5:3 LOWG hist, 2:0 HIGHG hist
#define MREG1_APEX_CONFIG9      0x48  // bit7:4 FF debounce, 3:1 SMD_SENSITIVITY_SEL, bit0 SENSITIVITY_MODE
#define MREG1_APEX_CONFIG10     0x49  // bit7:3 LOWG_PEAK_TH_SEL, 2:0 LOWG_TIME_TH_SEL
#define MREG1_APEX_CONFIG11     0x4A  // bit7:3 HIGHG_PEAK_TH_SEL, 2:0 HIGHG_TIME_TH_SEL
#define MREG1_APEX_CONFIG12     0x67  // bit7:4 FF_MAX_DURATION_SEL, 3:0 FF_MIN_DURATION_SEL
#define MREG1_ACCEL_WOM_X_THR   0x4B
#define MREG1_ACCEL_WOM_Y_THR   0x4C
#define MREG1_ACCEL_WOM_Z_THR   0x4D

// APEX_CONFIG0 / APEX_CONFIG1 bitleri (rev 1.2)
#define APEX0_DMP_MEM_RESET_EN  (1 << 0)
#define APEX0_DMP_INIT_EN       (1 << 2)
#define APEX0_DMP_POWER_SAVE_EN (1 << 3)     // reset değeri 0x08: açık
#define APEX1_PED_ENABLE        (1 << 3)
#define APEX1_TILT_ENABLE       (1 << 4)
#define APEX1_FF_ENABLE         (1 << 5)
#define APEX1_SMD_ENABLE        (1 << 6)
#define APEX1_DMP_ODR_25HZ      0x00
#define APEX1_DMP_ODR_50HZ      0x02
#define INT1_WOM_XYZ            0x07
#define INT1_SMD                (1 << 3)
#define IS2_SMD                 (1 << 3)
#define IS3_STEP_DET            (1 << 5)
#define IS3_STEP_OVF            (1 << 4)
#define IS3_TILT                (1 << 3)
#define IS3_FF                  (1 << 2)
#define IS3_LOWG                (1 << 1)

// PWR_MGMT0 bitleri
#define PWR_IDLE                (1 << 4)     // RC osilatör açık kalır → MREG erişimi için şart
#define PWR_GYRO_MODE_OFF       0x00
#define PWR_GYRO_MODE_STBY      (0x01 << 2)
#define PWR_GYRO_MODE_LN        (0x03 << 2)  // Low Noise
#define PWR_ACCEL_MODE_OFF      0x00
#define PWR_ACCEL_MODE_LP       (0x02)       // Low Power
#define PWR_ACCEL_MODE_LN       (0x03)       // Low Noise

// WHO_AM_I beklenen değer
#define ICM42670_WHO_AM_I_VAL   0x67

// ─── I2C Yardımcıları (i2c_bus HAL üzerinden) ────────────────────────────────

#define imc_wr(reg, val)       i2c_bus_write_reg(I2C_ADDR_ICM42670, (reg), (val))
#define imc_rd(reg, buf, len)  i2c_bus_read_reg(I2C_ADDR_ICM42670, (reg), (buf), (len))

// ─── MREG1 Dolaylı Erişim ─────────────────────────────────────────────────────

/* MREG erişimi için çipin iç saati (MCLK) açık olmalı: PWR_MGMT0.IDLE=1 yap,
 * MCLK_RDY'yi bekle, işi bitince PWR_MGMT0'ı eski haline döndür. Saat kapalıyken
 * (accel LP modunda kapalıdır) MREG'e dokunmak çipi kilitleyebilir. */
static esp_err_t mclk_on(uint8_t* saved_pwr)
{
    esp_err_t ret = imc_rd(REG_PWR_MGMT0, saved_pwr, 1);
    if (ret != ESP_OK) return ret;
    ret = imc_wr(REG_PWR_MGMT0, (uint8_t)(*saved_pwr | PWR_IDLE));
    if (ret != ESP_OK) return ret;
    for (int i = 0; i < 20; i++) {
        uint8_t rdy = 0;
        if (imc_rd(REG_MCLK_RDY, &rdy, 1) == ESP_OK && (rdy & 0x08)) return ESP_OK;
        vTaskDelay(1);
    }
    imc_wr(REG_PWR_MGMT0, *saved_pwr);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t mreg1_read(uint8_t mreg_addr, uint8_t* out)
{
    uint8_t pwr = 0;
    esp_err_t ret = mclk_on(&pwr);
    if (ret != ESP_OK) return ret;
    ret = imc_wr(REG_BLK_SEL_R, 0x00);
    if (ret == ESP_OK) ret = imc_wr(REG_MADDR_R, mreg_addr);
    esp_rom_delay_us(20);   // datasheet: en az 10 us
    if (ret == ESP_OK) ret = imc_rd(REG_M_R, out, 1);
    esp_rom_delay_us(20);
    imc_wr(REG_PWR_MGMT0, pwr);
    return ret;
}

static esp_err_t mreg1_write(uint8_t mreg_addr, uint8_t val)
{
    uint8_t pwr = 0;
    esp_err_t ret = mclk_on(&pwr);
    if (ret != ESP_OK) return ret;
    ret = imc_wr(REG_BLK_SEL_W, 0x00);
    if (ret == ESP_OK) ret = imc_wr(REG_MADDR_W, mreg_addr);
    if (ret == ESP_OK) ret = imc_wr(REG_M_W, val);
    esp_rom_delay_us(20);   // datasheet: en az 10 us
    imc_wr(REG_PWR_MGMT0, pwr);
    return ret;
}

static esp_err_t mreg1_rmw(uint8_t addr, uint8_t mask, uint8_t val);   // aşağıda tanımlı

// ─── WOM ISR ─────────────────────────────────────────────────────────────────

static volatile bool      s_int_pending = false;
static uint32_t           s_apex_mask   = 0;       // açık APEX özellikleri (imu_apex_t)
static bool               s_int_wired   = false;
static uint8_t            s_dmp_odr     = 0x02;    // APEX1_DMP_ODR_* (50 Hz normal, 25 Hz uykuda)
static imu_apex_events_t  s_wake_ev;               // derin uykudan uyanışta yakalanan
static bool               s_wake_ev_valid = false;

static void IRAM_ATTR imu_int1_isr(void* arg)
{
    /* INT1 kenarı: hangi olay olduğu görev bağlamında INT_STATUS2/3 okunarak anlaşılır
     * (I²C ISR'de yapılmaz). sensor_service icm42670_int_take() ile alır. */
    s_int_pending = true;
    event_bus_publish_from_isr(EVENT_IMU_INT, 0);
}

static void decode_status(uint8_t s2, uint8_t s3, imu_apex_events_t* o)
{
    memset(o, 0, sizeof(*o));
    o->smd      = s2 & IS2_SMD;
    o->wom_axes = s2 & INT1_WOM_XYZ;
    o->wom      = o->wom_axes != 0;
    o->step     = s3 & IS3_STEP_DET;
    o->step_ovf = s3 & IS3_STEP_OVF;
    o->tilt     = s3 & IS3_TILT;
    o->ff       = s3 & IS3_FF;
    o->lowg     = s3 & IS3_LOWG;
}

/* 2026-11-06: DMP bellek sıfırlama + başlatma, TDK inv_imu_reset_dmp/resume_dmp ile aynı sıra.
 * MEM_RESET MCLK açıkken yazılır ve bitin donanımca temizlenmesi beklenir; saat kapalıyken
 * (accel LP) yazılırsa SRAM temizlenmez, DMP "boşta" kalır ve STEP_CNT_OVF her 50 ms tetiklenir
 * (NAR kart 3'te görüldü). DMP_INIT_EN de bit temizlenene kadar beklenir (≤50 ms). */
static esp_err_t dmp_mem_reset(uint8_t keep_bits)
{
    uint8_t pwr = 0;
    esp_err_t ret = mclk_on(&pwr);
    if (ret != ESP_OK) return ret;
    ret = imc_wr(REG_APEX_CONFIG0, (uint8_t)(keep_bits | APEX0_DMP_MEM_RESET_EN));
    vTaskDelay(pdMS_TO_TICKS(2));          // datasheet/TDK: 1 ms
    bool done = false;
    for (int i = 0; ret == ESP_OK && i < 50; i++) {
        uint8_t v = 0;
        if (imc_rd(REG_APEX_CONFIG0, &v, 1) == ESP_OK && !(v & APEX0_DMP_MEM_RESET_EN)) { done = true; break; }
        vTaskDelay(1);
    }
    imc_wr(REG_PWR_MGMT0, pwr);
    if (ret == ESP_OK && !done) { ESP_LOGW(TAG, "DMP bellek sifirlama bitmedi"); ret = ESP_ERR_TIMEOUT; }
    return ret;
}

static esp_err_t dmp_resume(uint8_t keep_bits)
{
    esp_err_t ret = imc_wr(REG_APEX_CONFIG0, (uint8_t)(keep_bits | APEX0_DMP_INIT_EN));
    bool done = false;
    for (int i = 0; ret == ESP_OK && i < 60; i++) {
        uint8_t v = 0;
        if (imc_rd(REG_APEX_CONFIG0, &v, 1) == ESP_OK && !(v & APEX0_DMP_INIT_EN)) { done = true; break; }
        vTaskDelay(1);
    }
    if (ret == ESP_OK && !done) { ESP_LOGW(TAG, "DMP_INIT_EN temizlenmedi (50 ms)"); ret = ESP_ERR_TIMEOUT; }
    return ret;
}

/* Olayları yönlendirme + APEX_CONFIG1 yazımı. s_apex_mask'a göre; DMP init'ten sonra çağrılır. */
static esp_err_t apply_apex(void)
{
    uint8_t c1 = s_dmp_odr;
    if (s_apex_mask & IMU_APEX_PED)  c1 |= APEX1_PED_ENABLE;
    if (s_apex_mask & IMU_APEX_TILT) c1 |= APEX1_TILT_ENABLE;
    if (s_apex_mask & IMU_APEX_SMD)  c1 |= APEX1_SMD_ENABLE | APEX1_PED_ENABLE;  // SMD pedometre ister
    if (s_apex_mask & (IMU_APEX_FF | IMU_APEX_LOWG)) c1 |= APEX1_FF_ENABLE;      // low-g = FF motorunun parçası
    esp_err_t ret = imc_wr(REG_APEX_CONFIG1, c1);
    if (ret != ESP_OK) return ret;
    /* Low-g'nin ayrı enable biti yok (rev 1.2); FF motoru + INT_SOURCE6 LOWG yönlendirmesi. */
    uint8_t src6 = 0;
    if (s_int_wired) {
        if (s_apex_mask & IMU_APEX_PED)  src6 |= (1 << 5) | (1 << 4);
        if (s_apex_mask & IMU_APEX_TILT) src6 |= (1 << 3);
        if (s_apex_mask & IMU_APEX_FF)   src6 |= (1 << 7);
        if (s_apex_mask & IMU_APEX_LOWG) src6 |= (1 << 6);
    }
    ret = mreg1_write(MREG1_INT_SOURCE6, src6);
    uint8_t src1 = 0;
    if (s_int_wired && (s_apex_mask & IMU_APEX_WOM)) src1 |= INT1_WOM_XYZ;
    if (s_int_wired && (s_apex_mask & IMU_APEX_SMD)) src1 |= INT1_SMD;
    if (ret == ESP_OK) ret = imc_wr(REG_INT_SOURCE1, src1);
    /* WOM_EN bit0: açık/kapalı; WOM_MODE bit1 (önceki örnekle karşılaştır) */
    if (ret == ESP_OK) ret = imc_wr(REG_WOM_CONFIG, (s_apex_mask & IMU_APEX_WOM) ? 0x03 : 0x00);
    /* 2026-11-06: DMP güç tasarrufu açıkken DMP uyur ve yalnız WOM uyandırır. WOM kapalıysa
     * tilt/pedometre/FF hiç çalışmaz (T3 eğim testi: 0 olay, dmp=bosta). Güç tasarrufu yalnız
     * WOM açıkken anlamlı; NAR'da bit WOM seçimine bağlanır (saat kartlarında dokunulmaz). */
    if (ret == ESP_OK && s_int_wired) {
        uint8_t c0 = 0;
        ret = imc_rd(REG_APEX_CONFIG0, &c0, 1);
        if (ret == ESP_OK) {
            uint8_t want = (uint8_t)((c0 & ~APEX0_DMP_POWER_SAVE_EN) |
                                     ((s_apex_mask & IMU_APEX_WOM) ? APEX0_DMP_POWER_SAVE_EN : 0));
            if (want != c0) ret = imc_wr(REG_APEX_CONFIG0, want);
            /* TDK inv_imu_apex_enable_*: her özellik değişiminde DMP yeniden başlatılır
             * (DMP_INIT_EN, bit temizlenene kadar bekle). Güç tasarrufu kapatılınca DMP'nin
             * uykudan çıkıp sürekli çalışması için de bu gerekiyor. */
            if (ret == ESP_OK) ret = dmp_resume((uint8_t)(want & APEX0_DMP_POWER_SAVE_EN));
        }
    }
    return ret;
}

// ─── icm42670_init ───────────────────────────────────────────────────────────

esp_err_t icm42670_init(void)
{
    ESP_LOGI(TAG, "Initializing ICM-42670-P...");

    // WHO_AM_I doğrula
    uint8_t who = 0;
    esp_err_t ret = imc_rd(REG_WHO_AM_I, &who, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ICM42670 I2C read failed (err=0x%x) — not present on this board?", ret);
        return ret;
    }
    if (who != ICM42670_WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "WHO_AM_I mismatch: got 0x%02X, expected 0x%02X", who, ICM42670_WHO_AM_I_VAL);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "WHO_AM_I OK (0x%02X)", who);

    s_int_wired = (PIN_IMU_INT1 >= 0);
    if (s_apex_mask == 0) s_apex_mask = IMU_APEX_PED | (s_int_wired ? IMU_APEX_WOM : 0);

    /* 2026-11-04: Derin uykudan uyanış (NAR, INT1 → GPIO21). Çip uyku boyunca çalışıyordu
     * (icm42670_arm_for_sleep): önce uyandıran olayı INT_STATUS2/3'ten al (soft reset silerdi),
     * sonra SICAK kurulum — reset ve DMP init yok, adım sayısı ve APEX durumu korunur. */
    bool warm = false;
    s_wake_ev_valid = false;
    if (s_int_wired && esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        uint8_t pwr = 0, s2 = 0, s3 = 0;
        if (imc_rd(REG_PWR_MGMT0, &pwr, 1) == ESP_OK && (pwr & 0x03) != PWR_ACCEL_MODE_OFF) {
            imc_rd(REG_INT_STATUS2, &s2, 1);
            imc_rd(REG_INT_STATUS3, &s3, 1);
            decode_status(s2, s3, &s_wake_ev);
            s_wake_ev_valid = (s2 | s3) != 0;
            warm = true;
            ESP_LOGI(TAG, "sicak kurulum (uyanis): INT_STATUS2=%02X INT_STATUS3=%02X", s2, s3);
        }
    }

    if (!warm) {
        /* Soft reset — SIGNAL_PATH_RESET bit4. (Eski kod DEVICE_CONFIG 0x01'e 0x01
         * yazıyordu: o register SPI ayarı; yazınca çip I2C'den düşüyor ve besleme
         * tamamen kesilene kadar geri gelmiyordu — NAR kartında adım adım doğrulandı.) */
        ret = imc_wr(REG_SIGNAL_PATH_RESET, 0x10);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "soft reset yazilamadi (0x%x)", ret); return ret; }
        vTaskDelay(pdMS_TO_TICKS(5));  // datasheet: 1 ms
        if (s_int_wired) {
            /* 2026-11-06: INT_SOURCE0 reset degeri 0x10 = RESET_DONE → INT1. Status (0x3A) hic
             * okunmadigi icin latched INT1 surekli dusuk kaliyor ve her INT_STATUS2/3 okumasindan
             * sonra yeniden tetikleniyordu (50 ms'de bir "ADIM-TASMA" firtinasi). TDK surucusu gibi
             * RESET_DONE'i okuyarak temizle, INT1'e yalnizca APEX/WOM olaylarini yonlendir. */
            uint8_t st = 0;
            imc_rd(REG_INT_STATUS, &st, 1);
            ret = imc_wr(REG_INT_SOURCE0, 0x00);
            if (ret != ESP_OK) { ESP_LOGE(TAG, "INT_SOURCE0 yazilamadi (0x%x)", ret); return ret; }
            ESP_LOGI(TAG, "reset sonrasi INT_STATUS=%02X temizlendi, INT_SOURCE0=0", st);
        }
    }

    // Accel: Low Power mode, 50Hz ODR, ±4g FSR
    // ACCEL_CONFIG0[6:5] = FSR 01 (±4g), [3:0] = ODR 1010 (50Hz)
    ret = imc_wr(REG_ACCEL_CONFIG0, (0x01 << 5) | 0x0A);
    // PWR_MGMT0: Accel LP mode aktif, Gyro kapalı
    if (ret == ESP_OK) ret = imc_wr(REG_PWR_MGMT0, PWR_ACCEL_MODE_LP | PWR_GYRO_MODE_OFF);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "accel yapilandirilamadi (0x%x)", ret); return ret; }
    vTaskDelay(pdMS_TO_TICKS(1));

    esp_err_t ax = ESP_OK;
    if (!warm) {
        /* APEX DMP (parametreler MREG1 APEX_CONFIG2..12 varsayılanında):
         * DMP belleğini sıfırla → DMP'yi başlat (güç tasarrufu biti korunur, reset değeri).
         * Hata IMU'yu devre dışı bırakmaz — ivme okuması yine çalışır. */
        /* DMP güç tasarrufu yalnız INT'li kartta (NAR, uykuda çalışacak); saat kartlarında eski
         * davranış (bit temiz) aynen kalsın — AMOLED derlemesi değişmez kuralı. */
        const uint8_t ps = (s_int_wired && (s_apex_mask & IMU_APEX_WOM)) ? APEX0_DMP_POWER_SAVE_EN : 0;
        if (s_int_wired) {
            /* NAR: MCLK açıkken bellek sıfırla, bitleri bekle (TDK sırası). */
            ax = dmp_mem_reset(ps);
            if (ax == ESP_OK) ax = dmp_resume(ps);
        } else {
            /* Saat kartları: eski davranış aynen (AMOLED derlemesi değişmez). */
            ax = imc_wr(REG_APEX_CONFIG0, (uint8_t)(ps | APEX0_DMP_MEM_RESET_EN));
            vTaskDelay(pdMS_TO_TICKS(2));
            if (ax == ESP_OK) ax = imc_wr(REG_APEX_CONFIG0, (uint8_t)(ps | APEX0_DMP_INIT_EN));
            vTaskDelay(pdMS_TO_TICKS(50));  // DMP init süresi
        }
    }
    s_dmp_odr = APEX1_DMP_ODR_50HZ;
    if (ax == ESP_OK && s_int_wired && !warm) {
        /* 2026-11-06: Düşme/low-g eşikleri. Çipin register varsayılanı low-g 31 mg / high-g 250 mg:
         * elle bırakılan, kablolu, dönen kartta birleşik ivme 31 mg altına inmiyor → DUSME hiç gelmedi
         * (T4). TDK inv_imu_apex_init_parameters_struct varsayılanları: low-g 563 mg (hist 156),
         * high-g 2500 mg (hist 156), 1'er örnek, FF 10-204 cm, debounce 2000 ms, tilt bekleme 4 s. */
        esp_err_t pr = mreg1_write(MREG1_APEX_CONFIG10, (uint8_t)((17 << 3) | 0));   // low-g 563 mg, 1 örnek
        if (pr == ESP_OK) pr = mreg1_write(MREG1_APEX_CONFIG11, (uint8_t)((9 << 3) | 0));   // high-g 2500 mg, 1 örnek
        if (pr == ESP_OK) pr = mreg1_write(MREG1_APEX_CONFIG5,  (uint8_t)((2 << 6) | (4 << 3) | 4)); // tilt 4 s, hist 156/156 mg
        if (pr == ESP_OK) pr = mreg1_write(MREG1_APEX_CONFIG12, (uint8_t)((5 << 4) | 0));   // FF 10..204 cm
        if (pr == ESP_OK) pr = mreg1_rmw(MREG1_APEX_CONFIG9, 0xF0, (uint8_t)(7 << 4));      // FF debounce 2000 ms
        if (pr != ESP_OK) ESP_LOGW(TAG, "APEX dusme/low-g esikleri yazilamadi (0x%x)", pr);
        else ESP_LOGI(TAG, "APEX esikleri (TDK): low-g 563 mg, high-g 2500 mg, FF 10-204 cm, debounce 2 s");
    }
    if (ax == ESP_OK) ax = apply_apex();
    if (ax != ESP_OK) ESP_LOGW(TAG, "APEX kurulamadi (0x%x) — adim sayaci/olaylar calismaz", ax);

    // INT1 pini — yalnızca kablolu ise (NAR: GPIO21). Değilse her şey polling.
    if (s_int_wired) {
        gpio_config_t int_cfg = {
            .pin_bit_mask = (1ULL << PIN_IMU_INT1),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&int_cfg);
        gpio_install_isr_service(0);           // zaten kuruluysa INVALID_STATE, sorun değil
        gpio_isr_handler_add((gpio_num_t)PIN_IMU_INT1, imu_int1_isr, NULL);
        icm42670_int1_config(true);            // aktif-düşük, push-pull, latched
        if ((s_apex_mask & IMU_APEX_WOM) && !warm &&
            icm42670_enable_wom(150) != ESP_OK) ESP_LOGW(TAG, "WOM kurulamadi — wrist-up calismaz");
    } else {
        ESP_LOGW(TAG, "PIN_IMU_INT1 not wired — WOM/wrist-up disabled");
    }

    s_imu_available = true;
    ESP_LOGI(TAG, "ICM-42670-P init OK (%s, Accel LP 50Hz, APEX mask 0x%02lX, INT1 %s)",
             warm ? "sicak" : "soguk", (unsigned long)s_apex_mask, s_int_wired ? "GPIO" : "yok");
    return ESP_OK;
}

// ─── icm42670_read ───────────────────────────────────────────────────────────

esp_err_t icm42670_read(icm42670_data_t* out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_imu_available) return ESP_ERR_NOT_SUPPORTED;

    uint8_t raw[14];  // TEMP(2) + ACCEL(6) + GYRO(6)
    esp_err_t rr = imc_rd(REG_TEMP_DATA1, raw, sizeof(raw));
    if (rr != ESP_OK) return rr;   // I2C hatası reboot sebebi olmasın

    // Sıcaklık (ham)
    out->temp_raw = (int16_t)((raw[0] << 8) | raw[1]);

    // Akselerometre — ±4g, sensitivity = 8192 LSB/g
    const float ACCEL_SENS = 8192.0f;
    out->accel_x = (float)((int16_t)((raw[2]  << 8) | raw[3]))  / ACCEL_SENS;
    out->accel_y = (float)((int16_t)((raw[4]  << 8) | raw[5]))  / ACCEL_SENS;
    out->accel_z = (float)((int16_t)((raw[6]  << 8) | raw[7]))  / ACCEL_SENS;

    // Jiroskop — ±2000dps (varsayılan), sensitivity = 16.4 LSB/dps
    const float GYRO_SENS = 16.4f;
    out->gyro_x  = (float)((int16_t)((raw[8]  << 8) | raw[9]))  / GYRO_SENS;
    out->gyro_y  = (float)((int16_t)((raw[10] << 8) | raw[11])) / GYRO_SENS;
    out->gyro_z  = (float)((int16_t)((raw[12] << 8) | raw[13])) / GYRO_SENS;

    return ESP_OK;
}

// ─── icm42670_get_steps ──────────────────────────────────────────────────────

esp_err_t icm42670_get_steps(uint32_t* out_steps)
{
    if (!out_steps) return ESP_ERR_INVALID_ARG;
    if (!s_imu_available) return ESP_ERR_NOT_SUPPORTED;

    // Donanım adım sayacı: APEX_DATA0 (düşük) / APEX_DATA1 (yüksek), 16-bit
    uint8_t d[2] = {0, 0};
    esp_err_t ret = imc_rd(REG_APEX_DATA0, d, 2);
    if (ret != ESP_OK) return ret;
    uint8_t lo = d[0], hi = d[1];

    *out_steps = ((uint32_t)hi << 8) | lo;
    return ESP_OK;
}

// ─── icm42670_enable_wom ─────────────────────────────────────────────────────

esp_err_t icm42670_enable_wom(uint16_t threshold_mg)
{
    // WOM threshold: 1 LSB = ~3.9mg, clamp to 8-bit
    uint8_t thr = (uint8_t)((threshold_mg * 256) / 1000);
    if (thr == 0) thr = 1;

    // Eşikler MREG1'de, eksen başına (1 LSB = 1g/256 ≈ 3.9 mg)
    esp_err_t ret = mreg1_write(MREG1_ACCEL_WOM_X_THR, thr);
    if (ret == ESP_OK) ret = mreg1_write(MREG1_ACCEL_WOM_Y_THR, thr);
    if (ret == ESP_OK) ret = mreg1_write(MREG1_ACCEL_WOM_Z_THR, thr);
    if (ret != ESP_OK) { ESP_LOGW(TAG, "WOM esigi yazilamadi (0x%x)", ret); return ret; }

    /* Yönlendirme (INT_SOURCE1) ve WOM_CONFIG (bit0 EN, bit1 MODE=önceki örnekle karşılaştır,
     * bit2 OR) apply_apex içinde, s_apex_mask'a göre. INT1 polaritesi icm42670_int1_config'de. */
    s_apex_mask |= IMU_APEX_WOM;
    ret = apply_apex();
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "WOM enabled: threshold=%dmg (reg=0x%02X)", threshold_mg, thr);
    return ESP_OK;
}

// ─── APEX API (2026-11-04) ───────────────────────────────────────────────────

esp_err_t icm42670_apex_enable(uint32_t mask, bool route_int1)
{
    if (!s_imu_available) return ESP_ERR_NOT_SUPPORTED;
    s_apex_mask = mask;
    s_int_wired = route_int1 && (PIN_IMU_INT1 >= 0);
    return apply_apex();
}

uint32_t icm42670_apex_mask(void) { return s_apex_mask; }

esp_err_t icm42670_apex_read(imu_apex_data_t* out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_imu_available) return ESP_ERR_NOT_SUPPORTED;
    uint8_t d[4] = {0};
    esp_err_t ret = imc_rd(REG_APEX_DATA0, d, 4);   // 0x31..0x34 ardışık
    if (ret != ESP_OK) return ret;
    out->steps       = (uint16_t)((d[1] << 8) | d[0]);
    out->cadence_u62 = d[2];
    out->activity    = d[3] & 0x03;
    out->dmp_idle    = (d[3] & 0x04) != 0;
    return ESP_OK;
}

esp_err_t icm42670_apex_events(imu_apex_events_t* out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_imu_available) return ESP_ERR_NOT_SUPPORTED;
    uint8_t s[2] = {0, 0};
    esp_err_t ret = imc_rd(REG_INT_STATUS2, s, 2);  // 0x3B, 0x3C; okuyunca temizlenir
    if (ret != ESP_OK) return ret;
    if (s_int_wired) { uint8_t st = 0; imc_rd(REG_INT_STATUS, &st, 1); }  // 0x3A da R/C; latched INT1 bırakılsın
    decode_status(s[0], s[1], out);
    if (out->ff) {
        uint8_t f[2] = {0, 0};
        if (imc_rd(REG_APEX_DATA4, f, 2) == ESP_OK) out->ff_dur = (uint16_t)((f[1] << 8) | f[0]);
    }
    return ESP_OK;
}

bool icm42670_int_take(void)
{
    bool p = s_int_pending;
    s_int_pending = false;
    return p;
}

int icm42670_int_level(void)
{
    return (PIN_IMU_INT1 >= 0) ? gpio_get_level((gpio_num_t)PIN_IMU_INT1) : -1;
}

esp_err_t icm42670_int1_config(bool latched)
{
    /* INT_CONFIG: bit0 polarite (0 aktif-düşük), bit1 sürücü (1 push-pull), bit2 mod (1 latched).
     * Latched: hat INT_STATUS okunana kadar düşük kalır → EXT1 (ANY_LOW) kaçırmaz. */
    return imc_wr(REG_INT_CONFIG, (uint8_t)(0x02 | (latched ? 0x04 : 0x00)));
}

static esp_err_t mreg1_rmw(uint8_t addr, uint8_t mask, uint8_t val)
{
    uint8_t cur = 0;
    esp_err_t ret = mreg1_read(addr, &cur);
    if (ret != ESP_OK) return ret;
    return mreg1_write(addr, (uint8_t)((cur & ~mask) | (val & mask)));
}

esp_err_t icm42670_set_tilt_wait(uint8_t sel)        { return mreg1_rmw(MREG1_APEX_CONFIG5, 0xC0, (uint8_t)(sel << 6)); }
esp_err_t icm42670_set_smd_sensitivity(uint8_t sel)  { return mreg1_rmw(MREG1_APEX_CONFIG9, 0x0E, (uint8_t)(sel << 1)); }
esp_err_t icm42670_set_ff_duration(uint8_t min_sel, uint8_t max_sel)
{
    return mreg1_write(MREG1_APEX_CONFIG12, (uint8_t)((max_sel << 4) | (min_sel & 0x0F)));
}
esp_err_t icm42670_set_lowg(uint8_t peak_sel, uint8_t time_sel)
{
    return mreg1_write(MREG1_APEX_CONFIG10, (uint8_t)((peak_sel << 3) | (time_sel & 0x07)));
}

esp_err_t icm42670_arm_for_sleep(void)
{
    if (!s_imu_available || !s_int_wired) return ESP_ERR_NOT_SUPPORTED;
    /* Accel LP 25 Hz (ODR kodu 1011), DMP 25 Hz; APEX seçimi aynen kalır, WOM açık. */
    esp_err_t ret = imc_wr(REG_ACCEL_CONFIG0, (0x01 << 5) | 0x0B);
    s_dmp_odr = APEX1_DMP_ODR_25HZ;
    if (ret == ESP_OK) ret = apply_apex();
    if (ret == ESP_OK) ret = icm42670_int1_config(true);
    /* Bekleyen status'u temizle; yoksa latched INT1 düşük kalır ve hemen uyanılır. */
    imu_apex_events_t ev;
    if (ret == ESP_OK) ret = icm42670_apex_events(&ev);
    s_int_pending = false;
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "uyku icin kuruldu: accel LP 25 Hz, APEX 0x%02lX, INT1 latched (GPIO%d=%d)",
                 (unsigned long)s_apex_mask, PIN_IMU_INT1, icm42670_int_level());
    return ret;
}

bool icm42670_wake_events(imu_apex_events_t* out)
{
    if (!s_wake_ev_valid) return false;
    if (out) *out = s_wake_ev;
    return true;
}

// ─── icm42670_set_gyro_enable ────────────────────────────────────────────────

esp_err_t icm42670_set_gyro_enable(bool enable)
{
    uint8_t pwr = 0;
    esp_err_t pr = imc_rd(REG_PWR_MGMT0, &pwr, 1);
    if (pr != ESP_OK) return pr;

    // Gyro mode bitlerini temizle [5:4]
    pwr &= ~(0x03 << 2);

    if (enable) {
        pwr |= PWR_GYRO_MODE_LN;   // Low Noise mode
        ESP_LOGI(TAG, "Gyro enabled (LN mode)");
    } else {
        // PWR_GYRO_MODE_OFF = 0x00, zaten temizlendi
        ESP_LOGI(TAG, "Gyro disabled");
    }

    return imc_wr(REG_PWR_MGMT0, pwr);
}

// ─── icm42670_suspend ────────────────────────────────────────────────────────

esp_err_t icm42670_suspend(void)
{
    if (!s_imu_available) return ESP_ERR_NOT_SUPPORTED;

    /* Hem Accel hem Gyro OFF → ICM-42670-P "sleep" durumuna geçer.
     * Datasheet Table 5.1: Accel OFF + Gyro OFF = ~3.5µA quiescent.
     * APEX (pedometer) accel ODR'a bağımlı olduğundan accel OFF olunca
     * otomatik durur — ek bir register temizliğine gerek yok.
     *
     * Wake sonrası icm42670_init() yeniden çağrılır (axp2101_init gibi);
     * soft-reset + ACCEL LP + APEX yeniden kurulur, step counter sıfırlanır.
     * (Adım sayısı NVS'te zaten kalıcı; `step_counter_app` boot'ta restore eder.) */
    esp_err_t ret = imc_wr(REG_PWR_MGMT0, PWR_ACCEL_MODE_OFF | PWR_GYRO_MODE_OFF);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "IMU suspended (Accel+Gyro OFF, ~3.5µA)");
    }
    return ret;
}
