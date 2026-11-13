# ESP32-S3-Touch-AMOLED-2.06 — Pin Bağlantı Haritası

Board: **ESP32-S3-Touch-AMOLED-2.06 Rev 1.1**
Üretici dökümanı: `ESP32-S3-Touch-AMOLED-2.06-datasheet.pdf` (kartın ürün sayfasından)
Doğrulama: Bu pinler hem datasheet PCB'sinden hem de mevcut firmware'in
[`src/core/os_config.h`](src/core/os_config.h) içindeki test edilmiş/çalışan
değerlerden derlendi.

---

## 1. MCU + RAM + Flash

| Bileşen | Detay |
|---|---|
| **MCU** | ESP32-S3R8 (Xtensa LX7 dual-core, @240 MHz) |
| **PSRAM** | 8 MB Octal SPI PSRAM (AP, generation 3) |
| **Flash** | 8 MB QIO (Datasheet) — runtime 32 MB header görüyor, image header 8 MB ile sınırlıyor |
| **Crystal (X1)** | 40 MHz main clock (board üzerinde, U2 yakını) |

> Not: Octal PSRAM **GPIO 33-37** (SPIQ/SPID/SPICLK/SPICS1/SPIHD) pinlerini
> kullanır → bu 5 pin uygulamada **kesinlikle kullanılamaz**.

---

## 2. USB-C Konnektörü (U9)

| Pin | İşlev | ESP32 GPIO |
|---|---|---|
| VBUS | 5V giriş | — (AXP2101 üzerinden işlenir) |
| D+ | USB Data + | **GPIO20** (USB-OTG D+) |
| D- | USB Data − | **GPIO19** (USB-OTG D−) |
| GND | Toprak | — |

USB-OTG native — USB-CDC console + DFU upload bu hat üzerinden gider.
PCB sağ tarafta "BOOT" ve "PWR" mekanik tuşları var (Key1, Key3).

---

## 3. PMIC — AXP2101 (I²C 0x34)

I²C üzerinden konfigüre edilir. **v1.4.48 (2026-05-15) güncelleme:** rail mapping
saatte fiziksel test ile doğrulandı, önceki bazı varsayımlar düzeltildi
.

| Rail | Voltaj | Bağlı bileşen | Test sonucu |
|---|---|---|---|
| **DCDC1** | 3.3 V | ESP32-S3 ana besleme (VCC3V3) + sistem rail | ❌ kapatma = saat ölür |
| **DCDC2** | 0.9 V | Display ile ilgili — kesilince ekran kararır | ✅ gerçek kesim güvenli |
| **DCDC3** | 1.2 V | Bağlı değil | — |
| **DCDC4** | 1.8 V | Bağlı değil | — |
| **ALDO1** | 3.3 V | Sensors (Touch + IMU + RTC) | ✅ kesim → touch ölür, 5sn sonra recovery |
| **ALDO2** | 3.3 V | Display panel (DSI_PWR_EN) | ⚠️ kesim display IC reset, simülasyon kullanılıyor |
| **ALDO3** | 3.3 V | Motor + sistem paralel (VCC3V3 backup) | ✅ kesim motor durdurur, sistem DCDC1'den hala besleniyor |
| **ALDO4** | 1.8 V | Bağlı değil | — |
| **BLDO1** | 1.2 V | Bağlı değil | — |
| **BLDO2** | 2.8 V | Bağlı değil | — |
| **CPUSLDO** | 1.2 V | ESP32 internal CPU LDO (kapatılamaz) | — |
| **DLDO1/2** | programlanabilir | Schematic'te bağlantı görülmedi | — |
| **VBUS** | 5V | USB'den şarj girişi | — |
| **BATT** | 3.7V LiPo | J1/J2 batarya konnektörü (pad var, batarya opsiyonel) | — |

**Önemli notlar:**
- Önceki "DCDC2 = Display IO 1.8V" varsayımı **yanlıştı** — schematic'te 0.9V, gerçek hedef belirsiz ama display ile bağlantılı (test kanıtı: kesim → ekran kararır).
- Önceki "ALDO1 = Audio" varsayımı **yanlıştı** — test kanıtladı ALDO1 sensors (touch, IMU, RTC) besleyici.
- Önceki "ALDO3 = Rezerv" varsayımı **yanlıştı** — schematic'te motor bloğu ALDO3 üzerinden besleniyor.
- ALDO4, DCDC3/4, BLDO1/2 board'da fiziksel olarak bağlı değil; toggle bunları kapatsa da etki yok.

| AXP2101 IRQ | ESP32 GPIO |
|---|---|
| PMIC_IRQ | **Bağlanmamış** (PIN_PMIC_IRQ = −1) — software polling kullanılır |

