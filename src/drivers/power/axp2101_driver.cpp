/**
 * @file axp2101_driver.cpp
 * @brief AXP2101 PMIC driver (I2C)
 *
 * Güç Mimarisi:
 *  DCDC1 (3.3V) → MCU, Flash, PSRAM              — always on
 *  DCDC2 (1.8V) → Display IOVDD                  — kapatılabilir
 *  LDO1  (3.3V) → Sensörler (IMU, RTC, Touch)    — uyku sırasında aktif (WOM için)
 *  LDO2  (3.3V) → Display AVDD                   — kapatılabilir
 *
 * Batarya yüzdesi hesabı:
 *  AXP2101 donanımsal coulomb counter içerir (REG_BAT_PERCENT).
 *  Ek olarak gerilim eşiği tablosuyla çapraz doğrulama yapılır.
 *
 * IRQ:
 *  PMIC_IRQ pini → ESP32 GPIO interrupt → event_bus'a EVENT_LOW_BATTERY veya
 *  EVENT_CHARGING_STATE yayımlanır.
 *
 * Derin Uyku (A-Modified — minimum quiescent profile):
 *  axp2101_prepare_deep_sleep() → IMU suspend → DCDC1+ALDO1 dışında tüm
 *  rayları (DCDC2-5, ALDO2-4, BLDO1-2, CPUSLDO, DLDO1) kapat → ext1 wake
 *  arm → esp_deep_sleep_start().
 *  Uyku sırasında:
 *    DCDC1 (3.3V) — ESP32-S3 RTC domain için zorunlu
 *    ALDO1 (3.3V) — PCF85063 RTC chip saat saymaya devam etsin diye
 *  IMU suspend (~3.5µA) + RTC (~250nA) + AXP2101 quiescent (~30µA) →
 *  toplam ~0.5–1mA hedef. Wake kaynağı: BTN_TOP (GPIO 0, EXT1).
 *
 * Referans: AXP2101 datasheet v1.0
 */

#include "axp2101_driver.h"
#include "../../core/os_config.h"
#include "../../core/event_bus.h"
#include "../../hal/i2c_bus.h"
#include "../imu/icm42670_driver.h"   // icm42670_suspend() — deep sleep prep
#include "../touch/ft3168_driver.h"   // ft3168_suspend() — deep sleep prep

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "AXP2101";
static bool s_available = false;
static uint8_t s_chip_id = 0;      // 0x4A: switch-charger surumu, 0x47: LINEER sarj surumu (NAR ana PMIC)  // set true only after successful init
static uint16_t s_nominal_charge_ma = 300;  // 0.3C ile hesaplanan sarj akimi; termal guard buna geri doner

// ─── Register Map ─────────────────────────────────────────────────────────────
// Chip ID / Status
#define AXP_REG_CHIP_ID         0x03   // Expected: 0x4A
#define AXP_REG_COM_STAT0       0x00   // Power status (VBUS, battery)
#define AXP_REG_COM_STAT1       0x01   // Charge status

// Interrupt
#define AXP_REG_IRQ_EN0         0x40
#define AXP_REG_IRQ_EN1         0x41
#define AXP_REG_IRQ_EN2         0x42
#define AXP_REG_IRQ_STAT0       0x48   // IRQ status (write 1 to clear)
#define AXP_REG_IRQ_STAT1       0x49
#define AXP_REG_IRQ_STAT2       0x4A

// ADC — battery
#define AXP_REG_BAT_PERCENT     0xA4   // SOC 0–100% (direct)
#define AXP_REG_VBAT_H          0x34   // Battery voltage high byte (14-bit)
#define AXP_REG_VBAT_L          0x35
#define AXP_REG_IBAT_H          0x36   // Battery current high byte (13-bit, signed)
#define AXP_REG_IBAT_L          0x37

// ADC enable / fuel gauge / battery detect
// REG 0x30 — DATASHEET DOĞRULAMA (Adım 13, 2026-05-14):
//   Önceki kod 0xCB yazıyordu — DATASHEET'TE BÖYLE BİR REGISTER YOK.
//   Section 6.13.2.30: ADC channel enable register = REG 0x30
//   Bit 0 = battery voltage ADC, Bit 1 = TS pin ADC,
//   Bit 2 = VBUS voltage, Bit 3 = system voltage, Bit 4 = die temperature
#define AXP_REG_ADC_CH_EN       0x30
#define AXP_REG_BAT_DETECT      0x68   // bit0 = battery detection enable
#define AXP_REG_FUEL_GAUGE      0x18   // bit3 = fuel gauge (SOC algoritması) enable
#define AXP_REG_TS_PIN_CTRL     0x50   // Adım 13: TS pin control (current source on/off)

// Charge control
#define AXP_REG_CHG_CTRL1       0x62   // Charge enable + current
#define AXP_REG_CHG_CTRL2       0x63   // Charge voltage

// CHGLED control (Adım 6 — 2026-05-14)
// AXP2101'in CHGLED pini bazı board'larda fiziksel bir LED'e bağlı.
// Charging stat değiştikçe LED yanıp söner → akım çeker. Disable etmek için
// REG 0x69 = 0x00: LED output manual mode + off.
#define AXP_REG_CHGLED_CTRL     0x69

// Power rail enable — correct AXP2101 register map (verified from XPowersLib + datasheet)
// DCDC enable/disable: single register 0x80 (one bit per channel)
#define AXP_REG_DCDC_ONOFF      0x80   // bit0=DC1, bit1=DC2, bit2=DC3, bit3=DC4, bit4=DC5
#define AXP_REG_DCDC1_VOL       0x82   // DCDC1 voltage: (mV-1500)/100, 3.3V = 0x12
#define AXP_REG_DCDC2_VOL       0x83   // DCDC2 voltage: dual-range, max 1540mV
#define AXP_REG_DCDC3_VOL       0x84   // DCDC3 voltage: dual-range, 1.8V = 0x5D

// ALDO/LDO enable — DATASHEET DOĞRULAMA (Adım 13, 2026-05-14):
//   Section 6.13.2.75 (REG 0x90) — primary LDO ON/OFF control:
//     bit7=DLDO1, bit6=CPUSLDO, bit5=BLDO2, bit4=BLDO1,
//     bit3=ALDO4, bit2=ALDO3, bit1=ALDO2, bit0=ALDO1
//   Section 6.13.2.76 (REG 0x91) — secondary LDO ON/OFF control:
//     bit0 = DLDO2 enable (önceki kodumuzda EKSİKTİ!)
#define AXP_REG_LDO_ONOFF       0x90   // primary LDO enable
#define AXP_REG_LDO_ONOFF1      0x91   // secondary LDO enable (DLDO2 only)
// ALDO voltage registers
#define AXP_REG_ALDO1_VOL       0x92   // (mV-500)/100 — 3.3V = 0x1C
#define AXP_REG_ALDO2_VOL       0x93   // ALDO2 = DS1_PWR_EN (display power!) — 3.3V = 0x1C
#define AXP_REG_ALDO3_VOL       0x94
#define AXP_REG_ALDO4_VOL       0x95

// Enable bit masks for AXP_REG_DCDC_ONOFF (0x80)
#define AXP_DCDC1_EN_BIT        (1 << 0)
#define AXP_DCDC2_EN_BIT        (1 << 1)
#define AXP_DCDC3_EN_BIT        (1 << 2)
#define AXP_DCDC4_EN_BIT        (1 << 3)
#define AXP_DCDC5_EN_BIT        (1 << 4)

