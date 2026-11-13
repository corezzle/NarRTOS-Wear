## Desteklenen board'lar

| Özellik | `esp32s3_wearable` (varsayılan) | `esp32s3_lcd128` | `nar_esp32s3` |
|---|---|---|---|
| MCU | ESP32-S3R8 (çift çekirdek, 240 MHz) | ESP32-S3 (240×240 yuvarlak LCD kartı) | ESP32-S3R16V SiP (240 MHz, 16 MB PSRAM dahili) |
| Flash / PSRAM | 8 MB QIO / 8 MB Octal PSRAM | 16 MB / 2 MB Quad PSRAM | 32 MB / 16 MB Octal PSRAM |
| Ekran | CO5300 AMOLED, QSPI, 410×502 | GC9A01A LCD, SPI, 240×240 yuvarlak | CO5300 AMOLED, QSPI, 410×502 |
| Dokunmatik | FT3168 (I2C) | CST816S (I2C) | CST820 (I2C) |
| PMIC | AXP2101 | yok (3V3 doğrudan) | AXP2101 (0x47 lineer varyant) |
| RTC / IMU | PCF85063 / ICM42670 | board'a göre | PCF85063 (SiP içinde) / ICM42670 |
| Ek | — | — | ES8311/ES7210 ses, **nRF9151 hücresel** (GSM/LTE-M/NB-IoT + GNSS) |

## NAR-ESP32S3: ses ve hücresel (nRF9151)

`nar_esp32s3` varyantı AMOLED 2.06 kartla aynı ekran/dokunmatik ailesini kullanır ama ek olarak **ses** (ES8311 kodek + ES7210 mik ADC + NS4150B amfi) ve **hücresel bağlantı** taşır: üzerindeki "NAR9151" modülü aslında bir **Nordic nRF9151** SiP'tir (LTE-M / NB-IoT + GNSS + gömülü SIM).

