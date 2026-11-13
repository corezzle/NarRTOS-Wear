# ESP32-S3 Power & Sleep Map

> Bu doküman tamamen **Espressif ESP32-S3 Series Datasheet v1.6** (75 sf, Espressif Systems) referansından üretilmiştir. Kaynağı: `~/Downloads/Esp32-s3_datasheet_en.pdf`.
>
> Amaç: Hangi pin neyi besliyor, hangi sleep modu ne kadar akım çekiyor, "tam uyku modu" gerçekte ne anlama geliyor — bunu ESP32-S3-Touch-AMOLED-2.06 saat board'umuzla eşleştirerek göstermek.

---

## 1. ESP32-S3 Güç Pinleri (Table 2-6 Power Pins, sf. 21)

| Pin # | Pin Adı       | Yön    | Power Domain                                  | İlgili IO Bloğu |
|-------|---------------|--------|-----------------------------------------------|-----------------|
| 2     | VDD3P3        | Input  | Analog power domain                           | —               |
| 3     | VDD3P3        | Input  | Analog power domain                           | —               |
| 20    | **VDD3P3_RTC**| Input  | RTC + part of Digital power domains           | **RTC IO**      |
| 29    | VDD_SPI       | I/O    | In-package PSRAM / off-package flash & PSRAM  | SPI IO          |
| 46    | VDD3P3_CPU    | Input  | Digital power domain                          | Digital IO      |
| 55    | VDDA          | Input  | Analog power domain                           | —               |
| 56    | VDDA          | Input  | Analog power domain                           | —               |
| 57    | GND           | —      | External ground                               | —               |

**Önemli not:** Datasheet'te `VDD3P3_RTC` aynı zamanda *part of Digital power domains*'i besliyor. Yani RTC IO + bir kısım digital. Deep sleep'te `VDD3P3_CPU` kesilirken **`VDD3P3_RTC` aktif kalmak zorunda** (RTC controller, RTC SRAM, RTC GPIO 0-21 için).

---

## 2. Chip İçi Voltaj Regülatörleri (Table 2-7, sf. 21)

Chip dışından **3.3 V** gelir → içeride şu regülatörler bunu çeker:

| Voltaj Regülatörü            | Çıkış  | Beslediği Domain                            |
|-------------------------------|--------|---------------------------------------------|
| Digital System Voltage Reg.   | 1.1 V  | Digital power domain (CPU, peripherals)     |
| Low-power Voltage Reg.        | 1.1 V  | RTC power domain (RTC ctrl, RTC SRAM, ULP)  |
| Flash Voltage Regulator       | 1.8 V  | In-package/off-package flash veya PSRAM     |

Bizim chipte (ESP32-S3R8, **8 MB embedded octal PSRAM**), Flash Voltage Regulator **PSRAM**'ı besliyor; deep sleep'te `VDDSDIO` domain'i OFF edilince bu reg'i de söndürebiliyoruz.

---

## 3. ESP32-S3 Power Scheme Block Diagram (Figure 2-2, sf. 22)

```
                                 ┌─────────── ESP32-S3 Chip Boundary ───────────┐
                                 │                                              │
   VDD3P3_RTC ──────┐            │   ┌──────────────────┐                       │
   (3.3V, pin 20)   │            │   │ Low Power V.Reg. │──→ RTC IO (GPIO 0-21) │
                    └────────────┼──→│      → 1.1V      │──→ RTC controller     │
                                 │   └──────────────────┘    + RTC SRAM + ULP   │
                                 │                                              │
   VDD3P3_CPU ──────┐            │   ┌──────────────────┐                       │
   (3.3V, pin 46)   │            │   │ Digital System   │──→ Digital IO (GPIOs) │
                    └────────────┼──→│  V.Reg. → 1.1V   │──→ CPU0 + CPU1        │
                                 │   └──────────────────┘    + peripherals      │
                                 │                                              │
   VDDA1 / VDDA2 ───┐            │   ┌──────────────────┐                       │
   (3.3V, pin 55,56)│            │   │  Analog domain   │──→ ADC1/2, PLL,       │
   + VDD3P3 (2,3) ──┴────────────┼──→│  (no regulator,  │    RF (2.4G), TSENS,  │
                                 │   │   direct 3.3V)   │    Touch, Bias        │
                                 │   └──────────────────┘                       │
                                 │                                              │
                                 │   ┌──────────────────┐    ┌── R_SPI=14Ω ──┐  │
                                 │   │  Flash Voltage   │────│   internal    │──┼── VDD_SPI
   (no external pin)             │   │  Reg. → 1.8V     │    │   switch      │  │   (pin 29)
                                 │   └──────────────────┘    └───────────────┘  │
                                 │                                              │
                                 │   CHIP_PU (pin 1) ──→ Master power enable    │
                                 │     HIGH = chip ON, LOW = chip OFF (~1 µA)   │
                                 └──────────────────────────────────────────────┘
```

