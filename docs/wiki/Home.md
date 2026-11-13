# NarRTOS

**ESP32-S3 için açık kaynak gömülü işletim sistemi.**
FreeRTOS çekirdeği üzerinde, LVGL 9 grafik kütüphanesiyle dokunmatik ekranlı akıllı
cihazlar (giyilebilir cihazlar, izleme cihazları, kontrol panelleri) için eksiksiz bir
çalışma ortamı.

```
Sürüm: v1.4.81   •   Platform: ESP32-S3   •   Grafik: LVGL 9   •   Çekirdek: FreeRTOS
```

---

## Ne sunar?

| | |
|---|---|
| **Çift çekirdekli görev mimarisi** | Arayüz çizimi ve sensör/servis işleri ayrı çekirdeklerde, çakışmadan |
| **Modüler uygulama çatısı** | Uygulamalar bir yaşam döngüsü arayüzüyle kaydolur; ekranın sahibi her an tek uygulama |
| **Olay tabanlı iletişim** | Hafif yayınla/abone ol veriyolu — servisler birbirini doğrudan çağırmaz |
| **Board varyant sistemi** | Aynı kod tabanı farklı ekran/dokunmatik donanımında çalışır, uygulama kodu değişmez |
| **Hazır servisler** | Güç, zaman (RTC+NTP), WiFi, BLE, ses, sensör, OTA |
| **Kablosuz güncelleme** | Cihaz kendini alandan günceller (A/B partisyon) |
| **Telemetri + konumlandırma** | GPS olmadan WiFi tabanlı konum (~20 m) |
| **Komut arayüzü** | USB seri veya telefondan (BLE) cihazı uzaktan sürme |

---

## Hızlı başlangıç

Bilgisayarınızda PlatformIO kuruluysa ve kart bağlıysa dört komut yeter:

```bash
git clone https://github.com/corezzle/NarRTOS-Wear.git
cd NarRTOS-Wear
pio run -e esp32s3_wearable -t upload
pio device monitor
```

Sırasıyla: projeyi indirir, klasörüne girer, derleyip karta yükler, cihazın
çıktısını dinler.

> **İlk kez mi kuruyorsunuz?** Bu dört komut, gerekli programların zaten kurulu
> olduğunu varsayar. Hiç yapmadıysanız [[Başlarken|getting-started]] sayfasından gidin. Python
> ve PlatformIO kurulumundan kabloyu takmaya kadar her adım tek tek anlatılıyor.

---

## Dokümantasyon haritası

**Temeller**
- [[Başlarken|getting-started]] — gereksinimler, derleme, yükleme, ilk açılış
- [[Mimari|architecture]] — katmanlar, görev dağılımı, boot akışı, LVGL eşzamanlılık
- [[Çekirdek ve Servisler|core-and-services]] — event bus, app manager, servis katmanı

**Donanım**
- [[Donanım ve Board Varyantları|hardware-and-board-variants]] — desteklenen board'lar, pin haritası, yeni board'a taşıma

**Geliştirme**
- [[Uygulama Geliştirme|app-development]] — 4 adımda yeni uygulama, kurallar, çözünürlük uyumu
- [[Kullanıcı Arayüzü|user-interface]] — jestler, Control Center, güç durumları, yerleşik uygulamalar

**Yetenekler**
- [[Komut Arayüzü|command-interface]] — USB + BLE ile uzaktan kontrol
- [[Telemetri ve Konumlandırma|telemetry-and-positioning]] — cihaz sağlığı + WiFi konumlandırma
- [[OTA Güncelleme|ota-updates]] — manifest, A/B akışı

**Yardım**
- [[Sorun Giderme|troubleshooting]] — sık karşılaşılan sorunlar ve çözümleri

---

## PDF dokümantasyon

Tüm sistemin tek dosyada toplanmış hali: `docs/narrtos-dokumantasyonu.pdf`
(üretici script: `docs/narrtos_doc_gen.py`)

---

## Lisans ve katkı

NarRTOS açık kaynaktır. Hata bildirimi, öneri ve katkılar için issue açabilir veya
merge request gönderebilirsiniz.