// Enable bit masks for AXP_REG_LDO_ONOFF (0x90)
#define AXP_ALDO1_EN_BIT        (1 << 0)
#define AXP_ALDO2_EN_BIT        (1 << 1)   // DISPLAY POWER (DS1_PWR_EN)
#define AXP_ALDO3_EN_BIT        (1 << 2)
#define AXP_ALDO4_EN_BIT        (1 << 3)
#define AXP_BLDO1_EN_BIT        (1 << 4)
#define AXP_BLDO2_EN_BIT        (1 << 5)
#define AXP_CPUSLDO_EN_BIT      (1 << 6)
#define AXP_DLDO1_EN_BIT        (1 << 7)

// Power off / sleep
// REG 0x26 — DATASHEET DOĞRULAMA (Adım 13, 2026-05-14):
//   Önceki kod 0x31 yazıyordu — DATASHEET'TE 0x31 SLEEP MODE DEĞİL.
//   Section 6.13.2.24 (REG 0x26: Sleep and wakeup control):
//     bit 0 = SLEEP enable, bit 1 = Wake Up enable,
//     bit 2 = Voltage select on wakeup, bit 3 = PWROK low on wakeup,
//     bit 4 = IRQ pin low to wakeup enable
//   Önceki yazılım 0x31'e yazıyordu, AXP2101 hiç sleep mode'a girmemişti.
#define AXP_REG_SLEEP_CFG       0x26
#define AXP_REG_SHUTDOWN        0x10

// Thermal
#define AXP_REG_TDIE_H          0x38
#define AXP_REG_TDIE_L          0x39

// Bu voltajın altında "pil takılı" biti yok sayılır (boştaki BAT pini)
#define AXP_BAT_PRESENT_MIN_MV  2500

// Chip ID expected
#define AXP2101_CHIP_ID         0x4A

// ─── I2C Yardımcıları (i2c_bus HAL üzerinden) ────────────────────────────────

#define axp_write(reg, val)       i2c_bus_write_reg(I2C_ADDR_AXP2101, (reg), (val))
#define axp_read(reg, buf, len)   i2c_bus_read_reg(I2C_ADDR_AXP2101, (reg), (buf), (len))
#define axp_read_u8(reg, out)     i2c_bus_read_reg(I2C_ADDR_AXP2101, (reg), (out), 1)

static esp_err_t axp_set_bit(uint8_t reg, uint8_t mask, bool value)
{
    return i2c_bus_rmw_reg(I2C_ADDR_AXP2101, reg, mask, value ? mask : 0);
}

// ─── PMIC IRQ Handler ─────────────────────────────────────────────────────────

static void IRAM_ATTR pmic_irq_isr(void* arg)
{
    // IRQ detayı task context'te okunacak; şimdilik genel event yayımla.
    // power_service_update() bir sonraki döngüde detayları okur.
    event_bus_publish_from_isr(EVENT_LOW_BATTERY, 0);  // placeholder
}

// ─── axp2101_init ─────────────────────────────────────────────────────────────