---

## 4. AMOLED Display — CO5300 / SH8601 (QSPI)

| Sinyal | ESP32 GPIO | Açıklama |
|---|---|---|
| DISP_CS   | **GPIO 12** | Chip select |
| DISP_SCLK | **GPIO 11** | QSPI clock |
| DISP_D0   | **GPIO  4** | QSPI data 0 |
| DISP_D1   | **GPIO  5** | QSPI data 1 |
| DISP_D2   | **GPIO  6** | QSPI data 2 |
| DISP_D3   | **GPIO  7** | QSPI data 3 |
| DISP_RST  | **GPIO  8** | Hardware reset (active LOW pulse) |
| DISP_TE   | **Bağlı değil** | Tear effect sinyali kullanılmıyor (PIN_DISP_TE = −1) |

| Özellik | Değer |
|---|---|
| Çözünürlük | 410 × 502 px |
| Renk derinliği | 24-bit RGB888 / 16-bit RGB565 (firmware RGB565 kullanıyor) |
| Power rail | ALDO2 (3.3 V) — AXP2101'den |

---

## 5. Touch Controller — FT3168 (I²C 0x38)

Kapasitif çoklu dokunma denetleyicisi.

| Sinyal | ESP32 GPIO | Açıklama |
|---|---|---|
| TOUCH_SDA | **GPIO 15** | I²C bus (paylaşımlı) |
| TOUCH_SCL | **GPIO 14** | I²C bus (paylaşımlı) |
| TOUCH_RST | **GPIO  9** | Hardware reset |
| TOUCH_INT | **GPIO 38** | Interrupt (active LOW) |
| Adres | 0x38 (I²C) | |
| Power rail | ALDO1 (3.3 V) | |

> Not: GPIO 38 **RTC-IO değil** → deep sleep'ten touch ile uyanma mümkün değil.
> Sadece UI task'ı aktifken touch çalışır.

---

## 6. RTC — PCF85063A/TL (I²C 0x51)

NXP gerçek zaman saati, ±3.5 ppm hassasiyet.

