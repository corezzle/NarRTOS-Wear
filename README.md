# NarRTOS Wear

**ESP32-S3 için açık kaynak gömülü işletim sistemi.** FreeRTOS çekirdeği üzerinde,
LVGL 9 grafik kütüphanesiyle dokunmatik ekranlı akıllı cihazlar (giyilebilir cihazlar,
izleme cihazları, kontrol panelleri) için eksiksiz bir çalışma ortamı.

*NarRTOS Wear is an open-source embedded OS for ESP32-S3 wearables: dual-core FreeRTOS,
LVGL 9 UI, modular app framework, OTA updates, telemetry, BLE/USB command interface.
Documentation is in Turkish (`docs/panel-icerik/`) with English versions under
`docs/panel-icerik/en/`.*

Kardeş proje: [NarRTOS Drive](https://github.com/corezzle/NarRTOS-Drive) (ekransız araç takip cihazı).

## Ne sunar?

| | |
|---|---|
| **Çift çekirdekli görev mimarisi** | Arayüz çizimi ve sensör/servis işleri ayrı çekirdeklerde |
| **Modüler uygulama çatısı** | Uygulamalar bir yaşam döngüsü arayüzüyle kaydolur (launcher, kadran, ayarlar, alarm, galeri, adım sayar, hava durumu, bildirimler, mağaza) |
| **Olay tabanlı iletişim** | Hafif yayınla/abone ol veriyolu; servisler birbirini doğrudan çağırmaz |
| **Board varyant sistemi** | Aynı kod tabanı farklı ekran/dokunmatik donanımında çalışır |
| **Hazır servisler** | Güç (AXP2101), zaman (RTC+NTP), WiFi, BLE (ANCS bildirim aynalama), ses, IMU (ICM-42670 APEX), OTA, telemetri |
| **Kablosuz güncelleme** | A/B partisyonlu OTA; manifest tabanlı sürüm yönetimi |
| **Telemetri + konumlandırma** | GPS olmadan WiFi tabanlı konum; isteğe bağlı GPS ve ham CAN |
| **Komut arayüzü** | USB seri veya BLE üzerinden aynı metin komutları (`help`) |

## Desteklenen kartlar

| PlatformIO ortamı | Donanım |
|---|---|
| `esp32s3_wearable` | ESP32-S3, 2.06" AMOLED 410×502 (CO5300), dokunmatik, AXP2101 PMIC |
| `esp32s3_18inch` | ESP32-S3, 1.8" AMOLED (SH8601) |
| `esp32s3_lcd128` | ESP32-S3, 1.28" yuvarlak LCD 240×240 (GC9A01A, CST816S) |
| `nar_esp32s3` | NAR-ESP32S3 geliştirme kartı (ekransız; IMU, mikrofon, CAN, hücresel modül) |

Pin haritaları `src/core/os_config.h` ve `pin.md` içinde; yeni bir karta taşıma
`docs/panel-icerik/06-donanim-ve-board-varyantlari.md` sayfasında anlatılıyor.

## Hızlı başlangıç

PlatformIO kuruluysa ve kart USB ile bağlıysa:

```bash
git clone https://github.com/corezzle/NarRTOS-Wear.git
cd NarRTOS-Wear
pio run -e esp32s3_wearable -t upload
pio device monitor
```

İlk kez kuruyorsanız `docs/panel-icerik/03-baslarken.md` (Python, PlatformIO ve VS Code
kurulumundan ilk açılışa kadar adım adım). Tarayıcıdan, hiçbir program kurmadan yükleme
için `docs/web-install/`.

OTA ve telemetri için bir sunucu adresi gerekir; `platformio.ini` içindeki
`OTA_BACKEND_HOST` örnek bir yerel adres taşır, kendi sunucunuza göre değiştirin ya da
cihazda `set backend http://<ip>:3001` komutuyla kalıcı olarak ayarlayın.

## Belgeler

- `docs/panel-icerik/` — tam belge seti (TR), `docs/panel-icerik/en/` (EN)
- `docs/wiki/` — aynı içeriğin wiki biçimi; `docs/wiki-html/` tek dosya HTML sürüm
- `docs/video/` — tanıtım ve eğitim videolarının kaynakları
- `pin.md`, `esp32s3_power_map.md` — donanım referansı

Başlıca konular: mimari, çekirdek ve servisler, uygulama geliştirme, kullanıcı arayüzü,
komut arayüzü, telemetri ve konumlandırma, OTA, sunucu kurulumu, sorun giderme.

## Dizin yapısı

```
src/core/       app manager, event bus, OS tick, yapılandırma ve pin haritaları
src/services/   güç, zaman, WiFi, BLE, OTA, telemetri, sensör, ses, komut, CAN, ...
src/drivers/    ekran, dokunmatik, PMIC, IMU, RTC, ses kodek, CAN (TWAI)
src/apps/       yerleşik uygulamalar
src/assets/     fontlar, simgeler, logolar
tools/          seri/BLE yardımcıları, ölçüm ve test betikleri
scripts/        sürüm paketleyici (release.py), yardımcı betikler
docs/           belgeler
```

## Sürüm paketleme

`scripts/release.py` her ortam için OTA imajı, tek dosya tam flash imajı ve tarayıcıdan
yükleme manifestlerini üretir; `--upload` ile OTA sunucusuna gönderir
(`OTA_ADMIN_TOKEN` ortam değişkeni gerekir).

## Üçüncü taraf bileşenler

- [ESP-IDF](https://github.com/espressif/esp-idf) (Apache-2.0)
- [LVGL 9](https://github.com/lvgl/lvgl) (MIT)
- Nunito ve Rubik yazı tipleri (SIL Open Font License)
- Ekran sürücülerindeki başlatma dizileri panel üreticilerinin referans örneklerinden alınmıştır.

## Lisans

Apache License 2.0. Ayrıntı için `LICENSE` ve `NOTICE` dosyalarına bakın.
Kodu kullanırken, değiştirirken ve dağıtırken telif satırını ve lisans metnini korumanız yeterlidir.

---

Corezzle Electronics