esp_err_t axp2101_init(void)
{
    ESP_LOGI(TAG, "Initializing AXP2101 PMIC...");

    /* Adım 13 (2026-05-14): AXP sleep-state clear on init.
     * Eğer bir önceki boot'ta axp2101_prepare_deep_sleep çağrıldıysa
     * REG 0x26 bit 0 = 1 (SLEEP enable) set edilmiş olabilir. Bu durumda
     * chip sleep mode'da kalmış olabilir. Bit 1 (Wake Up enable) yazarak
     * uyandır + bit 0 clear. Sleep değilse no-op. */
    (void)axp_write(AXP_REG_SLEEP_CFG, 0x02);   // Bit 1 = Software wakeup
    vTaskDelay(pdMS_TO_TICKS(5));
    (void)axp_write(AXP_REG_SLEEP_CFG, 0x00);   // Clear all bits — normal mode

    // Chip ID doğrula
    uint8_t chip_id = 0;
    esp_err_t ret = axp_read_u8(AXP_REG_CHIP_ID, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 I2C read failed (err=0x%x) — not present on this board?", ret);
        return ret;
    }
    /* 0x47: NAR'ın ANA PMIC'i = AXP2101 LİNEER şarj sürümü (datasheet "AXP2101_Rev1.0", 100 mA–1 A
     * linear charger + 5 DCDC). 0x4A: switch-charger sürümü (referans geliştirme kartları, ilk NAR numunesi). */
    s_chip_id = chip_id;
    if (chip_id == 0x47) {
        ESP_LOGI(TAG, "AXP2101 Chip ID OK (0x47 — lineer sarj surumu, NAR)");
    } else if (chip_id != AXP2101_CHIP_ID) {
        ESP_LOGE(TAG, "Chip ID mismatch: got 0x%02X, expected 0x%02X", chip_id, AXP2101_CHIP_ID);
        return ESP_ERR_NOT_FOUND;
    }

#ifdef BOARD_VARIANT_NAR
    /* 2026-11-10: PWRON (guc tusu) hatti bu kartta surekli dusuk: PMIC uzun-basis kesmesi veriyor
     * (REG 0x49 bit2), GPIO10 = 0. PMIC varsayilani (REG 0x22 = 0x06, OFFLEVEL 6 sn) karti her acilistan 6 sn sonra
     * kapatiyordu (USB portu ~5,5 sn gorunup kayboluyordu). "PWRON > OFFLEVEL as POWEROFF source" (bit1) kapatilir;
     * register ucucu oldugu icin her acilista yazilir. Asiri isi korumasi (bit2) ve diger bitler degismez.
     * Hattin neden dusuk kaldigi donanimda olculecek (PWRON pini / yan tus / BSS138).
     * KALICI KARAR (2026-11-10): bu ozellik NAR'da HER ZAMAN KAPALI kalir. Bu satiri kaldirma, baska bir
     * yerde 0x22 bit1'i yeniden acma; hat onarilsa bile karar degismeden dokunma. */
    (void)axp_set_bit(0x22, 0x02, false);
#endif
    if (chip_id == AXP2101_CHIP_ID) ESP_LOGI(TAG, "AXP2101 Chip ID OK (0x%02X)", chip_id);

    // ── Display güç rayı voltaj ayarı (ALDO2 → DS1_PWR_EN) ───────────────────
    // 2026-05-12: ENABLE BIT BURADA SET EDİLMİYOR.
    // Eski davranış: axp2101_init ALDO2'yi açıyordu → her boot'ta (sync wake
    // dahil) display rail enerjili → silent uyanmada bile ekran açılabiliyor.
    // Yeni davranış: voltage config burada, enable BİT ise co5300_init içinde.
    // task_ui spawn olmazsa (sync wake) → co5300_init de çağrılmaz → ALDO2
    // tamamen kapalı kalır → display IC tamamen güçsüz → ekran ASLA açılamaz.
    ESP_ERROR_CHECK(axp_write(AXP_REG_ALDO2_VOL, 0x1C));  // 3.3V config (henüz açık değil)
    ESP_LOGI(TAG, "ALDO2 voltage configured @ 3.3V (NOT yet enabled — co5300_init will enable)");

    // ── ALDO1 (3.3V → IMU + Touch + PCF85063 RTC) — her boot'ta enable ───────
    // 2026-05-13: ALDO1 önceden default POR state'inde ON kabul ediliyordu.
    // Şimdi axp2101_prepare_deep_sleep ALDO1'i OFF yapıyor (Adım 2 — power
    // optimization). PMIC sleep boyunca state'i korur → wake sonrası ALDO1
    // hâlâ OFF olur. Bu yüzden BURADA explicit enable ediyoruz; her boot
    // başlangıçta ALDO1 ON olur.
    //   - Fresh boot: hep ON → touch/IMU/RTC çalışır.
    //   - Sync wake: main.cpp handle_wakeup_reason sonrası tekrar OFF yapar
    //     (silent mode + battery saving).
#ifdef BOARD_VARIANT_NAR
    /* 2026-10-30 (Emin + Claude): dev kit şematiğinde ALDO4 = ICM-42670 **VDDIO**
     * (VDD = VCC3V3); I²C pull-up'ları A3V3 3,3 V. VDDIO 1,8 V iken IMU'nun
     * SDA/SCL girişleri sürekli VDDIO+1,5 V gördü → üç IMU 3-5 dakikada pinlerini
     * kaybedip hattı kelepçeledi (SCL ≈0 V). ALDO4 bu yüzden 3,3 V'a alındı
     * (ICM-42670-P VDDIO 1,71-3,6 V) ve pull-up rayı ALDO1'den ÖNCE açılır:
     * IMU VDDIO'suz kalırken hatta 3,3 V olmasın. SiP içinde ALDO4'ün yükü yok
     * (datasheet: pin 36, kullanıcı yapılandırması). */
    (void)axp_write(0x95, 0x1C);                                       // ALDO4 3.3 V — IMU VDDIO
    (void)axp_set_bit(AXP_REG_LDO_ONOFF, (1 << 3), true);              // ALDO4 AÇIK (önce)
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_LOGI(TAG, "ALDO4 (IMU VDDIO) -> ON @ 3.3V, ALDO1'den once (I2C 3,3 V hatti)");
#endif
    ESP_ERROR_CHECK(axp_write(AXP_REG_ALDO1_VOL, 0x1C));  // 3.3V
    ESP_ERROR_CHECK(axp_set_bit(AXP_REG_LDO_ONOFF, AXP_ALDO1_EN_BIT, true));

    /* ── NAR ana PMIC'i: AXP2101 LİNEER şarj sürümü, Chip ID 0x47 (Session 46v/47b) ──
     * Bu sürümün fabrika varsayılanları 0x4A'dan farklı: LDO'lar KAPALI başlar
     * (ALDO4 kapalıyken beslemesiz IMU I2C hattını ~2.0 V'a kırpar — i2c_bus_init
     * bunu kör yazmayla açar), ALDO1 1.8 V, ALDO2 2.8 V, ALDO3 3.3 V, ALDO4 2.9 V,
     * VBUS akım limiti 500 mA. Kartın doğrulanmış çalışma değerleri her açılışta yazılır
     * (register'lar uçucu). DCDC5 (0x80 bit4 / 0x86) bilerek ELLENMEZ: `Vsys/DCDC5`
     * hattı VSYS düğümüne bağlı. */
    if (chip_id == 0x47) {
#ifndef BOARD_VARIANT_NAR
        (void)axp_write(0x95, 0x0D);                                   // ALDO4 1.8 V — IMU VDD
        (void)axp_set_bit(AXP_REG_LDO_ONOFF, (1 << 3), true);          // ALDO4 AÇIK (kapanırsa I2C kilitlenir)
#endif
        (void)axp_write(0x94, 0x19);                                   // ALDO3 3.0 V — titreşim motoru
        (void)axp_set_bit(AXP_REG_LDO_ONOFF, (1 << 2), true);
        (void)axp_write(0x15, 0x06);                                   // VBUS voltaj eşiği 4.36 V
        (void)axp_write(0x16, 0x04);                                   // VBUS akım limiti 1500 mA (varsayılan 500 mA)
        (void)axp_write(0x50, 0x12);                                   // TS: harici giriş, şarjı etkilemez (NTC checklist'te okunur)
        ESP_LOGI(TAG, "0x47 (lineer surum): ALDO3 3.0V, ALDO4 acik, VBUS limiti 1500 mA yazildi");
    }

    ESP_LOGI(TAG, "ALDO1 (IMU + Touch + RTC) → ON @ 3.3V");

    // ── Fuel Gauge / ADC kurulumu ────────────────────────────────────────────
    // POR sonrası VBAT/TS ADC default ON ama VBUS/VSYS/TDIE kapalı; emin olmak
    // için tüm gerekli kanalları zorla aç. TS pin (termistor) bu boardda yok →
    // bit1=0 yaparak ölçümünü kapatıyoruz; aksi halde fuel gauge "battery
    // temp out of range" diyerek SOC hesaplamayı durdurabiliyor.
    //   0x1D = 0001 1101 → VBAT(0) + VBUS(2) + VSYS(3) + TDIE(4), TS(1) kapalı
    ESP_ERROR_CHECK(axp_write(AXP_REG_ADC_CH_EN, 0x1D));

    // Battery detection: PMIC'in BAT pinindeki gerilimi izleyip "batarya
    // takılı mı" kararı vermesi için aktif olmalı.
    ESP_ERROR_CHECK(axp_set_bit(AXP_REG_BAT_DETECT, (1 << 0), true));

    // Fuel gauge SOC algoritmasını etkinleştir (REG 0x18 bit 3)
    ESP_ERROR_CHECK(axp_set_bit(AXP_REG_FUEL_GAUGE, (1 << 3), true));

    // Şarj akımını pil kapasitesinden 0.3C olarak ayarla (os_config: BATT_CAPACITY_MAH)
    ESP_ERROR_CHECK(axp2101_set_charge_current_auto(BATT_CAPACITY_MAH));

    // Adım 6 (2026-05-14): CHGLED disable.
    // AXP2101'in CHGLED pini bu boardda fiziksel bir LED'e bağlıysa,
    // charging stat değiştikçe LED yanıp söner → akım çeker. Manuel
    // mode + output disabled olarak set ederek LED'i tamamen kapat.
    // Bağlı değilse no-op (sadece register write).
    ESP_ERROR_CHECK(axp_write(AXP_REG_CHGLED_CTRL, 0x00));
    ESP_LOGI(TAG, "CHGLED disabled (REG 0x69 = 0x00)");

    // Diagnostik: hangi durumda olduğumuzu logla — boot sonrası fuel gauge
    // tablosunu güncellemek 1-2 sn alır, bu okuma "ham" ilk değerdir.
    {
        uint8_t stat0 = 0, soc = 0, vbat[2] = {0, 0};
        axp_read_u8(AXP_REG_COM_STAT0,    &stat0);
        axp_read_u8(AXP_REG_BAT_PERCENT,  &soc);
        axp_read   (AXP_REG_VBAT_H,        vbat, 2);
        uint16_t vraw = ((uint16_t)(vbat[0] & 0x3F) << 8) | vbat[1];
        uint16_t vmv  = (uint16_t)((vraw * 11) / 10);
        ESP_LOGI(TAG, "Diag: STAT0=0x%02X  SOC=%u%%  VBAT=%umV (raw=0x%04X)",
                 stat0, soc, vmv, vraw);
        ESP_LOGI(TAG, "       %s  %s  %s",
                 (stat0 & (1 << 5)) ? "VBUS-IN"  : "VBUS-OUT",
                 (stat0 & (1 << 3)) ? "BAT-PRES" : "BAT-NONE",
                 (stat0 & (1 << 2)) ? "CHARGING" : "DISCHG/IDLE");
    }

    // IRQ yapılandır:
    // IRQ_EN0[1] = bat_low_warn, IRQ_EN1[2] = charging, IRQ_EN1[3] = charge_done
    ESP_ERROR_CHECK(axp_write(AXP_REG_IRQ_EN0, 0x02));
    ESP_ERROR_CHECK(axp_write(AXP_REG_IRQ_EN1, 0x0C));
    ESP_ERROR_CHECK(axp_write(AXP_REG_IRQ_EN2, 0x00));

    // IRQ flag'lerini temizle
    ESP_ERROR_CHECK(axp_write(AXP_REG_IRQ_STAT0, 0xFF));
    ESP_ERROR_CHECK(axp_write(AXP_REG_IRQ_STAT1, 0xFF));
    ESP_ERROR_CHECK(axp_write(AXP_REG_IRQ_STAT2, 0xFF));

    // GPIO interrupt: PMIC_IRQ bu boardda bağlı değil (-1), atla.
    // NOT: shift ifadesini "PIN_PMIC_IRQ < 0 ? 0 : PIN_PMIC_IRQ" ile guard'la
    // → guard runtime'da false olsa bile compiler `(1ULL << -1)` constant
    // expression'ı eval etmeye çalışıp warning veriyordu.
    if (PIN_PMIC_IRQ >= 0) {
        gpio_config_t irq_cfg = {
            .pin_bit_mask = 1ULL << (PIN_PMIC_IRQ < 0 ? 0 : PIN_PMIC_IRQ),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&irq_cfg);
        gpio_isr_handler_add((gpio_num_t)PIN_PMIC_IRQ, pmic_irq_isr, NULL);
    }

    s_available = true;
    ESP_LOGI(TAG, "AXP2101 init OK (charge=300mA, all rails ON)");
    return ESP_OK;
}

