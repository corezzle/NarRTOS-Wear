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
> olduğunu varsayar. Hiç yapmadıysanız [[Başlarken]] sayfasından gidin. Python
> ve PlatformIO kurulumundan kabloyu takmaya kadar her adım tek tek anlatılıyor.

---

## Dokümantasyon haritası

**Temeller**
- [[Başlarken]] — gereksinimler, derleme, yükleme, ilk açılış
- [[Mimari]] — katmanlar, görev dağılımı, boot akışı, LVGL eşzamanlılık
- [[Çekirdek ve Servisler]] — event bus, app manager, servis katmanı

**Donanım**
- [[Donanım ve Board Varyantları]] — desteklenen board'lar, pin haritası, yeni board'a taşıma

**Geliştirme**
- [[Uygulama Geliştirme]] — 4 adımda yeni uygulama, kurallar, çözünürlük uyumu
- [[Kullanıcı Arayüzü]] — jestler, Control Center, güç durumları, yerleşik uygulamalar

**Yetenekler**
- [[Komut Arayüzü]] — USB + BLE ile uzaktan kontrol
- [[Telemetri ve Konumlandırma]] — cihaz sağlığı + WiFi konumlandırma
- [[OTA Güncelleme]] — manifest, A/B akışı

**Yardım**
- [[Sorun Giderme]] — sık karşılaşılan sorunlar ve çözümleri

---

## PDF dokümantasyon

Tüm sistemin tek dosyada toplanmış hali: `docs/narrtos-dokumantasyonu.pdf`
(üretici script: `docs/narrtos_doc_gen.py`)

---

## Lisans ve katkı

NarRTOS açık kaynaktır. Hata bildirimi, öneri ve katkılar için issue açabilir veya
merge request gönderebilirsiniz.
