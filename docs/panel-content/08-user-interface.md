NarRTOS'un kullanıcı etkileşimi üç girdiye dayanır: **dokunmatik jestler**, **iki fiziksel buton** ve **hareket** (bilek kaldırma).

## Jest (swipe) sistemi

Dokunmatik denetleyici donanımsal jest verisi göndermediği için swipe algılama **yazılımda** yapılır: dokunma okuma geri çağrısı parmak izini takip edip yön çıkarır ve global `g_swipe_dir` değişkenine yazar (-1 sol, +1 sağ, -2 yukarı, +2 aşağı). `app_manager`'ın 50 ms'lik olay zamanlayıcısı bu değeri okuyup navigasyona çevirir.

| Bağlam | Jest | Davranış |
|---|---|---|
| Saat kadranı | Sağa swipe | Launcher'ı açar (uygulama menüsü) |
| Saat kadranı | Sola / yukarı / aşağı | Bilinçli olarak atanmamıştır (kazara ekran değişimini önler) |
| Diğer tüm ekranlar | Sağa veya sola swipe | Geri — tek elle kullanım için iki yön de geri çalışır |
| Üst kenar (y ≤ 40 px) | Aşağı çekme | Control Center'ı açar |
| Control Center açıkken | Yukarı/sağa/sola swipe veya karartılmış zemine dokunma | Paneli kapatır |

Kod karşılığı (`app_manager.cpp`, sadeleştirilmiş):

```
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

**Swipe kilidi:** Sürükleme tabanlı widget'lar (ör. parlaklık çubuğu) sürükleme boyunca swipe navigasyonunu kilitler (`ui_service_is_swipe_locked`) — slider kullanırken yanlışlıkla "geri" tetiklenmez.

## Control Center

Üstten aşağı çekilen hızlı ayarlar paneli. Aktif uygulamanın **üzerine** çizilir (`lv_layer_top()`) — uygulama ekranı bozulmaz. İçerik: **WiFi | Bluetooth | Uçak modu | El feneri** hızlı anahtarları.

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

Uyandırma kaynakları loglarda ayırt edilir (dokunma, üst/alt buton, bilek kaldırma) — sahte uyandırmaların teşhisi için. Bilek kaldırma, IMU'nun hareket-ile-uyandırma (WOM) kesmesiyle gelir.

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

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Tasarım notu</div><div style="line-height:1.6;color:inherit">Uygulamalar ekran ölçülerini sabit yazmaz; ölçü sabitleri board varyantına göre seçilir. Bkz. Uygulama Geliştirme › Çözünürlük uyumu.</div></div>

<div style="display:flex;gap:30px;justify-content:center;flex-wrap:wrap;margin:30px 0"><figure style="margin:0;max-width:260px;flex:0 1 auto;text-align:center"><img src="GORSEL:watchface-selector.gif" alt="Kadran seçici" style="width:100%;display:block;border-radius:0" /><figcaption style="font-size:12.5px;color:inherit;opacity:.72;margin-top:4px">Kadranlar arasında yatay kaydırma</figcaption></figure><figure style="margin:0;max-width:260px;flex:0 1 auto;text-align:center"><img src="GORSEL:app-launch.gif" alt="Uygulama açılışı" style="width:100%;display:block;border-radius:0" /><figcaption style="font-size:12.5px;color:inherit;opacity:.72;margin-top:4px">Ana ekrandan uygulamaya ve geri</figcaption></figure></div>

<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(148px,1fr));gap:22px 18px;margin:28px 0"><figure style="margin:0;text-align:center"><img src="GORSEL:watchface-analog.png" alt="Analog kadran" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Analog kadran</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">FACE_ANALOG</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:watchface-digital.png" alt="Dijital kadran" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Dijital kadran</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">FACE_DIGITAL</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:watchface-activity.png" alt="Activity kadranı" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Activity kadranı</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">FACE_ACTIVITY</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:watchface-io-control.png" alt="Çıkış kontrolü" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Çıkış kontrolü</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">FACE_IO_CONTROL</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:launcher.png" alt="Ana ekran" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Ana ekran</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">3×3 launcher</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:step-counter.png" alt="Adım sayar" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Adım sayar</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">günlük hedef halkası</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:weather.png" alt="Hava durumu" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Hava durumu</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">çoklu şehir, canlı veri</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:gallery.png" alt="Galeri" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Galeri</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">fotoğraf tarayıcı</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:alarm.png" alt="Alarmlar" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Alarmlar</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">kurma ekranı</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:notifications.png" alt="Bildirimler" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Bildirimler</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">ANCS listesi</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:notification-detail.png" alt="Bildirim detayı" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Bildirim detayı</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">gövde + kapat</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:store-apps.png" alt="Mağaza" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Mağaza</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">Apps sekmesi</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:store-detail.png" alt="Mağaza detayı" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Mağaza detayı</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">uygulama kartı</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:settings.png" alt="Ayarlar" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Ayarlar</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">kök menü</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:power-rails.png" alt="Güç rayları" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Güç rayları</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">9 toggle + kapat</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:ota.png" alt="Yazılım güncelleme" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Yazılım güncelleme</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">OTA akışı</span></figure></div>
