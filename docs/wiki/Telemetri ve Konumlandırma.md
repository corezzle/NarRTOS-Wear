# Telemetri ve Konumlandırma

NarRTOS cihazları, periyodik olarak **sağlık verisi** ve **çevre WiFi taraması** raporlar.
Sunucu tarafı bu taramadan cihazın konumunu çözer — **GPS donanımı olmadan**.

## Cihaz tarafı (`telemetry_service`)

- **60 saniyede bir** rapor (WiFi bağlıysa)
- **5 turda bir** çevre WiFi taraması (sonuç önbelleğe alınır)
- Fire-and-forget: ağ/sunucu yoksa rapor atlanır, cihaz etkilenmez

```c
#define TELEMETRY_PERIOD_MS         (60 * 1000)  // rapor periyodu
#define TELEMETRY_SCAN_EVERY_CYCLES 5            // 5 turda bir tarama

static void telemetry_task(void* arg) {
    uint32_t cycle = 0;
    for (;;) {
        if (wifi_service_is_connected()) {
            if (cycle % TELEMETRY_SCAN_EVERY_CYCLES == 0)
                refresh_wifi_scan();   // BSSID listesini önbelleğe al
            else                       //   (bu tur rapor YOK — bağlantı toparlansın)
                send_report();         // önbellekli listeyle temiz HTTP POST
        }
        cycle++;
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}
```

> **Neden tarama ve rapor ayrı turlarda?** Aktif WiFi taraması bağlantıyı ~20 sn kesintiye
> uğratır. Aynı turda tarama + POST yapılırsa rapor hep başarısız olur.

### Gönderilen rapor

```json
{
  "device_id":        "E4B063A764CC",
  "wifi_rssi":        -55,
  "wifi":             [ {"mac":"A4:B1:C2:D3:E4:F5", "rssi":-53}, ... ],
  "uptime_s":         887,
  "firmware_version": "1.4.81",
  "heap_free":        970288
}
```

## Konum: SSID ile değil, BSSID (MAC) ile

Cihaz, çevredeki erişim noktalarının **ağ adlarını (SSID) değil, donanım MAC adreslerini
(BSSID)** toplar:

- SSID benzersiz değildir (binlerce aynı isim); MAC ise tekildir
- Geolocation sağlayıcılarının veritabanı "bu MAC'li modem şu koordinattadır" eşlemesini tutar
- Sinyal güçlerine göre ağırlıklandırılmış kesişim **±20 m** mertebesinde konum verir
- Tarama için ağlara **bağlanmak gerekmez** — yayınları duymak yeterlidir; şifre kullanılmaz,
  SSID gönderilmez

### Firmware API'si

```c
typedef struct { uint8_t bssid[6]; int8_t rssi; uint8_t channel; } wifi_bssid_ap_t;
esp_err_t wifi_service_scan_bssids(wifi_bssid_ap_t* out, uint8_t max, uint8_t* out_count);
```

Mevcut asenkron tarama (ayarlar ekranındaki ağ listesi) SSID döndürür; bu senkron API
geolocation için BSSID verir.

> **BT+WiFi coexistence:** Bluetooth açıkken özel tarama süreleri verilirse sürücü hata üretir.
> `scan_time` 0 bırakılmalı — sürücü coex-güvenli varsayılanı kendi seçer.

## Sunucu tarafı: konum çözümü

Sağlayıcı-bağımsız tasarlanmıştır. İstek/yanıt formatı **Ichnaea** standardıdır (Google
Geolocation API ile birebir aynı); sağlayıcı değişimi yalnızca iki ortam değişkenidir.

| Sağlayıcı | Sonuç |
|---|---|
| beaconDB (açık, anahtarsız) | Boru hattı çalışır; bölgesel kapsama zayıfsa IP fallback döner |
| Google Geolocation API | Gerçek çözüm: **±19 m** (doğrulandı) |

Çözüm nesnesi rapora gömülür:

```json
"geo": {
  "lat": 41.0289898, "lng": 29.2229154,
  "accuracy_m": 18.9,
  "fallback": null,              // null = GERÇEK WiFi çözümü; "ipf" = IP tahmini
  "provider": "www.googleapis.com"
}
```

### Veri dürüstlüğü kuralı

Sağlayıcı AP'leri tanımadığında **IP tabanlı kaba tahmine** düşebilir (±25 km — testlerde
İstanbul'daki bir cihazı farklı bir şehirde gösterdi). Kural:

- fallback'li sonuç yalnızca panelde gri bilgi olarak gösterilir
- **koordinat alanlarına yazılmaz**
- **konum kuyruğuna asla basılmaz**

Konum tüketen sistemlere kaba tahmin gitmesi mimari olarak imkânsızdır.

### Maliyet koruması

Google fiyatı: ayda 10.000 istek ücretsiz, sonrası 1.000 istekte 5 USD. Önlem alınmasaydı her
rapor bir API çağrısı olurdu (ayda ~43.000 istek/cihaz). **Önbellek** aynı AP kümesi için
(sıralı MAC seti anahtarı, 10 dk TTL) sonucu yeniden kullanır — sabit cihazda kullanım ayda
birkaç yüz çağrıya iner. API hatası/zaman aşımında rapor konumsuz devam eder; telemetri akışı
hiçbir koşulda konum servisine bağımlı değildir.

## Dış sistemlere yayın (mesaj kuyruğu)

Backend, raporu bir mesaj kuyruğuna (RabbitMQ) publish eder:

| Kuyruk | Ne zaman |
|---|---|
| `live-data-queue` | Her raporda (cihaz sağlığı verisi) |
| `location-data-queue` | YALNIZCA gerçek konum çözümü varsa (fallback'siz) |

### İletim durumu

| Durum | Anlamı |
|---|---|
| `dry_run` | İletim anahtarı kapalı — mesaj hazırlanır ve kaydedilir ama gönderilmez (güvenli prova modu) |
| `sent` | Mesaj kuyruğa iletildi |
| `dropped` | İletim denendi, başarısız (kalıcı kayıt yine tutulur) |

### Mesaj formatı

```json
{
  "deviceType": "TRACKER",              // genel izleme cihazı tipi
  "imei": "E4B063A764CC",
  "dateTime": "2026-07-21T01:46:48Z",
  "latitude": 41.0289898,               // yalnızca gerçek çözüm; yoksa 0
  "longitude": 29.2229154,
  "locationAccuracy": 18.9,
  "gsmSignalQuality": 20,
  "wifi": [ {"mac":"...","dbms":-53}, ... ],
  "softwareVersion": "1.4.81",
  "battery": null
}
```

Araç-özel alanlar (CAN, lastik basıncı, kontak, yakıt) bilinçli olarak **yoktur** — bu format
giyilebilir cihaz, hayvan, yaşlı ve eşya takibi gibi tüm GPS'siz senaryolar için ortaktır.
