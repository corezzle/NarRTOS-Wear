## Layers

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

Upper layers call lower ones; **communication upwards happens over the event bus.**
Services do not depend on each other directly — this is what makes a single service
replaceable without touching the rest.

## Task distribution

| Task | Core | Priority | Job |
|---|---|---|---|
| UI task | Core 0 | 5 | LVGL render loop (`ui_service_tick`) |
| Sensor task | Core 1 | 4 | IMU / step counter sampling, event generation |
| Power task | — | 6 | Battery monitoring, display sleep/wake flow |
| Service tasks | Core 1 | 3 | Background work such as WiFi, OTA, telemetry, command interface |

Keeping UI rendering on a different core from sensor and service work prevents a heavy
network operation from freezing the interface.

## LVGL concurrency model

LVGL is **not thread-safe**. In NarRTOS the owner of the LVGL mutex is `ui_service`; every
`lv_*` call made outside the UI task must be done under the lock:

```
ui_service_lock();                        // LVGL mutex'ini al
lv_label_set_text(lbl, "merhaba");
ui_service_unlock();                      // bırak

// UI görevi kendi döngüsünde:
uint32_t next = ui_service_tick();        // mutex içinde lv_timer_handler
```

The other safe way to update the interface from a service callback is `lv_async_call`, which
hands the work over to the LVGL task.

## Boot flow

`app_main()` in `src/main.cpp` brings the system up layer by layer. **Order matters** — each
step relies on the infrastructure established by the previous one:

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

## Fault tolerance principle

Even if peripheral init (touch, RTC, IMU, PMIC) fails, **the system continues to boot** — the
error is logged and the related feature stays disabled. The device does not become unusable
because a single peripheral is faulty.

## Project structure

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

![Layered architecture](GORSEL: mimari-katmanlar.png)

*Five layers — upper layers call lower ones, communication upwards goes over the event bus*

![Dual-core task distribution](GORSEL: cift-cekirdek.png)

*How tasks map onto cores and priorities*