// ─── axp2101_get_bat_status ───────────────────────────────────────────────────

esp_err_t axp2101_get_bat_status(axp2101_bat_status_t* out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_available) {
        // PMIC not present — return safe defaults
        out->percent      = 100;
        out->voltage_mv   = 0;
        out->current_ma   = 0;
        out->charge_state = AXP_CHARGE_UNKNOWN;
        out->is_low       = false;
        out->bat_present  = false;
        return ESP_ERR_NOT_SUPPORTED;
    }

    // 1. Batarya yüzdesi (donanım SOC)
    uint8_t pct = 0;
    esp_err_t ret = axp_read_u8(AXP_REG_BAT_PERCENT, &pct);
    if (ret != ESP_OK) return ret;
    out->percent = (pct > 100) ? 100 : pct;

    // 2. Gerilim (14-bit ADC, 1.1mV/LSB)
    uint8_t vbat[2];
    ret = axp_read(AXP_REG_VBAT_H, vbat, 2);
    if (ret != ESP_OK) return ret;
    uint16_t vraw = ((uint16_t)(vbat[0] & 0x3F) << 8) | vbat[1];
    out->voltage_mv = (uint16_t)((vraw * 11) / 10);  // ~1.1mV/LSB

    // 3. Akım (13-bit, signed, 0.5mA/LSB)
    uint8_t ibat[2];
    ret = axp_read(AXP_REG_IBAT_H, ibat, 2);
    if (ret != ESP_OK) return ret;
    int16_t iraw = (int16_t)(((ibat[0] & 0x1F) << 8) | ibat[1]);
    if (ibat[0] & 0x20) iraw |= 0xE000;  // sign extend
    out->current_ma = (int16_t)(iraw / 2);  // 0.5mA/LSB

    // 4. Şarj durumu
    uint8_t stat1 = 0;
    ret = axp_read_u8(AXP_REG_COM_STAT1, &stat1);
    if (ret != ESP_OK) return ret;
    uint8_t chg_state = (stat1 >> 5) & 0x07;
    switch (chg_state) {
        case 0: out->charge_state = AXP_CHARGE_DISCHARGE; break;
        case 1:
        case 2: out->charge_state = AXP_CHARGE_CHARGING;  break;
        case 4: out->charge_state = AXP_CHARGE_FULL;      break;
        default: out->charge_state = AXP_CHARGE_UNKNOWN;  break;
    }

    out->is_low = (out->percent <= AXP_LOW_BAT_THRESHOLD_PCT);

    // 5. Batarya takılı mı? (STAT0 bit 3)
    //    BAT pin'inde sağlıklı gerilim varsa AXP2101 bu biti set eder.
    //    USB-only çalışırken bit clear olur ve VBAT ~2.1V floating okunur.
    uint8_t stat0 = 0;
    if (axp_read_u8(AXP_REG_COM_STAT0, &stat0) == ESP_OK) {
        out->bat_present = (stat0 & (1 << 3)) != 0;
        /* 0x47 varyantında (NAR ana PMIC) boştaki BAT pini ~0.76 V'ta duruyor ve çip
         * "pil var" biti set ediyor (Session 46x). Gerçek bir Li-ion 2.5 V'un altına
         * inmez (koruma devresi keser) → altındaysa pil YOK say. */
        if (out->voltage_mv < AXP_BAT_PRESENT_MIN_MV) out->bat_present = false;
    } else {
        out->bat_present = (out->voltage_mv >= 2800);  // güvenli fallback
    }

    // Pil yoksa SOC anlamsız; kullanıcı arayüzünün net karar verebilmesi için
    // charge_state'i UNKNOWN'a çekiyoruz (DISCHARGE yanlış izlenim verir).
    if (!out->bat_present) {
        out->charge_state = AXP_CHARGE_UNKNOWN;
        out->is_low       = false;
    }

    return ESP_OK;
}

// ─── axp2101_set_rail ─────────────────────────────────────────────────────────

