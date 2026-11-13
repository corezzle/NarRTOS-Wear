NarRTOS cihazları periyodik olarak sağlık verisi ve çevre WiFi taraması raporlar. Sunucu tarafı bu taramadan cihazın konumunu çözer — **GPS donanımı olmadan**, yaklaşık **±19 metre** hassasiyetle.

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Bu sayfa cihaz tarafını anlatır</div><div style="line-height:1.6;color:inherit">Sunucuyu kurmak, konum sağlayıcısını seçmek ve mesaj kuyruğunu bağlamak için <a href="/document-content/narrtos-sunucu-kurulumu-8">Sunucu Kurulumu</a> sayfasından devam edin.</div></div>

## Nasıl çalışır

Zincirin tamamı beş adımdır. Cihaz yalnızca ilk ikisinden sorumludur: ölçmek ve göndermek. Konum çözümü ve dış sistemlere yayın sunucu tarafında olur.

![Telemetri boru hattı animasyonu](GORSEL: pipeline.gif)

*Bir raporun cihazdan mesaj kuyruğuna kadar izlediği yol*

<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:14px;margin:18px 0"><div style="background:rgba(128,128,128,.08);border:1px solid rgba(128,128,128,.24);border-radius:14px;padding:20px"><div style="font-size:11.5px;font-weight:800;letter-spacing:1.2px;text-transform:uppercase;color:#12A0AD">Periyot</div><div style="font-size:17px;font-weight:700;margin:8px 0 6px;color:inherit">60 saniye</div><div style="font-size:14px;color:inherit;opacity:.72;line-height:1.5">WiFi bağlıysa her turda bir rapor denenir.</div></div><div style="background:rgba(128,128,128,.08);border:1px solid rgba(128,128,128,.24);border-radius:14px;padding:20px"><div style="font-size:11.5px;font-weight:800;letter-spacing:1.2px;text-transform:uppercase;color:#12A0AD">Tarama</div><div style="font-size:17px;font-weight:700;margin:8px 0 6px;color:inherit">5 turda bir</div><div style="font-size:14px;color:inherit;opacity:.72;line-height:1.5">Çevre BSSID listesi tazelenir ve önbelleğe alınır.</div></div><div style="background:rgba(128,128,128,.08);border:1px solid rgba(128,128,128,.24);border-radius:14px;padding:20px"><div style="font-size:11.5px;font-weight:800;letter-spacing:1.2px;text-transform:uppercase;color:#12A0AD">Hata davranışı</div><div style="font-size:17px;font-weight:700;margin:8px 0 6px;color:inherit">Fire-and-forget</div><div style="font-size:14px;color:inherit;opacity:.72;line-height:1.5">Ağ ya da sunucu yoksa rapor atlanır; cihaz etkilenmez.</div></div></div>

## Tarama ve rapor neden ayrı turlarda?

Aktif WiFi taraması bağlantıyı **yaklaşık 20 saniye** kesintiye uğratır. Aynı turda hem tarama hem POST yapılırsa rapor hep başarısız olur. Bu yüzden tarama turu yalnızca listeyi tazeler, gönderim bir sonraki turlara bırakılır.

![Tarama ve rapor döngüsü animasyonu](GORSEL: scan-cycle.gif)

*Altı turluk döngü ve her turda üretilen seri port çıktısı*

```
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

## Konum: SSID ile değil, BSSID ile

Cihaz, çevredeki erişim noktalarının **ağ adlarını (SSID) değil, donanım MAC adreslerini (BSSID)** toplar. Sebebi basit: SSID benzersiz değildir, MAC ise tekildir.

![BSSID ile konum çözümü](GORSEL: 02-bssid-positioning.png)

*Sinyal güçlerine göre ağırlıklandırılmış kesişim — yaklaşık ±19 m*

- SSID benzersiz değildir; binlerce modem aynı ismi taşır.

- Geolocation sağlayıcılarının veritabanı *"bu MAC'li modem şu koordinattadır"* eşlemesini tutar.

- Tarama için ağlara **bağlanmak gerekmez** — yayınları duymak yeterlidir. Şifre kullanılmaz, SSID gönderilmez.

```
typedef struct { uint8_t bssid[6]; int8_t rssi; uint8_t channel; } wifi_bssid_ap_t;