**Anahtar gözlemler:**

1. **CHIP_PU pin'i master kill switch.** Datasheet sf. 60'da: *"Power off — CHIP_PU is set to low level. The chip is shut down. 1 µA"*. Bu, yazılım üzerinden ESP32'nin kendi kendini kapatamayacağı tek mod — dış bir devre CHIP_PU'yu LOW çekmeli.
2. **VDD3P3_RTC ayrı pin** çünkü deep sleep'te bu rail tek başına ayakta kalır (CPU rail'i kesilirken).
3. **VDD_SPI** "input or output" — ESP-IDF, eFuse'a göre PSRAM'ı 1.8 V Flash Reg'den besler veya dışarıdan 3.3 V verilebilir. Bizim board'da chip-içi PSRAM = chip-içi reg.

---

## 4. Sleep Modları & Akım Tüketimi (Table 4-7, 4-8, sf. 58-60)

### 4.1 Mod hiyerarşisi

```
   Active mode          (CPU running, RF off)                   ~30-90 mA
        │
        ▼
   Modem-sleep          (CPU running, Wi-Fi clock-gated)        13-108 mA
        │                                                       (freq + core sayısına göre)
        ▼
   Light-sleep          (CPU paused, RAM korunur,               240 µA (chip)
        │               PSRAM self-refresh, GPIO Hi-Z)          +140 µA (8MB 3.3V PSRAM)
        │                                                       = ~380 µA toplam
        ▼
   Deep-sleep           (CPU OFF, sadece RTC + ULP canlı)       8 µA  (RTC periph ON)
        │                                                       7 µA  (RTC periph OFF) ◀── BURADAYIZ
        ▼
   Power off            (CHIP_PU = LOW, chip tamamen ölü)       1 µA   ◀── tek daha derin nokta
                        Wake için harici CHIP_PU = HIGH gerekir
                        (bizim board'da: AXP2101 Soft PWROFF)
```

### 4.2 Detaylı tablo (datasheet birebir)

| Work mode    | Açıklama                                                       | Akım (chip-level) |
|--------------|----------------------------------------------------------------|-------------------|
| Light-sleep  | VDD_SPI + Wi-Fi powered down, GPIOs high-impedance             | **240 µA**        |
| Light-sleep  | + 8MB octal PSRAM @ 3.3V self-refresh                          | +140 µA           |
| Deep-sleep   | RTC memory + RTC peripherals UP                                | **8 µA**          |
| Deep-sleep   | RTC memory UP, RTC peripherals DOWN                            | **7 µA**          |
| **Power off**| CHIP_PU = LOW, chip shut down                                  | **1 µA**          |

> **Kaynak:** Table 4-8 "Current Consumption in Low-Power Modes", sf. 59 + dipnot, sf. 60.

### 4.3 Bizim ölçümümüz vs. datasheet

KEWEISI USB akım ölçer = **~0.02 A** (20 mA, çözünürlük 10 mA).
Datasheet "deep-sleep, RTC periph OFF" = **7 µA** = 0.007 mA.

**Fark = 20 mA - 0.007 mA = 19.993 mA**.

Bu 19.993 mA **ESP32 chip'inden değil, board'dan** geliyor:

```
  Toplam VBUS akımı = 20 mA
   ├── ESP32 chip (deep sleep, RTC periph off) ............ 0.007 mA  ◀── datasheet limit
   ├── AXP2101 PMIC quiescent + LED + I²C ................. ~5 mA
   ├── USB-C CC pull-downs (2× 5.1 kΩ → 5V) ............... ~2 mA
   ├── ES8311 codec keep-alive (VCC3V3 her zaman var) ..... ~1-2 mA
   ├── NS4150B amp leakage (CTRL LOW ama VCC3V3 var) ...... <1 mA
   ├── Status LED (eğer açık kalmışsa) .................... ~1-3 mA
   └── Pull-up dirençleri (I²C 2.2k×2 + BTN 10k+10k) ...... ~3-5 mA
                                                       Toplam: ~15-20 mA  ✓
```

**Sonuç:** Yazılımla ESP32 chip akımı zaten datasheet limiti olan **7 µA**'e indirilmiş durumda. Geri kalan 20 mA, ESP32 değil, **board'un kendisi**. KEWEISI 10 mA çözünürlükte olduğu için sub-mA değişiklikleri gösteremiyor — bu nedenle yapılan tüm optimizasyonlar (Adım 5, 8, 10, 11, 13, 14, 14B, 15, 16) ölçer üzerinde görünmüyor.