esp_err_t axp2101_set_rail(axp2101_rail_t rail, bool enable)
{
    if (!s_available) return ESP_ERR_NOT_SUPPORTED;

    if (rail == AXP_RAIL_DCDC1 && !enable) {
        ESP_LOGE(TAG, "DCDC1 (system rail) CANNOT be disabled!");
        return ESP_ERR_INVALID_ARG;
    }

    /* Lineer sürüm (0x47) kilidi: ALDO4 = IMU VDD. Kapanırsa beslemesiz IMU I2C hattını
     * ~2.0 V'a kırpar ve PMIC dahil hiçbir çipe ulaşılamaz (Session 46l/46s). */
    if (s_chip_id == 0x47 && rail == AXP_RAIL_LDO4 && !enable) {
        ESP_LOGE(TAG, "ALDO4 (IMU VDD) bu kartta KAPATILAMAZ — I2C hatti kilitlenir");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Rail %d → %s", rail, enable ? "ON" : "OFF");

    // AXP2101: DCDC enable/disable via reg 0x80, ALDO via reg 0x90
    switch (rail) {
        case AXP_RAIL_DCDC1:
            return axp_set_bit(AXP_REG_DCDC_ONOFF, AXP_DCDC1_EN_BIT, enable);
        case AXP_RAIL_DCDC2:
            return axp_set_bit(AXP_REG_DCDC_ONOFF, AXP_DCDC2_EN_BIT, enable);
        case AXP_RAIL_DCDC3:   // 1.2V — v1.4.47
            return axp_set_bit(AXP_REG_DCDC_ONOFF, AXP_DCDC3_EN_BIT, enable);
        case AXP_RAIL_DCDC4:   // 1.8V — v1.4.47
            return axp_set_bit(AXP_REG_DCDC_ONOFF, AXP_DCDC4_EN_BIT, enable);
        case AXP_RAIL_LDO1:    // ALDO1 — audio A3V3 muhtemel
            return axp_set_bit(AXP_REG_LDO_ONOFF, AXP_ALDO1_EN_BIT, enable);
        case AXP_RAIL_LDO2:    // ALDO2 — display panel DSI_PWR_EN
            return axp_set_bit(AXP_REG_LDO_ONOFF, AXP_ALDO2_EN_BIT, enable);
        case AXP_RAIL_LDO3:    // ALDO3 — motor + sistem paralel
            return axp_set_bit(AXP_REG_LDO_ONOFF, AXP_ALDO3_EN_BIT, enable);
        case AXP_RAIL_LDO4:    // ALDO4 — VL3 1.8V (belirsiz)
            return axp_set_bit(AXP_REG_LDO_ONOFF, AXP_ALDO4_EN_BIT, enable);
        case AXP_RAIL_BLDO1:   // 1.2V — v1.4.47
            return axp_set_bit(AXP_REG_LDO_ONOFF, AXP_BLDO1_EN_BIT, enable);
        case AXP_RAIL_BLDO2:   // 2.8V — v1.4.47
            return axp_set_bit(AXP_REG_LDO_ONOFF, AXP_BLDO2_EN_BIT, enable);
        default:
            ESP_LOGE(TAG, "Unknown rail: %d", rail);
            return ESP_ERR_INVALID_ARG;
    }
}

// ─── axp2101_get_rail_state ──────────────────────────────────────────────────
// v1.1.1 (2026-05-14): Settings → Güç Rayları toggle UI'si bu fonksiyonu
// kullanarak AXP register'ından rail'in gerçek durumunu okur. Kullanıcı
// runtime'da bir rail'i kapatıp tekrar açtığında, başka bir kod yoluyla
// (örn. driver init veya prepare_deep_sleep) rail state'i değişebileceği
// için her sayfaya girişte state'i hardware'den taze okumak doğrudur.

esp_err_t axp2101_get_rail_state(axp2101_rail_t rail, bool* out_enabled)
{
    if (!s_available)     return ESP_ERR_NOT_SUPPORTED;
    if (!out_enabled)     return ESP_ERR_INVALID_ARG;

    uint8_t reg, mask;
    switch (rail) {
        case AXP_RAIL_DCDC1: reg = AXP_REG_DCDC_ONOFF; mask = AXP_DCDC1_EN_BIT; break;
        case AXP_RAIL_DCDC2: reg = AXP_REG_DCDC_ONOFF; mask = AXP_DCDC2_EN_BIT; break;
        case AXP_RAIL_DCDC3: reg = AXP_REG_DCDC_ONOFF; mask = AXP_DCDC3_EN_BIT; break;
        case AXP_RAIL_DCDC4: reg = AXP_REG_DCDC_ONOFF; mask = AXP_DCDC4_EN_BIT; break;
        case AXP_RAIL_LDO1:  reg = AXP_REG_LDO_ONOFF;  mask = AXP_ALDO1_EN_BIT; break;
        case AXP_RAIL_LDO2:  reg = AXP_REG_LDO_ONOFF;  mask = AXP_ALDO2_EN_BIT; break;
        case AXP_RAIL_LDO3:  reg = AXP_REG_LDO_ONOFF;  mask = AXP_ALDO3_EN_BIT; break;
        case AXP_RAIL_LDO4:  reg = AXP_REG_LDO_ONOFF;  mask = AXP_ALDO4_EN_BIT; break;
        case AXP_RAIL_BLDO1: reg = AXP_REG_LDO_ONOFF;  mask = AXP_BLDO1_EN_BIT; break;
        case AXP_RAIL_BLDO2: reg = AXP_REG_LDO_ONOFF;  mask = AXP_BLDO2_EN_BIT; break;
        default:
            ESP_LOGE(TAG, "get_rail_state: unknown rail %d", rail);
            return ESP_ERR_INVALID_ARG;
    }

    uint8_t v = 0;
    esp_err_t ret = axp_read_u8(reg, &v);
    if (ret == ESP_OK) {
        *out_enabled = (v & mask) != 0;
    }
    return ret;
}

// ─── axp2101_prepare_deep_sleep ──────────────────────────────────────────────

esp_err_t axp2101_prepare_deep_sleep(void)
{
    ESP_LOGI(TAG, "Preparing for deep sleep (Adım 2: ALDO1 dahil tüm raylar OFF, sadece DCDC1)...");

    /* ─── ALDO1 chip'lerini düşük güce al ───────────────────────────────────
     * ALDO1'i AÇIK bırakacağız (RTC chip saymaya devam etsin). Ama üstündeki
     * IMU + Touch IC default mode'da ~5-10 mA çekiyor → ikisini de suspend
     * et:
     *   - IMU (ICM42670): pedometer ~25 µA → suspend ~3.5 µA
     *   - Touch (FT3168): aktif scan ~5-10 mA → HIBERNATE ~5 µA  (büyük kazanç!)
     * RTC chip µA seviyesinde olduğu için dokunulmuyor. */
#if PIN_IMU_INT1 >= 0
    /* 2026-11-04 (NAR): IMU uykuda ÇALIŞIR — accel LP 25 Hz + APEX + WOM, INT1 (GPIO21) MCU'yu
     * uyandırır. Kurulamazsa eski davranış (suspend). */
    if (icm42670_arm_for_sleep() != ESP_OK) (void)icm42670_suspend();
#else
    (void)icm42670_suspend();
#endif
    (void)ft3168_suspend();

    if (s_available) {
        /* ─── Tüm yardımcı rayları kapat (ALDO1 dahil) ─────────────────────
         * Hedef tüketim profili (USB inline meter'da):
         *   DCDC1 (3.3V → MCU+Flash+PSRAM)        ON  — ESP32 RTC için zorunlu
         *   ALDO1 (3.3V → IMU+Touch+PCF85063)     OFF — Adım 2 (2026-05-13):
         *                                          ESP32 internal RTC zamanı
         *                                          tutuyor; PCF85063 yokken
         *                                          time_service_init wake'te
         *                                          system time'a güveniyor.
         *   ALDO2 (display AVDD)                  OFF
         *   ALDO3, ALDO4                          OFF
         *   BLDO1, BLDO2, CPUSLDO, DLDO1          OFF
         *   DCDC2 (display IOVDD 1.8V)            OFF
         *   DCDC3, DCDC4, DCDC5                   OFF
         *
         * Tek register write ile atomik kapatma — önce display, sonra
         * geri kalanı (panel ramp-down sıralı olsun diye).
         */

        // Önce display rayları (ALDO2=AVDD önce, sonra DCDC2=IOVDD — önerilen
        // power-down sırası, panel datasheet'inde belirtildiği şekilde)
        axp_set_bit(AXP_REG_LDO_ONOFF,  AXP_ALDO2_EN_BIT, false);
        vTaskDelay(pdMS_TO_TICKS(5));
        axp_set_bit(AXP_REG_DCDC_ONOFF, AXP_DCDC2_EN_BIT, false);

        // Geri kalan tüm DCDC ve LDO'lar OFF — sadece DCDC1 açık kalır
        axp_write(AXP_REG_DCDC_ONOFF, AXP_DCDC1_EN_BIT);
        // TÜM LDO'lar OFF — ALDO1 dahil. ESP32 internal RTC zamanı tutuyor.
        // NAR (0x47): ALDO4 = IMU VDDIO, AÇIK KALIR — kapanırsa INT1 ölür ve I2C hattı kelepçelenir
        // (bkz. axp2101_set_rail kilidi); uyku akımına +IMU LP (~25-30 µA) + ALDO4 sessiz akımı.
        axp_write(AXP_REG_LDO_ONOFF,  s_chip_id == 0x47 ? AXP_ALDO4_EN_BIT : 0x00);
        // Adım 13: REG 0x91 — DLDO2 OFF (önceki kodumuzda EKSİKTİ!)
        axp_write(AXP_REG_LDO_ONOFF1, 0x00);

        ESP_LOGI(TAG, "Rails: DCDC1 only ON, all DCDC/LDO/DLDO OFF (incl. DLDO2 fix)");
    }

    // ALDO1 (sensörler) açık kalıyor → PCF85063 RTC saat saymaya devam eder
    // DCDC1 (MCU) açık kalıyor → ESP32-S3 deep sleep RTC domain beslenmeli

    // Uyandırma kaynakları: butonlar (EXT1 — RTC IO gerektirir)
    //   PIN_BTN_TOP    = GPIO0  → RTC IO, geçerli
    //   PIN_BTN_BOTTOM = GPIO10 → RTC IO, geçerli AMA donanım sorunu nedeniyle
    //                              EXT1 mask'tan ÇIKARILDI. Boot'ta verify
    //                              yapıldığında (rails ON, RTC IO deinit, internal
    //                              pull-up + 100µs settle) pin 200 ms boyunca
    //                              %100 LOW okunuyordu — kullanıcı butona
    //                              basmadığı halde. Mekanik stuck button veya
    //                              PCB GND short ihtimali; yazılımdan elenemiyor.
    //                              Spurious wake'i önlemenin tek yolu mask'tan
    //                              çıkarmak. Kullanıcı yan butonla deep sleep'ten
    //                              uyandıramaz; üst butonla (BTN_TOP) uyandırır.
    //                              ACTIVE/DIM/DISPLAY_OFF state'lerinde her iki
    //                              buton da ISR ile çalışmaya devam eder.
    //   PIN_IMU_INT1   = -1 (saat) / GPIO21 (NAR, RTC IO) → APEX olayı: aktif-düşük latched INT1
    //   PIN_TOUCH_INT  = GPIO38 → RTC-capable DEĞİL.
    uint64_t wakeup_mask = (1ULL << PIN_BTN_TOP);
#if PIN_IMU_INT1 >= 0
    wakeup_mask |= (1ULL << PIN_IMU_INT1);
#endif
    esp_err_t sleep_ret = esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (sleep_ret != ESP_OK) {
        ESP_LOGW(TAG, "ext1 wakeup config failed (0x%x), skipping", sleep_ret);
    }

    /* ─── ADC channels OFF — sleep boyunca batarya monitoring gereksiz ─────
     * Wake'te axp2101_init tekrar 0x1D yazıp aktive ediyor → bu kapatma
     * idempotent ve güvenli. ~100-500 µA tasarruf. */
    axp_write(AXP_REG_ADC_CH_EN, 0x00);

    /* ─── Adım 13 (2026-05-14): AXP2101 SLEEP MODE — DOĞRU REGISTER ─────────
     * Önceki kod REG 0x31'e yazıyordu — DATASHEET'TE 0x31 SLEEP MODE DEĞİL,
     * AXP2101 hiç sleep mode'a girmemiş, bu zamana kadar internal devreler
     * (bandgap reference, oscillator monitoring, vb.) hep aktif kalmış.
     *
     * Datasheet Section 6.13.2.24 (REG 0x26: Sleep and wakeup control):
     *   bit 0 = SLEEP enable → 1 yazınca chip sleep mode'a girer
     *   bit 1 = Wake Up enable → 1 yazınca software wakeup tetiklenir
     *
     * Figure 6-4 sequence: bit 0 = 1 → AXP sleep mode → quiescent ~40 µA.
     * Wake'te axp2101_init bit 1 = 1 yazıp chip'i uyandırır. */
    axp_set_bit(AXP_REG_SLEEP_CFG, 0x01, true);   /* REG 0x26 bit 0: SLEEP enable */
    ESP_LOGI(TAG, "Adım 13: AXP2101 SLEEP mode entered (REG 0x26 — fixed from 0x31)");

    /* ─── Adım 13: TS pin current source disable ────────────────────────────
     * REG 0x50 — TS pin control. Default'ta TS pin'e 20-60 µA current source
     * sürülüyor (battery thermistor okuma için). Bizim board'da batarya yok,
     * TS okuma anlamsız. Disable etmek için bit 3:2 = 00 (off). 0x00 yazmak
     * tüm TS fonksiyonlarını kapatır. */
    axp_write(AXP_REG_TS_PIN_CTRL, 0x00);
    ESP_LOGI(TAG, "Adım 13: TS pin current source OFF (REG 0x50, ~50 uA tasarruf)");

    /* ─── Adım 9 (2026-05-14): VCC3V3 üstündeki audio chip'leri shutdown ─────
     * KRİTİK BULGU: Schematic incelemesinde ES8311 codec ve ES7210 audio ADC
     * chip'lerinin AXP2101'in HİÇBİR LDO'sundan beslenmediği, doğrudan
     * VCC3V3 (DCDC1 — daima açık sistem rail'i) ile beslendiği keşfedildi:
     *   - ES8311: PVDD + DVDD → R51 (0R) → VCC3V3
     *   - ES7210: AVDD + DVDD → R17 (0R) → A3V3 = VCC3V3
     * Boot'ta hiç initialize edilmedikleri için power-on default state'te
     * kalıyor, sürekli ~5-15 mA çekiyorlar. Açıklanamayan 20 mA'ın
     * kaynağı bu.
     *
     * Software reset komutu (Reg 0x00 bit 7) ile internal clock'lar durur,
     * blok'lar idle moda geçer. I2C bus hâlâ çalışıyor (pull-up'lar VCC3V3'te).
     * Tahmin: 10-15 mA tasarruf. */
    {
        const uint8_t ES8311_ADDR = 0x18;
        const uint8_t ES7210_ADDR = 0x40;

        /* ES8311 codec — software reset + explicit power-down (Adım 15).
         * Reset alone puts chip in "default" state but blocks may still
         * draw current. Explicit power-down register writes for deepest mode.
         */
        esp_err_t e1 = i2c_bus_write_reg(ES8311_ADDR, 0x00, 0x80);
        ESP_LOGI(TAG, "ES8311 codec reset: %s",
                 e1 == ESP_OK ? "OK" : esp_err_to_name(e1));
        vTaskDelay(pdMS_TO_TICKS(2));
        /* Adım 15: ES8311 deeper power-down sequence
         * REG 0x45 = 0x00: System Power Down — all internal blocks OFF
         * REG 0x32 = 0x00: DAC mute (HP power-down)
         * REG 0x12 = 0x00: ADC power off (mic preamp off)
         * REG 0x14 = 0xC0: ADC + DAC reset asserted
         */
        i2c_bus_write_reg(ES8311_ADDR, 0x45, 0x00);
        i2c_bus_write_reg(ES8311_ADDR, 0x32, 0x00);
        i2c_bus_write_reg(ES8311_ADDR, 0x12, 0x00);
        i2c_bus_write_reg(ES8311_ADDR, 0x14, 0xC0);
        ESP_LOGI(TAG, "ES8311: Adım 15 deep power-down (REG 0x45/32/12/14)");

        /* ES7210 audio ADC — software reset + explicit power-down. */
        esp_err_t e2 = i2c_bus_write_reg(ES7210_ADDR, 0x00, 0xFF);
        ESP_LOGI(TAG, "ES7210 ADC reset:    %s",
                 e2 == ESP_OK ? "OK" : esp_err_to_name(e2));
        vTaskDelay(pdMS_TO_TICKS(2));
        /* Adım 15: ES7210 deeper power-down sequence
         * REG 0x01 = 0x3F: Clock manager — all internal clocks STOPPED
         * REG 0x07 = 0x20: Mode control — standby mode
         * REG 0x4B = 0x0F: ADC1+2 standby
         * REG 0x4C = 0x0F: ADC3+4 standby
         */
        i2c_bus_write_reg(ES7210_ADDR, 0x01, 0x3F);
        i2c_bus_write_reg(ES7210_ADDR, 0x07, 0x20);
        i2c_bus_write_reg(ES7210_ADDR, 0x4B, 0x0F);
        i2c_bus_write_reg(ES7210_ADDR, 0x4C, 0x0F);
        ESP_LOGI(TAG, "ES7210: Adım 15 deep power-down (REG 0x01/07/4B/4C)");

        vTaskDelay(pdMS_TO_TICKS(5));   /* chip'lerin idle'a oturması için */
    }

    /* ─── Adım 8 (2026-05-14): AXP2101 monitoring + IRQ disable ─────────────
     * Yöneticinin uyarısı sonrası tekrar datasheet incelemesi: sleep boyunca
     * hala AXP2101'in fuel gauge, BAT detection ve IRQ devreleri aktifti.
     * Bu monitoring fonksiyonları sleep'te gereksiz ve sürekli akım çekiyor:
     *   - Fuel gauge: coulomb counter math overhead (100-300 µA)
     *   - BAT detection: "batarya takılı mı?" polling (50-100 µA)
     *   - IRQ enables: charge/low-bat IRQ source aktif (50-200 µA)
     * Hepsi axp2101_init'te varsayılana geri yükleniyor (her wake'te). */
    axp_set_bit(AXP_REG_FUEL_GAUGE, (1 << 3), false);   /* Fuel gauge OFF */
    axp_set_bit(AXP_REG_BAT_DETECT, (1 << 0), false);   /* BAT detection OFF */
    axp_write(AXP_REG_IRQ_EN0, 0x00);                    /* All IRQs OFF */
    axp_write(AXP_REG_IRQ_EN1, 0x00);
    axp_write(AXP_REG_IRQ_EN2, 0x00);
    ESP_LOGI(TAG, "Adım 8: Fuel gauge + BAT detect + IRQ enables OFF (~200-600 uA)");

    ESP_LOGI(TAG, "Deep sleep ready: A-Modified rails (DCDC1 only) + IMU suspended, BTN_TOP wakeup armed");

    /* ─── Adım 16 (2026-05-14): REGISTER READ-BACK DIAGNOSTIC ──────────────
     * Kullanıcı/manager soruyu sordu: "AXP2101 yazımlarımıza gerçekten
     * cevap veriyor mu?" Kanıt için kritik register'ları geri oku +
     * beklenen değerle karşılaştır + logla.
     *
     * Eğer readback değerleri yazdığımız değerlerden farklıysa:
     *   - Chip yazımı reddediyor (I²C ACK alıyor ama internal state değişmiyor)
     *   - Veya auto-clear bit'ler (sleep mode bit gibi)
     *   - Veya hatalı register adresi (datasheet yanlış olabilir)
     */
    {
        uint8_t v = 0xAA;   /* dummy */
        ESP_LOGI(TAG, "── Adım 16 register readback (yazım doğrulama) ──");

        axp_read_u8(AXP_REG_DCDC_ONOFF, &v);
        ESP_LOGI(TAG, "  REG 0x80 (DCDC ONOFF):  0x%02X  (yazıldı: 0x01 = DCDC1 only)", v);

        axp_read_u8(AXP_REG_LDO_ONOFF, &v);
        ESP_LOGI(TAG, "  REG 0x90 (LDO ONOFF 0): 0x%02X  (yazıldı: 0x00 = all LDOs off)", v);

        axp_read_u8(AXP_REG_LDO_ONOFF1, &v);
        ESP_LOGI(TAG, "  REG 0x91 (LDO ONOFF 1): 0x%02X  (yazıldı: 0x00 = DLDO2 off)", v);

        axp_read_u8(AXP_REG_ADC_CH_EN, &v);
        ESP_LOGI(TAG, "  REG 0x30 (ADC CH EN):   0x%02X  (yazıldı: 0x00 = all ADC off)", v);

        axp_read_u8(AXP_REG_SLEEP_CFG, &v);
        ESP_LOGI(TAG, "  REG 0x26 (SLEEP CFG):   0x%02X  (yazıldı: bit 0 = SLEEP enable)", v);

        axp_read_u8(AXP_REG_TS_PIN_CTRL, &v);
        ESP_LOGI(TAG, "  REG 0x50 (TS PIN):      0x%02X  (yazıldı: 0x00 = TS off)", v);

        axp_read_u8(AXP_REG_CHGLED_CTRL, &v);
        ESP_LOGI(TAG, "  REG 0x69 (CHGLED):      0x%02X  (yazıldı: 0x00 = LED off)", v);

        axp_read_u8(AXP_REG_FUEL_GAUGE, &v);
        ESP_LOGI(TAG, "  REG 0x18 (FUEL/CHG):    0x%02X  (bit 3 = 0 expected, fuel gauge off)", v);

        axp_read_u8(AXP_REG_BAT_DETECT, &v);
        ESP_LOGI(TAG, "  REG 0x68 (BAT DETECT):  0x%02X  (bit 0 = 0 expected, detect off)", v);
        ESP_LOGI(TAG, "── /Adım 16 ──");
    }

    return ESP_OK;
}

// ─── axp2101_set_charge_current ──────────────────────────────────────────────

esp_err_t axp2101_set_charge_current(uint16_t ma)
{
    /* REG 0x62[4:0] (iki sürümde de aynı): N<=8 → 25·N mA, N>8 → 200+100·(N−8) mA.
     * (Eski eşleme datasheet'e uymuyordu; yalnız 300 mA tesadüfen doğru koda (9) düşüyordu —
     * tek çağrı 300 mA olduğu için davranış aynı kaldı.)
     * Üst sınır: lineer sürüm (0x47) 1 A; switch sürüm burada da 1 A ile sınırlı tutulur. */
    if (ma > 1000) ma = 1000;
    uint8_t icc;
    if      (ma == 0)   icc = 0;
    else if (ma < 100)  icc = 4;                                   // en düşük kademe 100 mA
    else if (ma <= 200) icc = (uint8_t)(ma / 25);                  // 100..200 mA, 25 mA adım
    else                icc = (uint8_t)(8 + (ma - 200) / 100);     // 300..1000 mA, 100 mA adım

    uint8_t cur = 0;
    ESP_ERROR_CHECK(axp_read_u8(AXP_REG_CHG_CTRL1, &cur));
    cur = (uint8_t)((cur & 0xE0) | (icc & 0x1F));
    ESP_ERROR_CHECK(axp_write(AXP_REG_CHG_CTRL1, cur));

    ESP_LOGI(TAG, "Charge current set: %dmA (icc=0x%X)", ma, icc);
    return ESP_OK;
}

// ─── axp2101_charge_ma_for_capacity ──────────────────────────────────────────
/* Pil kapasitesinden (mAh) 0.3C hedefini hesaplar ve AXP2101'in yazabildiği bir
 * kademeye AŞAĞI yuvarlar (asla 0.3C'yi aşmaz → nazik/güvenli). Taban 100 mA. */
uint16_t axp2101_charge_ma_for_capacity(uint16_t mah)
{
    static const uint16_t steps[] = {100,125,150,175,200,300,400,500,600,700,800,900,1000};
    if (mah == 0) return 100;
    uint32_t target = (uint32_t)mah * 3u / 10u;        /* 0.3C */
    uint16_t best = 100;                                /* taban kademe */
    for (size_t i = 0; i < sizeof(steps)/sizeof(steps[0]); i++) {
        if (steps[i] <= target) best = steps[i];
        else break;
    }
    return best;
}

// ─── axp2101_set_charge_current_auto ─────────────────────────────────────────
/* Kapasiteden 0.3C akımını hesaplayıp yazar; termal guard'ın geri döneceği
 * nominal değeri de saklar. */
esp_err_t axp2101_set_charge_current_auto(uint16_t mah)
{
    uint16_t ma = axp2101_charge_ma_for_capacity(mah);
    s_nominal_charge_ma = ma;
    ESP_LOGI(TAG, "Pil %u mAh -> 0.3C sarj akimi %u mA", mah, ma);
    return axp2101_set_charge_current(ma);
}

// ─── axp2101_soft_power_off (Adım 17 — Soft Power Off test mode) ─────────────
//
// Datasheet Section 6.5.4.3 — "Power Off" sources:
//   - POK long press (yan PWR button)
//   - Write "1" to REG10H[0]  ← BU YÖNTEM
//   - VSYSGOOD high go low
//   - DCDC over/under voltage
//   - Die temp over level2
//
// Soft PWROFF state:
//   - Tüm output'lar OFF (DCDC1 dahil → ESP32 ölür)
//   - Sadece RTCLDO açık kalır
//   - Quiescent: ~40 µA (datasheet) + USB pull-ups + LED = ~5 mA toplam
//
// Bu fonksiyon DÖNMEZ — ESP32 power kaybeder ve halt eder.

esp_err_t axp2101_soft_power_off(void)
{
    if (!s_available) {
        ESP_LOGE(TAG, "Soft PWROFF: AXP2101 not available");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "═══ AXP2101 SOFT PWROFF ═══");
    ESP_LOGW(TAG, "Tum outputlar kapatilacak. Wake icin:");
    ESP_LOGW(TAG, "  1) Yan PWR button (Key3) bas");
    ESP_LOGW(TAG, "  2) VEYA USB kabloyu cikar+tak");
    ESP_LOGW(TAG, "════════════════════════════");

    /* Log mesajlarının çıkması için kısa bekleme */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* REG 0x10 bit 0 = 1: Soft PWROFF tetikle */
    esp_err_t ret = axp_set_bit(AXP_REG_SHUTDOWN, 0x01, true);

    /* Chip output'ları kestiği an ESP32 ölür, bu satırların altı çalışmaz */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Buraya geldiysek soft PWROFF başarısız demektir */
    ESP_LOGE(TAG, "Soft PWROFF write done but chip still alive!? (ret=0x%x)", ret);
    return ret;
}


// ─── axp2101_set_charge_voltage_mv ───────────────────────────────────────────
/* REG 0x64[2:0]. DİKKAT: lineer sürümde (0x47) 000 = 5.0 V (switch sürümde "reserved") —
 * Li-ion için tehlikeli; bu fonksiyon 000'ı ASLA yazmaz, yalnız bilinen kademeleri kabul eder. */
esp_err_t axp2101_set_charge_voltage_mv(uint16_t mv)
{
    if (!s_available) return ESP_ERR_NOT_SUPPORTED;
    uint8_t code;
    switch (mv) {
        case 4000: code = 1; break;
        case 4100: code = 2; break;
        case 4200: code = 3; break;
        case 4350: code = 4; break;
        case 4400: code = 5; break;
        default:
            ESP_LOGE(TAG, "gecersiz sarj voltaji %u mV (4000/4100/4200/4350/4400)", mv);
            return ESP_ERR_INVALID_ARG;
    }
    uint8_t cur = 0;
    esp_err_t ret = axp_read_u8(0x64, &cur);
    if (ret != ESP_OK) return ret;
    return axp_write(0x64, (uint8_t)((cur & 0xF8) | code));
}

// ─── axp2101_charge_thermal_guard ────────────────────────────────────────────
/* Lineer şarjda (0x47) VSYS≈5 V ile pil arasındaki fark ısıya dönüşür (300 mA'de ~0.4 W).
 * Periyodik çağrılır; YALNIZ 0x47 + şarj sürerken + çip sıcaklığı yüksekken devreye girer:
 * ≥75 °C → şarj akımı 100 mA'e iner, ≤60 °C → 300 mA'e döner. Aksi halde hiçbir şey yazmaz. */
void axp2101_charge_thermal_guard(void)
{
    static bool s_throttled = false;
    if (!s_available || s_chip_id != 0x47) return;
    uint8_t st1 = 0, raw[2] = {0, 0};
    if (axp_read_u8(0x01, &st1) != ESP_OK) return;
    bool charging = ((st1 >> 5) & 0x03) == 0x01;
    if (!charging && !s_throttled) return;
    if (axp_read(0x3C, raw, 2) != ESP_OK) return;
    uint16_t t = (uint16_t)(((raw[0] & 0x3F) << 8) | raw[1]);
    if (t == 0) return;                                  /* ADC kanalı kapalı */
    float die_c = 22.0f + (7274.0f - (float)t) / 20.0f;
    if (!s_throttled && charging && s_nominal_charge_ma > 100 && die_c >= 75.0f) {
        ESP_LOGW(TAG, "PMIC cip %.0f C — sarj akimi 100 mA'e dusuruldu", die_c);
        axp2101_set_charge_current(100);
        s_throttled = true;
    } else if (s_throttled && die_c <= 60.0f) {
        ESP_LOGI(TAG, "PMIC cip %.0f C — sarj akimi %u mA'e geri alindi", die_c, s_nominal_charge_ma);
        axp2101_set_charge_current(s_nominal_charge_ma);
        s_throttled = false;
    }
}

/* PMIC çip (die) sıcaklığı. ADC kanalı kapalıysa (REG 0x30 bit 4) açar, ~20 ms bekler, 0x3C/0x3D okur.
 * Formül XPowersLib ile aynı: T = 22 + (7274 - raw) / 20  [°C]. */
esp_err_t axp2101_get_die_temp(float* out_c)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;
    uint8_t en = 0;
    esp_err_t e = axp_read_u8(AXP_REG_ADC_CH_EN, &en);
    if (e != ESP_OK) return e;
    if (!(en & 0x10)) {
        e = axp_set_bit(AXP_REG_ADC_CH_EN, 0x10, true);
        if (e != ESP_OK) return e;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    uint8_t raw[2] = {0, 0};
    e = axp_read(0x3C, raw, 2);
    if (e != ESP_OK) return e;
    uint16_t t = (uint16_t)(((raw[0] & 0x3F) << 8) | raw[1]);
    if (t == 0) return ESP_ERR_INVALID_RESPONSE;
    *out_c = 22.0f + (7274.0f - (float)t) / 20.0f;
    return ESP_OK;
}