- **AT arayüzü:** nRF9151 UART ile ESP'ye bağlı (nRF P0.11 TX → GPIO43, P0.12 RX → GPIO44). AT için nRF'e Nordic SLM yazılımı yüklenir; `AT → OK` doğrulandı. (GPIO43/44 = ESP U0TXD/U0RXD; kullanmadan önce ROM'un UART0 ikincil çıkışı pinlerden koparılmalı.)

- **Modem firmware:** SiP fabrikadan test görüntüsüyle (PTI) gelir ve şebeke/CFUN desteklemez; tam sürüm (`mfw_nrf91x1_2.0.4`) yüklenir, sonra `AT+CFUN=1 → OK`. J-Link yoksa ESP, SWD köprüsü (pyocd) + seri fmfu aracı olarak kullanılır.

- **eSIM:** gömülü **ST4SIM** (MFF2) 1GLOBAL profiliyle gelir; ICCID okunup operatör portalında aktive edilir (ST AN5740). Entegrasyon sürüyor.

- **PMIC 0x47:** NAR'ın AXP2101'i lineer şarj varyantıdır; LDO'lar kapalı açılır, I2C kilidi kör-yazma ile açılır, sürücü her açılışta rayları yazar.

## Açılış donanım self-test'i

Soğuk açılışta (deep-sleep uyanışı hariç) OS, çevrebirimleri sırayla sınayıp **seri monitöre** bir checklist tablosu basar (`selftest_service`) — ekrana çizmez, launcher normal açılır. Test edilenler: PSRAM/flash, I2C taraması, PMIC, güç rayları, USB/pil, IMU, sıcaklık, RTC, dokunmatik çip, kodek, ekran, SD. NAR varyantında ek olarak ses loopback, Wi-Fi, titreşim motoru ve **GSM/nRF9151 (AT → IMEI)**. Her satır PASS / FAIL / UYARI / ATLA döner; donanımı/derlemesi olmayan satırlar ATLA görünür. Aynı test motoru (`selftest_ext`) narrtos-board-selftest ve narrtos-lte-test ile birebir ortaktır.

![Açılış donanım self-test'i seri monitör çıktısı](GORSEL: boot-selftest.svg)

*Soğuk açılışta seri monitöre basılan self-test tablosu — sağlam bir kartta tüm satırlar PASS döner (eksik/derlenmemiş çevrebirim olursa o satır FAIL yerine ATLA/UYARI).*

## Pin haritası (varsayılan board)

Pin tanımları kodda tek yerde yaşar (`core/os_config.h`) ve tüm sürücüler buradan okur:

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

LCD varyantının pinleri ayrı başlıktadır: `core/os_config_lcd128_pins.h` — varyant derlemesinde otomatik seçilir.

## Sürücüler (`src/drivers/`)

| Sürücü | Çip | Arayüz | Not |
|---|---|---|---|
| `display/co5300` | CO5300 AMOLED | QSPI | Varsayılan ekran; parlaklık panel komutuyla |
| `display/gc9a01_128` | GC9A01A LCD | SPI + DC | 1.28" varyant; parlaklık LEDC PWM (arka ışık) |
| `touch/ft3168` | FT3168 | I2C | Varsayılan dokunmatik |
| `touch/cst816s_128` | CST816S | I2C | 1.28" varyant dokunmatiği |
| `power/axp2101` | AXP2101 PMIC | I2C | Güç rayları, batarya ölçümü, şarj |
| `rtc/pcf85063` | PCF85063 | I2C | Gerçek zaman saati (güç kesilse de saat korunur) |
| `imu/icm42670` | ICM42670 | I2C | İvme/jiroskop — adım sayar ve jestler |

## Varyant kalıbı: aynı sembol seti

Alternatif ekran sürücüleri, varsayılan sürücüyle **birebir aynı fonksiyon imzalarını** (`co5300_*`) dışa verir ve tamamı kendi derleme guard'ı içindedir:

```
// gc9a01_128_driver.cpp
#ifdef DISPLAY_VARIANT_LCD128        // yalnız LCD varyantında derlenir
...  co5300_init(), co5300_lvgl_flush_cb(), ...   // aynı isimler
#endif
// co5300_driver.cpp'deki guard, LCD build'ında onu boşaltır ->
// linker her ortamda TEK sembol seti görür; main.cpp / ui_service DEĞİŞMEZ.
```

## Yeni board'a taşıma (porting)

Gerçekte yapılmış bir taşımanın (1.28" yuvarlak LCD) izlediği yol:

**1. Ekran sürücüsü** — `drivers/display/<yeni>_driver.cpp` Varsayılan sürücünün dışa verdiği aynı fonksiyon setini uygula, dosyanın tamamını kendi guard'ına al (`#ifdef DISPLAY_VARIANT_X`). Çözünürlüğü sabit yazma — `CO5300_H_RES` / `V_RES` bayraklarından al.

**2. Dokunmatik sürücüsü** — `drivers/touch/<yeni>_driver.cpp` Aynı kalıp: varsayılan dokunmatik sürücüsünün imzaları + guard.

**3. Pin haritası** — `core/os_config_<yeni>_pins.h` `os_config.h`, board guard'ına göre doğru başlığı seçer.

**4. Build ortamı** — `platformio.ini`

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

**5. ESP-IDF konfigürasyonu** — `sdkconfig.esp32s3_yeniboard` PSRAM tipi (quad/octal), flash boyutu gibi çip-seviyesi ayarlar env'e özel dosyada.

**6. UI ölçü sabitleri** — bkz. Uygulama Geliştirme › Çözünürlük uyumu.

### Kontrol listesi

- [ ] Ekran + dokunmatik sürücü çifti aynı sembol setiyle, kendi guard'ında

- [ ] Pin başlığı + platformio env + sdkconfig

- [ ] Her iki ortam da derleniyor; **varsayılan build'ın binary'si DEĞİŞMEDİ**

- [ ] Boot logları temiz; dokunmatik/ekran/servisler cihazda doğrulandı

<div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Değişmezlik kuralı</div><div style="line-height:1.6;color:inherit">Varyant eklemek, mevcut donanımın derlemesini asla etkilememelidir. Yeni kodun tamamı guard içinde olmalı, varsayılan dal orijinal değerlerini korumalıdır.</div></div>

## 240×240 yuvarlak panelde aynı sistem

Aynı kod tabanı, yalnızca derleme zamanı seçimiyle yuvarlak GC9A01A panelde çalışır. Yerleşim değerleri firmware'den gelir: içerik sütunu `204 px` (kenarlardan 18 px içeride), ana listede `STEP 60`, güç raylarında `STEP 50`, launcher hücresi `88×92`.

<div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Yuvarlak panelin kısıtı</div><div style="line-height:1.6;color:inherit">204 px genişliğindeki satır ancak <strong>y ≈ 180</strong>'e kadar daire içinde kalır; altına inen satırlar kırpılır. Bu yüzden ana listede aynı anda iki satır tam görünür, güç rayları satırları 180 px'e daraltılır.</div></div>

<div style="display:flex;gap:30px;justify-content:center;flex-wrap:wrap;margin:30px 0"><figure style="margin:0;max-width:260px;flex:0 1 auto;text-align:center"><img src="GORSEL:lcd128-tour.gif" alt="240×240 panel turu" style="width:100%;display:block;border-radius:0" /><figcaption style="font-size:12.5px;color:inherit;opacity:.72;margin-top:4px">Yuvarlak panelde ekranlar arası geçiş</figcaption></figure></div>

<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(148px,1fr));gap:22px 18px;margin:28px 0"><figure style="margin:0;text-align:center"><img src="GORSEL:watchface-digital.png" alt="Dijital kadran" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Dijital kadran</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">Rubik Black 100</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:launcher.png" alt="Ana ekran" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Ana ekran</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">hücre 88×92, ikon 64</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:settings.png" alt="Ayarlar" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Ayarlar</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">içerik 204 px, STEP 60</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:power-rails.png" alt="Güç rayları" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Güç rayları</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">STEP 50</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:step-counter.png" alt="Adım sayar" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Adım sayar</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">hedef halkası</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:weather.png" alt="Hava durumu" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Hava durumu</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">çoklu şehir</span></figure></div>
