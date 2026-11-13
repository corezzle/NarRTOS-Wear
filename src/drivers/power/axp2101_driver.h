#pragma once
/**
 * @brief AXP2101 PMIC Driver
 *
 * Interface: I2C (shared bus, addr 0x34)
 *
 * Responsibilities:
 *  - Battery voltage / percentage / charge state
 *  - LDO/DCDC rail enable/disable for peripheral power gating
 *  - PMIC interrupt handling (low-bat, overheat, button)
 *  - Deep sleep coordination (disable non-essential rails before sleep)
 *
 * Power Rails (typical wearable config):
 *  DCDC1 → 3.3V system rail (MCU, Flash, PSRAM) — always on
 *  DCDC2 → 1.8V display IOVDD
 *  LDO1  → 3.3V sensor rail (IMU, RTC)
 *  LDO2  → display AVDD (can be gated in sleep)
 */

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

// ─── Charge State ────────────────────────────────────────────────────────────
typedef enum {
    AXP_CHARGE_DISCHARGE = 0,
    AXP_CHARGE_CHARGING  = 1,
    AXP_CHARGE_FULL      = 2,
    AXP_CHARGE_UNKNOWN   = 3,
} axp2101_charge_state_t;

// ─── Battery Status ───────────────────────────────────────────────────────────
typedef struct {
    uint8_t  percent;         // 0–100%
    uint16_t voltage_mv;      // Battery voltage in mV
    int16_t  current_ma;      // Charge current (+ charging, - discharging)
    axp2101_charge_state_t charge_state;
    bool     is_low;          // true if below LOW_BAT_THRESHOLD
    bool     bat_present;     // true if PMIC sees a battery on BAT pin
                              // (false = USB-only çalışıyor, pil takılı değil)
} axp2101_bat_status_t;

#define AXP_LOW_BAT_THRESHOLD_PCT  15

// ─── Power Rail IDs ───────────────────────────────────────────────────────────
/* v1.4.47 (2026-05-15): Schematic yeniden incelendi (ESP32-S3-Touch-AMOLED-2.06
 * datasheet), önceki mapping yorumları yanlıştı. Düzeltilmiş bilgiler:
 *   DCDC1 = VCC3V3 (sistem, kapatılamaz)
 *   DCDC2 = 0.9V (hedef belirsiz, schematic tablosunda)
 *   DCDC3 = 1.2V
 *   DCDC4 = 1.8V (muhtemelen AMOLED IOVDD veya FLASH IO)
 *   ALDO1 = VL1 3.3V (muhtemelen A3V3 audio analog supply)
 *   ALDO2 = VL2 3.3V → DSI_PWR_EN (display panel — kanıtlı)
 *   ALDO3 = VCC3V3 (motor + sistem paralel — kanıtlı)
 *   ALDO4 = VL3 1.8V (kullanım belirsiz)
 *   BLDO1 = 1.2V (kullanım belirsiz)
 *   BLDO2 = 2.8V (kullanım belirsiz)
 *   CPUSLDO = VCL 1.2V (ESP32 internal CPU LDO, kapatılamaz)
 *
 * Settings → Güç Rayları sayfası belirsiz rail'leri test için listeliyor;
 * kullanıcı toggle ile kapatıp etkiyi gözlemleyebilir. */
typedef enum {
    AXP_RAIL_DCDC1 = 0,   // VCC3V3 (sistem, kapatılamaz)
    AXP_RAIL_DCDC2 = 1,   // 0.9V (hedef belirsiz)
    AXP_RAIL_DCDC3 = 2,   // 1.2V (hedef belirsiz) — v1.4.47
    AXP_RAIL_DCDC4 = 3,   // 1.8V (muhtemelen display IO veya FLASH IO) — v1.4.47
    AXP_RAIL_LDO1  = 4,   // ALDO1: VL1 3.3V (muhtemelen audio A3V3)
    AXP_RAIL_LDO2  = 5,   // ALDO2: VL2 3.3V (display panel DSI_PWR_EN)
    AXP_RAIL_LDO3  = 6,   // ALDO3: VCC3V3 paralel + motor
    AXP_RAIL_LDO4  = 7,   // ALDO4: VL3 1.8V (kullanım belirsiz)
    AXP_RAIL_BLDO1 = 8,   // 1.2V (kullanım belirsiz) — v1.4.47
    AXP_RAIL_BLDO2 = 9,   // 2.8V (kullanım belirsiz) — v1.4.47
} axp2101_rail_t;

