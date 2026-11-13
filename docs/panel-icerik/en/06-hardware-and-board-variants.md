## Supported boards

| Feature | `esp32s3_wearable` (default) | `esp32s3_lcd128` | `nar_esp32s3` |
|---|---|---|---|
| MCU | ESP32-S3R8 (dual core, 240 MHz) | ESP32-S3 (240×240 round LCD board) | ESP32-S3R16V SiP (240 MHz, 16 MB PSRAM inside) |
| Flash / PSRAM | 8 MB QIO / 8 MB Octal PSRAM | 16 MB / 2 MB Quad PSRAM | 32 MB / 16 MB Octal PSRAM |
| Display | CO5300 AMOLED, QSPI, 410×502 | GC9A01A LCD, SPI, 240×240 round | CO5300 AMOLED, QSPI, 410×502 |
| Touch | FT3168 (I2C) | CST816S (I2C) | CST820 (I2C) |
| PMIC | AXP2101 | none (3V3 direct) | AXP2101 (0x47 linear variant) |
| RTC / IMU | PCF85063 / ICM42670 | depends on the board | PCF85063 (inside SiP) / ICM42670 |
| Extra | — | — | ES8311/ES7210 audio, **nRF9151 cellular** (GSM/LTE-M/NB-IoT + GNSS) |

## NAR-ESP32S3: audio and cellular (nRF9151)

The `nar_esp32s3` variant shares the AMOLED 2.06 display/touch family but adds **audio** (ES8311 codec + ES7210 mic ADC + NS4150B amp) and **cellular**: its "NAR9151" module is in fact a **Nordic nRF9151** SiP (LTE-M / NB-IoT + GNSS + embedded SIM).

- **AT interface:** the nRF9151 is on a UART to the ESP (nRF P0.11 TX → GPIO43, P0.12 RX → GPIO44). Nordic SLM firmware is flashed to the nRF for AT; `AT → OK` verified. (GPIO43/44 are the ESP's U0TXD/U0RXD; the ROM's secondary UART0 output must be detached from them first.)
- **Modem firmware:** the SiP ships with a factory test image (PTI) that does not support network/CFUN; the full release (`mfw_nrf91x1_2.0.4`) is flashed, after which `AT+CFUN=1 → OK`. With no J-Link, the ESP doubles as an SWD bridge (pyocd) + serial fmfu tool.
- **eSIM:** the embedded **ST4SIM** (MFF2) ships with a 1GLOBAL profile; its ICCID is read and activated in the operator portal (ST AN5740). Integration in progress.
- **PMIC 0x47:** NAR's AXP2101 is the linear-charger variant; LDOs boot off, the I2C lock is cleared with a blind write, and the driver writes the rails on every boot.

## On-boot hardware self-test

On a cold boot (except deep-sleep wake) the OS probes the peripherals in turn and prints a checklist table to the **serial monitor** (`selftest_service`) — it never draws to the screen, the launcher opens normally. Covered: PSRAM/flash, I2C scan, PMIC, power rails, USB/battery, IMU, temperature, RTC, touch chip, codec, display, SD. On the NAR variant it also covers audio loopback, Wi-Fi, vibration motor and **GSM/nRF9151 (AT → IMEI)**. Each line returns PASS / FAIL / WARN / SKIP; rows with no hardware/build show SKIP. The same test engine (`selftest_ext`) is shared verbatim with narrtos-board-selftest and narrtos-lte-test.

![On-boot hardware self-test output on the serial monitor](GORSEL: acilis-selftest.svg)

*The self-test table printed to the serial monitor on a cold boot — on a healthy board every line returns PASS (a missing or not-compiled peripheral shows SKIP/WARN instead of FAIL).*

## Pin map (default board)

Pin definitions live in exactly one place in the code (`core/os_config.h`) and every driver reads them from there:

```
#define PIN_DISP_CS     12      // QSPI ekran
#define PIN_DISP_SCLK   11
#define PIN_DISP_D0      4      // D0..D3 = 4/5/6/7
#define PIN_DISP_RST     8
#define PIN_I2C_SDA     15      // paylaşımlı I2C: dokunmatik + PMIC + RTC + IMU
#define PIN_I2C_SCL     14
#define PIN_TOUCH_RST    9
#define PIN_TOUCH_INT   38
#define PIN_BTN_TOP      0      // aktif LOW (boot butonu)
#define PIN_BTN_BOTTOM  10
#define PIN_BUZZER      21
// SD (SDMMC): D0=3, CMD=1, CLK=2
```

The LCD variant keeps its pins in a separate header: `core/os_config_lcd128_pins.h` — selected automatically when that variant is built.

## Drivers (`src/drivers/`)