| Sinyal | ESP32 GPIO |
|---|---|
| SDA | GPIO 15 (paylaşımlı I²C) |
| SCL | GPIO 14 (paylaşımlı I²C) |
| INT | **Bağlı değil** (PCB üzerinde wake-capable GPIO'ya wire değil) |
| Adres | 0x51 |
| Power rail | ALDO1 (3.3 V) — uyku boyunca tutulması için pil veya ana rail gerekli |

> Not: Alarm IRQ pin'i ESP32'ye bağlı olmadığı için deep sleep'te RTC alarm
> ile uyandırma yapılamıyor — bu yüzden v1.0.1'de alarm wake için
> esp_sleep_enable_timer_wakeup() ile yazılım workaround'u kullanıldı.

---

## 7. IMU — 6/9 Axis (I²C 0x68)

Datasheet "6/9-Axis" bölümünde tanımlı. Firmware ICM-42670-P olarak deniyor
ama bazı board revizyonlarında **QMI8658** silikon konabiliyor — WHO_AM_I
register'ı ile ayırt edilir. Mevcut firmware'de I²C read başarısız
(`err=0xffffffff`) → IMU bu board'da fiziksel olarak ya farklı silikon ya da
montaj eksik.

| Sinyal | ESP32 GPIO |
|---|---|
| SDA | GPIO 15 (paylaşımlı I²C) |
| SCL | GPIO 14 (paylaşımlı I²C) |
| INT1 | **Bağlı değil** (PIN_IMU_INT1 = −1) |
| Adres | 0x68 (alternatif 0x69) |
| Power rail | ALDO1 (3.3 V) |
| X / Y / Z eksen | PCB sırt yüzeyinde U3/U5 yakını yön işaretleri var |

---

## 8. SD Card Slot (SDMMC 1-bit)

| Sinyal | ESP32 GPIO | Not |
|---|---|---|
| SD_CLK  | **GPIO  2** | SDMMC clock |
| SD_CMD  | **GPIO  1** | SDMMC command |
| SD_D0   | **GPIO  3** | SDMMC data 0 (1-bit mode — yalnız D0) |

> Not: 4-bit mode için D1/D2/D3 pinleri board'da çıkmamış → sadece 1-bit
> SDMMC mümkün. Pratik throughput ~5-10 MB/s, gallery için yeterli.

---

## 9. Tuş'lar (Mekanik Switch'ler)

| Tuş | ESP32 GPIO | PCB Label | Açıklama |
|---|---|---|---|
| **BOOT / TOP** | **GPIO 0** | Key1 ("BOOT") | Active LOW, boot button çift kullanım — kısa/uzun bas yazılımda |
| **PWR / BOTTOM** | **GPIO 10** | Key3 ("PWR") | Active LOW, yan buton — deep sleep wake için RTC-IO |

> İki tuş da **deep sleep wake-capable** RTC IO'larında (GPIO 0 ve GPIO 10).
> EXT1 wake source olarak kullanılıyor.

---

## 10. Buzzer / Speaker (Opsiyonel)

| Sinyal | ESP32 GPIO | Not |
|---|---|---|
| BUZZER | **GPIO 21** | LEDC PWM ile sürülür, alarm tonları için |

> Not: ESP32-S3'te dahili DAC yok → LEDC üzerinden square-wave PWM. Pasif
> piezo speaker veya buzzer GPIO 21 ile GND arasına bağlanır. Board'da
> sabit/dahili speaker yok; aşağıda ayrı **PA & SPEAKER** bölümü var.

---

## 11. PA & Speaker — Audio Power Amplifier (External)

Datasheet'te ayrı bir blok. Codec → PA → Speaker zinciri:

| Sinyal | Hedef |
|---|---|
| SPK +/− | Board'un alt-sağ köşesindeki **SPK** padları (P5, P6) — harici hoparlör için |
| AEC | Acoustic Echo Cancellation — codec bloğunun girişi |

I²S ile codec'e bağlı (codec block aşağıda). Speaker padları (SPK) opsiyonel
mini hoparlör için.

---

## 12. Codec (Audio I²S)

Datasheet'te ayrı bir Codec bloğu. Muhtemelen ES8311 veya benzeri (datasheet
schematic block'unda yongayı tam okuyamadım). I²S üzerinden çalışır:

| Sinyal | ESP32 GPIO | Not |
|---|---|---|
| I2S_MCLK / BCLK / LRCK / DIN / DOUT | Codec datasheet'inde detayda | Mevcut firmware kullanmıyor — audio_service sadece buzzer (GPIO 21) |

Audio gerekirse codec bloğunun pinleri üretici BSP/belgelerinden teyit edilmeli.
Firmware'de henüz codec sürücüsü yok.

---

## 13. Mikrofonlar — MIC1 + MIC2

İki harici PDM/analog mikrofon — board sırt yüzeyinde **MIC1** (sol üst) ve
**MIC2** (sol alt) etiketleri var.

| Mic | Konum | Tür |
|---|---|---|
| MIC1 | Üst sol | Analog/PDM (codec/ADC üzerinden işlenir) |
| MIC2 | Alt sol | Analog/PDM |

> Firmware'de henüz mikrofon kullanılmıyor; voice input için codec sürücüsü
> ve ses işleme entegrasyonu gerekli.

---

## 14. Vibration Motor

| Sinyal | Detay |
|---|---|
| Motor + / − | Board sırt yüzeyinde **MOTOR** etiketli iki pad (P1, P2) — opsiyonel titreşim motoru için |

Motor sürücüsü AXP2101'den veya ESP32 GPIO'sundan tetiklenebilir; mevcut
firmware'de bu özellik henüz implemente değil.

---

## 15. ADC

Datasheet'te ayrı bir ADC bloğu var. Anlaşılan birden fazla analog sinyalin
(MIC, batarya voltajı, vb.) sayısallaştırılması için. Spesifik bağlantılar
schematic'in küçük yazıları okunmadığından net değil — firmware'in ihtiyacı
olduğunda üretici BSP'sine bakılmalı.

---

## 16. Breakout Header (PCB Alt Kenar)

PCB'nin alt kenarında 10 pinlik breakout headeri var (sırasıyla soldan sağa):

| Etiket | ESP32 GPIO | İşlev |
|---|---|---|
| **VBUS** | — | USB 5V çıkış (input olarak da kullanılabilir) |
| **GND** | — | Toprak |
| **D+/IO20** | GPIO 20 | USB D+ veya genel GPIO |
| **D−/IO19** | GPIO 19 | USB D− veya genel GPIO |
| **IO15** | GPIO 15 | **Paylaşımlı I²C SDA** — dikkat: dış cihaz takmadan önce I²C zaten yüklü |
| **IO14** | GPIO 14 | **Paylaşımlı I²C SCL** — aynı uyarı |
| **RXD** | UART RX | Console / debug RX (varsayılan UART0) |
| **TXD** | UART TX | Console / debug TX |
| **GND** | — | Toprak |
| **3V3** | — | 3.3V çıkış (board içinden, ~250 mA kapasiteli) |

> Not: IO14 ve IO15 board içinde I²C bus'ı için kullanılıyor. Bu pinleri
> breakout'tan dış cihaz için kullanmak isterseniz I²C bus üzerinde
> conflict olur (AXP2101, FT3168, IMU, RTC adres çakışması).

---

## 17. Batarya Bağlantısı

| Konum | Etiket | Detay |
|---|---|---|
| PCB sırt yüzeyi | **J1** ("BAT") | LiPo batarya konnektörü (pad) — board satılırken batarya genelde yok |
| Voltaj | 3.7 V nominal | AXP2101 şarj girişi |

> Not: Batarya konnektörü PCB üzerinde **pad** olarak duruyor — direkt
> JST konnektör veya lehimleme gerekir. Yönetici/donanım ekibine "üretime
> hazır revizyon batarya konnektörlü mü?" diye sorulabilir.

---

## 18. Tam GPIO Tahsisi Özeti (kullanılan ESP32 pinleri)

| GPIO | Birincil İşlev | Notlar |
|---:|---|---|
| 0  | BTN_TOP (BOOT) | Active LOW, RTC-IO wake-capable |
| 1  | SD_CMD | SDMMC |
| 2  | SD_CLK | SDMMC |
| 3  | SD_D0 | SDMMC 1-bit |
| 4  | DISP_D0 | QSPI display |
| 5  | DISP_D1 | QSPI display |
| 6  | DISP_D2 | QSPI display |
| 7  | DISP_D3 | QSPI display |
| 8  | DISP_RST | Display reset |
| 9  | TOUCH_RST | FT3168 reset |
| 10 | BTN_BOTTOM (PWR) | Active LOW, RTC-IO wake-capable |
| 11 | DISP_SCLK | QSPI display |
| 12 | DISP_CS | QSPI display |
| 14 | I²C_SCL | Shared bus (AXP2101 + FT3168 + IMU + RTC) |
| 15 | I²C_SDA | Shared bus |
| 19 | USB D− | USB-OTG (veya breakout GPIO) |
| 20 | USB D+ | USB-OTG (veya breakout GPIO) |
| 21 | BUZZER | LEDC PWM |
| 38 | TOUCH_INT | FT3168 interrupt (NOT RTC-IO) |

| GPIO | Durum |
|---:|---|
| 13, 16, 17, 18 | **Boş — kullanıma açık** (RTC-IO veya genel I/O) |
| 33, 34, 35, 36, 37 | **Kullanılamaz** — Octal PSRAM tarafından rezerve |
| 39, 40, 41, 42 | **Boş — kullanıma açık** (RTC-IO veya genel I/O) |
| 43 (U0TXD), 44 (U0RXD) | Console UART (breakout TXD/RXD) |
| 45, 46, 47, 48 | **Boş — kullanıma açık** (ama 45/46 strapping pinleri, dikkat) |

---

## 19. I²C Adres Haritası (paylaşımlı bus)

| Adres | Cihaz | Power |
|---|---|---|
| 0x34 | AXP2101 PMIC | her zaman açık (kendi besleme) |
| 0x38 | FT3168 Touch | ALDO1 |
| 0x51 | PCF85063 RTC | ALDO1 |
| 0x68 | IMU (ICM-42670 / QMI8658) | ALDO1 |

Bus frekansı: 400 kHz (Fast Mode), pull-up board'da entegre.

---

## 20. Uyku ve Wake Kaynakları

Deep sleep'te aktif kalan / wake-capable bileşenler:

| Wake Source | Mevcut Durum |
|---|---|
| EXT1 (BTN_TOP GPIO 0) | ✅ Aktif |
| EXT1 (BTN_BOTTOM GPIO 10) | ✅ Aktif |
| Timer (RTC) | ✅ Aktif — OTA sync + alarm wake için |
| Touch (FT3168 INT) | ❌ GPIO 38 RTC-IO değil |
| RTC alarm (PCF85063 INT) | ❌ INT pin board'da wake-capable GPIO'ya wire'lı değil |
| IMU WOM (wake-on-motion) | ❌ INT1 bağlı değil + IMU silikonu da sorunlu |
| PMIC IRQ | ❌ PIN_PMIC_IRQ bağlı değil |

---

## Kaynak Atıfları

- **Datasheet**: `ESP32-S3-Touch-AMOLED-2.06-datasheet.pdf` (kartın ürün sayfasından) — board sketch + schematic block diagram
- **Firmware (verified)**: [`src/core/os_config.h`](src/core/os_config.h:74) — GPIO_PIN_MAP bölümü
- **Üretici belgeleri (referans)**: kartın ürün wiki sayfası (datasheet ek detay için)
