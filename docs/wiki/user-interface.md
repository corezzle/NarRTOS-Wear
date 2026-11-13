# Kullanıcı Arayüzü

NarRTOS'un kullanıcı etkileşimi üç girdiye dayanır: **dokunmatik jestler**, **iki fiziksel
buton** ve **hareket** (bilek kaldırma).

## Jest (swipe) sistemi

Dokunmatik denetleyici donanımsal jest verisi göndermediği için swipe algılama **yazılımda**
yapılır: dokunma okuma geri çağrısı parmak izini takip edip yön çıkarır ve global
`g_swipe_dir` değişkenine yazar (-1 sol, +1 sağ, -2 yukarı, +2 aşağı). `app_manager`'ın
50 ms'lik olay zamanlayıcısı bu değeri okuyup navigasyona çevirir.

| Bağlam | Jest | Davranış |
|---|---|---|
| Saat kadranı | Sağa swipe | Launcher'ı açar (uygulama menüsü) |
| Saat kadranı | Sola / yukarı / aşağı | Bilinçli olarak atanmamıştır (kazara ekran değişimini önler) |
| Diğer tüm ekranlar | Sağa veya sola swipe | Geri — tek elle kullanım için iki yön de geri çalışır |
| Üst kenar (y ≤ 40 px) | Aşağı çekme | Control Center'ı açar |
| Control Center açıkken | Yukarı/sağa/sola swipe veya karartılmış zemine dokunma | Paneli kapatır |

Kod karşılığı (`app_manager.cpp`, sadeleştirilmiş):

```c
static void check_swipe(void)              // 50 ms'lik olay zamanlayıcısında
{
    if (ui_service_is_swipe_locked()) { g_swipe_dir = 0; return; }
    if (g_swipe_dir == 0) return;
    int8_t dir = g_swipe_dir;  g_swipe_dir = 0;      // okundu, sıfırla

    if (control_center_is_open()) { control_center_close(); return; }

    switch (dir) {
        case -1:  // sağa swipe
            if (s_current == APP_ID_WATCHFACE)
                app_manager_launch(APP_ID_LAUNCHER); // kadran -> menü
            else
                app_manager_back();                  // diğerlerinde: geri
            break;
        case +1:  // sola swipe: kadranda no-op, diğerlerinde geri
            if (s_current != APP_ID_WATCHFACE) app_manager_back();
            break;
    }
}
```

**Swipe kilidi:** Sürükleme tabanlı widget'lar (ör. parlaklık çubuğu) sürükleme boyunca swipe
navigasyonunu kilitler (`ui_service_is_swipe_locked`) — slider kullanırken yanlışlıkla "geri"
tetiklenmez.

## Control Center

Üstten aşağı çekilen hızlı ayarlar paneli. Aktif uygulamanın **üzerine** çizilir
(`lv_layer_top()`) — uygulama ekranı bozulmaz. İçerik: **WiFi | Bluetooth | Uçak modu |
El feneri** hızlı anahtarları.

## Fiziksel butonlar

| Buton | Pin | Rol |
|---|---|---|
| Üst buton | GPIO 0 | Uyandırma / ana ekrana dönüş (kısa-uzun basış olay veriyoluyla uygulamaya iletilir) |
| Alt buton | GPIO 10 | Uyandırma / uygulamaya özgü işlev |

## Güç durumları ve uyandırma

`power_service` dört durumlu bir makine işletir; her kullanıcı etkileşimi sayacı sıfırlar:

```
ACTIVE --(30 sn hareketsizlik)--> DIM --(5 sn)--> DISPLAY_OFF
   ^                                                  |
   +---- dokunma / buton / bilek kaldırma (WOM) ------+

DISPLAY_OFF --(10 dk hareketsizlik)--> DEEP_SLEEP
   (derin uykudan uyandırma: buton, RTC alarmı)
```

Uyandırma kaynakları loglarda ayırt edilir (dokunma, üst/alt buton, bilek kaldırma) — sahte
uyandırmaların teşhisi için. Bilek kaldırma, IMU'nun hareket-ile-uyandırma (WOM) kesmesiyle
gelir.

## Yerleşik uygulamalar

| Uygulama | APP_ID | Ne yapar / kullandığı servisler |
|---|---|---|
| WatchFace | 0 | Varsayılan ana ekran: saat, tarih, batarya, adım. Olaylarla canlı güncellenir |
| StepCounter | 1 | Adım sayar ekranı — `sensor_service`'in IMU verisi |
| Settings | 2 | Çok sayfalı ayarlar: WiFi (tarama + el yazısı/klavye şifre girişi), ekran, OTA, sistem bilgisi |
| Launcher | 3 | 3×3 grid uygulama menüsü (kadrandan sağa swipe) |
| HeartRate | 5 | İskelet (gelecek uygulama) |
| Weather | 6 | Şehir + sıcaklık + UV/rüzgâr/nem halkaları; `weather_service` (WiFi gerekir) |
| Timer | 7 | İskelet (gelecek uygulama) |
| Notifications | 8 | Telefon bildirimleri (BLE/ANCS): liste + detay + dismiss |
| Gallery | 9 | Cihaz depolamasındaki görseller; WiFi üzerinden yükleme sayfası |
| Alarm | 10 | Alarm listesi + kurma ekranı; `alarm_service` + `audio_service` |
| Store | 11 | Uygulama kataloğu (metadata-only mimari) |

> **Tasarım notu:** Uygulamalar ekran ölçülerini sabit yazmaz; ölçü sabitleri board varyantına
> göre seçilir. Bkz. [[Uygulama Geliştirme|app-development]] › Çözünürlük uyumu.