| Driver | Chip | Interface | Note |
|---|---|---|---|
| `display/co5300` | CO5300 AMOLED | QSPI | Default display; brightness set with a panel command |
| `display/gc9a01_128` | GC9A01A LCD | SPI + DC | 1.28" variant; brightness via LEDC PWM (backlight) |
| `touch/ft3168` | FT3168 | I2C | Default touch controller |
| `touch/cst816s_128` | CST816S | I2C | Touch controller of the 1.28" variant |
| `power/axp2101` | AXP2101 PMIC | I2C | Power rails, battery measurement, charging |
| `rtc/pcf85063` | PCF85063 | I2C | Real-time clock (keeps time across power loss) |
| `imu/icm42670` | ICM42670 | I2C | Accelerometer/gyroscope — step counting and gestures |

## The variant pattern: one and the same symbol set

Alternative display drivers export **exactly the same function signatures** (`co5300_*`) as the default driver, and the whole file sits inside its own compile guard:

```
// gc9a01_128_driver.cpp
#ifdef DISPLAY_VARIANT_LCD128        // yalnız LCD varyantında derlenir
...  co5300_init(), co5300_lvgl_flush_cb(), ...   // aynı isimler
#endif
// co5300_driver.cpp'deki guard, LCD build'ında onu boşaltır ->
// linker her ortamda TEK sembol seti görür; main.cpp / ui_service DEĞİŞMEZ.
```

## Porting to a new board

The path an actual port (the 1.28" round LCD) followed:

**1. Display driver** — `drivers/display/<new>_driver.cpp` Implement the same set of functions the default driver exports, and wrap the entire file in its own guard (`#ifdef DISPLAY_VARIANT_X`). Do not hard-code the resolution — take it from the `CO5300_H_RES` / `V_RES` flags.

**2. Touch driver** — `drivers/touch/<new>_driver.cpp` Same pattern: the signatures of the default touch driver, plus a guard.

**3. Pin map** — `core/os_config_<new>_pins.h` `os_config.h` picks the right header based on the board guard.

**4. Build environment** — `platformio.ini`

```
[env:esp32s3_yeniboard]
extends = env:esp32s3_wearable        ; ortak her şeyi miras al
board_build.flash_size = 16MB         ; donanıma göre
board_build.psram_type = qio
upload_speed = 460800                 ; USB köprüsü kararsızsa düşür
build_flags =
    ${env:esp32s3_wearable.build_flags}
    -DCO5300_H_RES=240 -DCO5300_V_RES=240
    -DDISPLAY_VARIANT_YENI=1
    -DBOARD_VARIANT_YENI=1
```

**5. ESP-IDF configuration** — `sdkconfig.esp32s3_<new>` Chip-level settings such as PSRAM type (quad/octal) and flash size go into a file specific to that env.

**6. UI size constants** — see Application Development › Resolution portability.

### Checklist

- [ ] Display + touch driver pair, same symbol set, each in its own guard

- [ ] Pin header + platformio env + sdkconfig

- [ ] Both environments compile; **the default build's binary is UNCHANGED**

- [ ] Boot logs are clean; touch/display/services verified on the device

<div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Invariance rule</div><div style="line-height:1.6;color:inherit">Adding a variant must never affect how existing hardware builds. All new code belongs inside a guard, and the default branch must keep its original values.</div></div>

## The same system on a 240×240 round panel

The same codebase runs on the round GC9A01A panel purely through a compile-time selection. The layout values come from the firmware: content column `204 px` (18 px in from the edges), `STEP 60` in the main list, `STEP 50` on the power rails, launcher cell `88×92`.

<div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">The constraint of a round panel</div><div style="line-height:1.6;color:inherit">A 204 px wide row stays inside the circle only down to <strong>y ≈ 180</strong>; rows below that get clipped. That is why only two rows are fully visible at once in the main list, and why the power-rail rows are narrowed to 180 px.</div></div>

<div style="display:flex;gap:30px;justify-content:center;flex-wrap:wrap;margin:30px 0"><figure style="margin:0;max-width:260px;flex:0 1 auto;text-align:center"><img src="GORSEL:lcd128-tur.gif" alt="240×240 panel tour" style="width:100%;display:block;border-radius:0" /><figcaption style="font-size:12.5px;color:inherit;opacity:.72;margin-top:4px">Moving between screens on the round panel</figcaption></figure></div>

<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(148px,1fr));gap:22px 18px;margin:28px 0"><figure style="margin:0;text-align:center"><img src="GORSEL:kadran-dijital.png" alt="Digital watch face" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Digital watch face</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">Rubik Black 100</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:launcher.png" alt="Home screen" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Home screen</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">cell 88×92, icon 64</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:ayarlar.png" alt="Settings" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Settings</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">content 204 px, STEP 60</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:guc-raylari.png" alt="Power rails" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Power rails</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">STEP 50</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:adim-sayar.png" alt="Step counter" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Step counter</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">goal ring</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:hava-durumu.png" alt="Weather" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Weather</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">multiple cities</span></figure></div>