esp_err_t wifi_service_scan_bssids(wifi_bssid_ap_t* out, uint8_t max, uint8_t* out_count);
```

### Bluetooth açıkken tarama tuzağı

BT+WiFi coexistence altında sürücüye özel tarama süreleri verilirse hata üretir. `scan_time` alanı **0 bırakılmalıdır** — sürücü coex-güvenli varsayılanı kendi seçer.

### Ayarlardaki ağ listesi neden ayrı bir API kullanıyor?

Ayarlar ekranındaki ağ listesi asenkron taramayla **SSID** döndürür — kullanıcı okusun diye. Geolocation ise senkron API ile **BSSID** ister. İki farklı amaç, iki farklı çağrı.

## Rapor anatomisi

Cihaz ham veriyi gönderir; sunucu konum çözümünü aynı kayda gömer. Aşağıdaki iki sekme, aynı raporun iki aşamasıdır.

```
{
  "device_id":        "E4B063A764CC",
  "wifi_rssi":        -55,
  "wifi":             [ {"mac": "A4:B1:C2:D3:E4:F5", "rssi": -53}, ... ],
  "uptime_s":         887,
  "firmware_version": "1.4.81",
  "heap_free":        970288
}
```

```
"geo": {
  "lat": 41.0289898, "lng": 29.2229154,
  "accuracy_m": 18.9,
  "fallback": null,              // null = GERÇEK WiFi çözümü; "ipf" = IP tahmini
  "provider": "www.googleapis.com"
}
```

## Veri dürüstlüğü kuralı

Sağlayıcı erişim noktalarını tanımadığında **IP tabanlı kaba tahmine** düşebilir. Sapması ±25 km'ye çıkar — testlerde İstanbul'daki bir cihazı farklı bir şehirde gösterdi. Bu yüzden fallback'li sonuç konum sayılmaz.

<div style="background:rgba(128,128,128,.10);border:1px solid rgba(128,128,128,.28);border-radius:12px;padding:16px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:10px;color:inherit">Sonuç değiştiğinde tüketicilere ne gider</div><div style="display:grid;grid-template-columns:2fr 1fr 1fr;gap:8px;font-size:11.5px;font-weight:800;letter-spacing:.6px;text-transform:uppercase;color:inherit;opacity:.72"><div>Tüketici</div><div style="text-align:center">fallback: null</div><div style="text-align:center">fallback: "ipf"</div></div><div style="display:grid;grid-template-columns:2fr 1fr 1fr;gap:8px;align-items:center;padding:10px 0;border-top:1px solid rgba(128,128,128,.20)"><div><div style="font-weight:700;color:inherit">Koordinat alanları</div><div style="font-size:12.5px;color:inherit;opacity:.72">latitude / longitude</div></div><div style="text-align:center;color:#12A0AD;font-weight:800">✓</div><div style="text-align:center;color:inherit;opacity:.72;font-weight:800">✕</div></div><div style="display:grid;grid-template-columns:2fr 1fr 1fr;gap:8px;align-items:center;padding:10px 0;border-top:1px solid rgba(128,128,128,.20)"><div><div style="font-weight:700;color:inherit">location-data-queue</div><div style="font-size:12.5px;color:inherit;opacity:.72">konum tüketen sistemler</div></div><div style="text-align:center;color:#12A0AD;font-weight:800">✓</div><div style="text-align:center;color:inherit;opacity:.72;font-weight:800">✕</div></div><div style="display:grid;grid-template-columns:2fr 1fr 1fr;gap:8px;align-items:center;padding:10px 0;border-top:1px solid rgba(128,128,128,.20)"><div><div style="font-weight:700;color:inherit">Admin paneli</div><div style="font-size:12.5px;color:inherit;opacity:.72">konum olarak, OSM bağlantısıyla</div></div><div style="text-align:center;color:#12A0AD;font-weight:800">✓</div><div style="text-align:center;color:inherit;opacity:.72;font-weight:800">✕</div></div></div>

<div style="border-left:4px solid #D95757;background:rgba(217,87,87,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Kural koda gömülüdür</div><div style="line-height:1.6;color:inherit">Fallback'li sonuç koordinat alanlarına <strong>yazılmaz</strong> ve konum kuyruğuna <strong>asla</strong> basılmaz. Kaba tahminin konum tüketen sistemlere gitmesi mimari olarak imkânsızdır — panelde bir seçenek değildir.</div></div>

## Maliyet koruması

Önlem alınmasaydı her rapor bir API çağrısı olurdu: cihaz başına ayda yaklaşık **43.000 istek**. Önbellek, aynı erişim noktası kümesi için sonucu yeniden kullanır.

![Konum önbelleği maliyet karşılaştırması](GORSEL: 05-cost-protection.png)

*Sıralı MAC seti anahtarı, 10 dakika TTL — sabit cihazda ayda birkaç yüz çağrı*

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Telemetri konuma bağımlı değildir</div><div style="line-height:1.6;color:inherit">Konum API'si hata verse ya da zaman aşımına uğrasa bile rapor konumsuz devam eder. Telemetri akışı hiçbir koşulda konum servisine bağlı değildir.</div></div>

## Panelde nasıl görünür

Raporlar admin panelinin Telemetri sekmesinde listelenir. Konum sütunu, veri dürüstlüğü kuralını doğrudan gösterir: gerçek çözümler tıklanabilir koordinat, IP tahminleri gri metindir.

![Admin paneli telemetri sekmesi](GORSEL: 01-admin-telemetry.png)

*Mavi bağlantılar gerçek WiFi çözümü; gri satır IP tahmini (±25 km). İletim rozetleri: sent dry-run dropped*

| Durum | Anlamı |
|---|---|
| dry_run | İletim anahtarı kapalı — mesaj hazırlanır ve kaydedilir ama gönderilmez (güvenli prova modu) |
| sent | Mesaj kuyruğa iletildi |
| dropped | İletim denendi, başarısız — kalıcı kayıt yine tutulur |

## Cihazda ekranı yoktur

Telemetri ve konumlandırma **başsız (headless) arka plan servisleridir**. Saat üzerinde bunları gösteren bir uygulama ya da ayar sayfası yoktur: `telemetry_service.h` yalnızca `telemetry_service_init()` açar, ve `src/apps/` altındaki hiçbir dosya telemetriye referans vermez.

Cihaz tarafındaki tek görünür çıktı seri port logudur:

```
I (12034) telemetry: init OK (device_id=E4B063A764CC)
I (12040) telemetry: telemetry loop started (period=60 s, url=...)
I (72118) telemetry: wifi scan refreshed: 7 AP
I (132204) telemetry: report sent (HTTP 200, rssi=-53)
```

<div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Hava Durumu'ndaki şehir bu değildir</div><div style="line-height:1.6;color:inherit">Saatte konum gösteren tek yer Hava Durumu uygulamasının şehir adıdır — ama o <code>weather_service</code>'in <strong>IP geolocation</strong> (ip-api.com) sonucudur, NVS'e önbelleklenir. Buradaki WiFi BSSID konumlandırmasıyla ilgisi yoktur ve hassasiyeti kilometrelerdir.</div></div>

<div style="margin:40px 0 0"><a href="/document-content/narrtos-sunucu-kurulumu-8" style="display:flex;align-items:center;gap:22px;flex-wrap:wrap;border:1px solid #12A0AD;background:rgba(18,160,173,.07);border-radius:14px;padding:26px 28px;text-decoration:none;color:inherit"><div><div style="font-size:12.5px;font-weight:800;letter-spacing:1.4px;text-transform:uppercase;color:#12A0AD">Sonraki adım</div><div style="font-size:22px;font-weight:800;margin-top:4px;color:inherit">Sunucu Kurulumu</div><div style="font-size:15.5px;color:inherit;opacity:.72;margin-top:4px">Backend'i ayağa kaldırın, konum sağlayıcısını seçin, kuyruğu bağlayın</div></div><div style="margin-left:auto;font-size:32px;color:#12A0AD">→</div></a></div>
