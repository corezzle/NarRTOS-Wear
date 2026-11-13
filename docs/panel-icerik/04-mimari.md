## Katmanlar

```
+--------------------------------------------------------------+
|  UYGULAMALAR (apps/)     watchface, launcher, settings, ...   |
+--------------------------------------------------------------+
|  ÇEKİRDEK (core/)        app_manager, event_bus,              |
|                          button_handler, os_config            |
+--------------------------------------------------------------+
|  SERVİSLER (services/)   ui, power, time, wifi, ble, audio,   |
|                          sensor, ota, ...                     |
+--------------------------------------------------------------+
|  SÜRÜCÜLER (drivers/)    display, touch, power(PMIC), rtc,    |
|                          imu, audio                           |
+--------------------------------------------------------------+
|  FreeRTOS + ESP-IDF + LVGL 9                                  |
+--------------------------------------------------------------+
```

Yukarı katman aşağıyı çağırır; **aşağıdan yukarıya iletişim olay veriyolu ile olur.** Servisler birbirine doğrudan bağımlı değildir — bu, tek bir servisin çıkarılıp değiştirilebilmesini sağlar.

## Görev (Task) dağılımı

| Görev | Çekirdek | Öncelik | İş |
|---|---|---|---|
| UI görevi | Core 0 | 5 | LVGL render döngüsü (`ui_service_tick`) |
| Sensör görevi | Core 1 | 4 | IMU/adım sayar örnekleme, olay üretimi |
| Güç görevi | — | 6 | Batarya izleme, ekran uyku/uyandırma akışı |
| Servis görevleri | Core 1 | 3 | WiFi, OTA, telemetri, komut arayüzü gibi arka plan işleri |

Arayüz çizimi ile sensör/servis işlerinin farklı çekirdeklerde olması, ağır bir ağ işleminin arayüzü dondurmasını engeller.

## LVGL eşzamanlılık modeli

LVGL thread-safe **değildir**. NarRTOS'ta LVGL mutex'inin sahibi `ui_service`'tir; UI görevinin dışından yapılacak her `lv_*` çağrısı kilit altında yapılmak zorundadır:

```
ui_service_lock();                        // LVGL mutex'ini al
lv_label_set_text(lbl, "merhaba");
ui_service_unlock();                      // bırak

// UI görevi kendi döngüsünde:
uint32_t next = ui_service_tick();        // mutex içinde lv_timer_handler
```

Bir servis callback'inden arayüzü güncellemenin diğer güvenli yolu `lv_async_call` ile işi LVGL görevine devretmektir.

## Boot (açılış) akışı

`src/main.cpp` içindeki `app_main()` sistemi katman katman ayağa kaldırır. **Sıra önemlidir** — her adım bir öncekinin kurduğu altyapıya dayanır:

```
1.  nvs_flash_init()            // kalıcı ayar deposu (NVS)
2.  i2c_bus_init()              // paylaşımlı I2C veriyolu
3.  axp2101_init()              // PMIC: güç raylarını aç (varsa)
4.  event_bus_init()            // pub/sub altyapısı
5.  button_handler_init()       // fiziksel butonlar
6.  pcf85063_init()             // RTC oku -> sistem saati
    time_service_init()         // NTP senkron daemon'u
7.  wifi_service_init()         // WiFi (kullanıcı tercihi NVS'ten okunur)
8.  co5300_init()               // ekran denetleyicisi (parlaklık 0'da başlar)
9.  ui_service_init()           // LVGL + display driver + mutex
10. ft3168_init()               // dokunmatik -> LVGL input device
11. app_manager_init()          // uygulama çatısı + uygulama kayıtları
12. alarm_service_init()        // LVGL timer tabanlı zamanlayıcı
13. audio_service_init()        // buzzer/ses
14. app_manager_launch(...)     // ilk ekran devralınır
    -> parlaklık ramp ile açılır (görsel "açılış" anı)
15. icm42670_init()             // IMU
    sensor_service_init()       // sensör görevi (Core 1)
16. power_service_init()        // güç/uyku yönetimi
17. ota_service_init()          // güncelleme denetleyicisi
```

### Hata toleransı ilkesi

Çevre birim init'leri (dokunmatik, RTC, IMU, PMIC) başarısız olsa bile **sistem boot'a devam eder** — hata loglanır, ilgili özellik devre dışı kalır. Cihaz, tek bir çevre biriminin arızasıyla açılamaz duruma düşmez.

## Proje yapısı

```
NarRTOS-ESP32S3/
├── platformio.ini              # build ortamları (env'ler)
├── partitions_custom.csv       # A/B OTA partisyon düzeni
├── sdkconfig.*                 # ESP-IDF konfigürasyonları (env başına)
├── src/
│   ├── main.cpp                # boot akışı (app_main)
│   ├── core/                   # ÇEKİRDEK
│   │   ├── os_config.h         #   sürüm, pin haritası, görev ayarları
│   │   ├── app_manager.{h,cpp} #   uygulama yaşam döngüsü
│   │   ├── event_bus.{h,cpp}   #   pub/sub olay veriyolu
│   │   └── button_handler.*    #   fiziksel buton yönetimi
│   ├── services/               # SERVİSLER
│   ├── drivers/                # SÜRÜCÜLER (display, touch, power, rtc, imu)
│   ├── hal/                    # donanım soyutlama (I2C bus)
│   ├── apps/                   # uygulama katmanı
│   └── assets/  fonts/  utils/
└── docs/                       # dokümantasyon
```

![Katmanlı mimari](GORSEL: mimari-katmanlar.png)

*Beş katman — yukarı katman aşağıyı çağırır, aşağıdan yukarı iletişim olay veriyolu ile olur*

![Çift çekirdekli görev dağılımı](GORSEL: cift-cekirdek.png)

*Görevlerin çekirdeklere ve önceliklere dağılımı*