// ─── API ──────────────────────────────────────────────────────────────────────

/** Initialize PMIC, configure IRQ pin, set charge current. */
esp_err_t axp2101_init(void);

/** Read full battery status. */
esp_err_t axp2101_get_bat_status(axp2101_bat_status_t* out);

/** Enable or disable a power rail. */
esp_err_t axp2101_set_rail(axp2101_rail_t rail, bool enable);

/**
 * v1.1.1 (2026-05-14): Settings → Güç Rayları sayfasının toggle UI'sini
 * mevcut donanım durumuyla senkronize edebilmesi için runtime rail state
 * okuma. AXP DCDC_ONOFF (0x80) ve LDO_ONOFF (0x90) register'larını sorgular.
 * out_enabled: ilgili bit set ise true (rail enabled), aksi halde false.
 * s_available false veya I²C okuma hatası → ESP_ERR_*; UI bu durumda
 * toggle'ı eski state'inde bırakmalı.
 */
esp_err_t axp2101_get_rail_state(axp2101_rail_t rail, bool* out_enabled);

/**
 * Prepare for deep sleep:
 *  - Disable display rails (DCDC2, LDO2)
 *  - Keep DCDC1 (MCU) and LDO1 (RTC, IMU WOM) active
 *  - Set wake source: PMIC button or WOM interrupt
 */
esp_err_t axp2101_prepare_deep_sleep(void);

/** Set charge current limit (mA). Typical: 200–500mA for wearables. */
esp_err_t axp2101_set_charge_current(uint16_t ma);

/** Pil kapasitesinden (mAh) 0.3C şarj akımını hesaplar; AXP kademesine aşağı yuvarlar. */
uint16_t axp2101_charge_ma_for_capacity(uint16_t mah);

/** Kapasiteden 0.3C akımını hesaplayıp yazar (termal guard nominali de saklanır). */
esp_err_t axp2101_set_charge_current_auto(uint16_t mah);

/** Şarj bitiş voltajı (4000/4100/4200/4350/4400 mV). 0x64'e 000 (lineer sürümde 5.0 V) asla yazmaz. */
esp_err_t axp2101_set_charge_voltage_mv(uint16_t mv);

/** Lineer sürüm (0x47) için şarj ısı koruması; periyodik çağrılır, diğer durumlarda etkisizdir. */
void axp2101_charge_thermal_guard(void);

/**
 * Adım 17 (2026-05-14): AXP2101 Soft Power Off.
 * REG 0x10 bit 0 = 1 yazar → tüm output'lar OFF (RTCLDO hariç).
 * ESP32 güç kaybeder ve ölür → bu fonksiyon DÖNMEZ.
 *
 * Wake (saatı geri açma) için:
 *   - Yan PWR button (Key3, AXP2101 PWRON pin) bas
 *   - VEYA USB kablosunu çıkar + tak (VBUS low → high transition)
 *
 * Quiescent akım datasheet: ~40 µA + LED + USB pull-ups = ~5 mA toplam.
 *
 * Kullanım: power_service_enter_deep_sleep yerine bu fonksiyon çağrılabilir
 * (test modu). Normal deep sleep button-wake'i çalışmayacak.
 */
esp_err_t axp2101_soft_power_off(void);

/* PMIC çip sıcaklığı (°C). ADC kanalını gerekirse açar. */
esp_err_t axp2101_get_die_temp(float* out_c);