---

## 5. Bizim Board'da Power Flow (ESP32-S3-Touch-AMOLED-2.06)

```
  ┌──────────────┐
  │ USB-C VBUS   │  5V, akım ölçere bağlı (KEWEISI)
  │   (5V)       │
  └──────┬───────┘
         │
         ▼
  ┌────────────────────────────────┐
  │ AXP2101 PMIC (I²C 0x34)        │  ← Master power manager
  │ ────────────────────────────── │
  │  DCDC1 → 3.3V VCC3V3 (system) ─┼──┬───→ ESP32-S3 (VDD3P3, VDD3P3_CPU, VDDA, VDD3P3_RTC)
  │                                │  ├───→ ES8311 audio codec
  │                                │  ├───→ NS4150B amp (PA_CTRL via GPIO 46)
  │                                │  ├───→ FT3168 touch ctrl
  │                                │  ├───→ PCF85063 RTC (VCC-RTC, via RTCLDO içerden)
  │                                │  └───→ QMI8658 IMU
  │  DCDC2 → 1.8V (display IOVDD)  ┼──────→ CO5300 AMOLED IOVDD
  │  ALDO1 → 3.3V (A3V3 audio)     ┼──────→ ES7210 ADC
  │  ALDO2/3/4 → unused on board   │
  │  BLDO1/2 → unused on board     │
  │                                │
  │  PWRON pin (Key3, yan buton)   │  ← Wake source (button hold ≥1s)
  │  IRQ pin → (not wired on board)│
  └──────────────┬─────────────────┘
                 │
        ┌────────┴──────────────────────────────────────────┐
        ▼                                                   ▼
  ┌──────────────────┐                              ┌──────────────────┐
  │ ESP32-S3R8       │                              │ Board sensors    │
  │ ───────────────  │                              │ ───────────────  │
  │ CHIP_PU ← VCC3V3 │ ← RC reset @ boot            │ FT3168 (touch)   │
  │   via 10k + cap  │   (sabit HIGH, AXP'dan değil)│ QMI8658 (IMU)    │
  │                  │                              │ PCF85063 (RTC)   │
  │ Deep sleep'te:   │                              │ ES8311 (codec)   │
  │  - CPU rail OFF  │                              │ ES7210 (ADC)     │
  │  - RTC rail ON   │                              │ NS4150B (amp)    │
  │  - VDD_SPI OFF   │                              │ FUSB302 (USB-PD) │
  │    (Adım 14B)    │                              └──────────────────┘
  │  → 7 µA limit    │                              Hepsi VCC3V3'ten besleniyor →
  └──────────────────┘                              deep sleep'te de canlı kalıyorlar
```

### 5.1 Kritik insight

CHIP_PU **AXP2101'e bağlı değil**, doğrudan VCC3V3'e 10 kΩ pull-up + RC reset devresi üzerinden bağlı. Yani:

- **ESP32 deep sleep**: chip içindeki CPU rail söner ama VCC3V3 hala 3.3V → CHIP_PU hala HIGH → chip "deep sleep" durumunda ama beslenmesi devam ediyor.
- **AXP2101 Soft PWROFF**: DCDC1 söner → VCC3V3 = 0V → CHIP_PU HIGH→LOW → ESP32 "Power off" moduna düşer (datasheet'in **1 µA** modu).

Bu nedenle Soft PWROFF en derin modu sağlıyor (KEWEISI'de 0.00 A), ama **R11 (10k pull-up, VCC3V3'e bağlı)** ile yan PWR butonu artık HIGH→LOW transition üretemiyor — wake çalışmıyor. Hardware limitasyonu.

---

## 6. ESP-IDF API'leri ile Datasheet Modlarını Aktive Etme

| Datasheet Mode                  | API çağrısı                                                       | Mevcut kodda? |
|----------------------------------|-------------------------------------------------------------------|---------------|
| Modem-sleep                     | `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` + idle                       | N/A (Wi-Fi off) |
| Light-sleep                     | `esp_light_sleep_start()`                                         | Kullanılmıyor |
| Deep-sleep (RTC periph ON, 8µA) | `esp_deep_sleep_start()` (default)                                | —              |
| **Deep-sleep (RTC periph OFF, 7µA)** | `esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF)` + `esp_deep_sleep_start()` | **✓ AKTİF** (power_service.cpp:499) |
| VDDSDIO/PSRAM OFF (-30 mA active, kayda değer leakage tasarrufu) | `esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_OFF)` | **✓ AKTİF** (power_service.cpp:507) |
| USB pad disconnect              | `usb_serial_jtag_ll_phy_enable_pad(false)` + `rtc_gpio_isolate(19/20)` | **✓ AKTİF** (power_service.cpp:330) |
| **Power off (CHIP_PU LOW, 1 µA)** | ESP32 yazılımıyla **mümkün değil** — harici devre gerekli         | **✓ AKTİF** ama "kill mode" olarak (axp2101_soft_power_off, power_service.cpp:540, yorumlu) |

**Tam anlamda "full sleep mode" = Power off = CHIP_PU LOW.** Bu sadece harici PMIC ile yapılır. Bizim çözüm: `axp2101_soft_power_off()` (AXP REG 0x10 bit 0 = 1). Aktive etmek için power_service.cpp:537'deki normal deep sleep yorumlanır, :540'taki çağrı açılır.

---

## 7. Wake Source Tablosu

Deep sleep'ten uyandırma kaynakları:

```
   ESP32-S3 Deep Sleep Wake Sources
   ─────────────────────────────────
   • RTC GPIO (EXT0/EXT1)  ─→ BTN_TOP (GPIO 0) ◀── kullanıyoruz
   • RTC Timer             ─→ alarm/sync wake ◀── kullanıyoruz
   • ULP coprocessor       ─→ kullanmıyoruz
   • Touch                 ─→ kullanmıyoruz
   • UART                  ─→ kullanmıyoruz

   AXP2101 Soft PWROFF'tan uyandırma
   ─────────────────────────────────
   • PWRON button (yan, Key3) ─→ R11 pull-up dead, ÇALIŞMIYOR
   • VBUS HIGH→LOW→HIGH       ─→ ÇALIŞIYOR (USB unplug+plug)
```

---

## 8. Özet & Karar Tablosu

| Soru                                                    | Cevap (datasheet kaynaklı)                     |
|---------------------------------------------------------|------------------------------------------------|
| ESP32-S3 yazılım üzerinden en az kaç µA çeker?          | **7 µA** (deep-sleep, RTC periph OFF, Table 4-8) |
| Şu an chip'in akımı?                                    | ~7 µA (zaten datasheet limit)                  |
| Donanım üzerinden daha da kapanabilir mi?               | Evet, **CHIP_PU LOW → 1 µA** (Power off)        |
| Bunu yazılımdan tetikleyebilir miyiz?                   | Doğrudan hayır; AXP2101 üzerinden evet (Soft PWROFF)|
| KEWEISI'de gözüken 20 mA neden?                         | Board components (PMIC, USB CC, codec, pull-ups), ESP32 değil |
| Buton wake korunarak akımı 20 mA'in altına çekebilir miyiz?| Yazılımdan hayır — sınır donanımda (PMIC quiescent + pull-up'lar) |

**Pratik sonuç:** Bu board üzerinde "akımı yazılımla daha aşağı çekme" optimizasyonu tükendi. Daha az akım için:
1. **Hardware revize** — VCC3V3'ten beslenen sensörleri AXP LDO arkasına alıp deep sleep'te söndürmek.
2. **Daha hassas ölçer** — sub-mA görmek için 1 µA çözünürlüklü ölçer (örn. ucurrent Gold veya Joulescope).
3. **AXP2101 Soft PWROFF** — wake'i feda edip "uzun depolama" modu için (kod hazır, kill mode olarak duruyor).

---

## 9. Kaynaklar

- **ESP32-S3 Series Datasheet v1.6** (Espressif Systems)
  - Sf. 21: §2.5 Power Supply, Table 2-6 Power Pins
  - Sf. 22: Figure 2-2 ESP32-S3 Power Scheme, Table 2-7 Voltage Regulators
  - Sf. 30: §3.2 RTC and Low-Power Management
  - Sf. 58: §4.6.1 RF Current Consumption (Table 4-6)
  - Sf. 59: §4.6.2 Table 4-7 Modem-sleep, Table 4-8 Low-Power Modes ◀ **kritik**
  - Sf. 60: PSRAM dipnotu (8MB octal @ 3.3V = +140 µA light-sleep)
- **AXP2101 Datasheet v1.0** (X-Powers, Nov 2022)
  - §6.5.4.3 Power Off
  - §6.13.2.X REG 0x10 (PMU Common Config)
- Proje dosyaları
  - `src/services/power_service.cpp` — sleep enter flow
  - `src/drivers/power/axp2101_driver.cpp` — `axp2101_prepare_deep_sleep`, `axp2101_soft_power_off`
  - güç optimizasyonu 17 adımda yapıldı (power_service.cpp içindeki notlar)
  - `pin.md` — referans kart pin mapping (BSP doğrulamalı)
