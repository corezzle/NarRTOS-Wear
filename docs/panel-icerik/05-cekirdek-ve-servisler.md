## Çekirdek bileşenler (`src/core/`)

### event_bus — pub/sub olay veriyolu

Görevler arası iletişimin tek yolu. Her abone kendi FreeRTOS kuyruğuna sahiptir; yayınlanan olay tüm abone kuyruklarına dağıtılır ve **abonenin kendi görev bağlamında** işlenir (ISR-güvenli yayın).

```
// Olay grupları (event_bus.h):
EVENT_BATTERY_LEVEL / EVENT_CHARGING_STATE / EVENT_LOW_BATTERY   // güç
EVENT_STEP_COUNT / EVENT_GESTURE_WRIST_UP / EVENT_SHAKE          // sensör
EVENT_TOUCH_DOWN / EVENT_BTN_TOP / EVENT_BTN_BOTTOM              // girdi
EVENT_TIME_UPDATED / EVENT_APP_SWITCH / EVENT_WEATHER_UPDATE     // sistem

void event_bus_publish(event_type_t type, uint32_t data);
```

Bir uygulamayı başka bir görevden açmanın thread-safe yolu da budur: `event_bus_publish(EVENT_APP_SWITCH, APP_ID_WEATHER)` → `app_manager` kendi zamanlayıcısında (LVGL görevi) uygulamayı açar.

### app_manager — uygulama yaşam döngüsü

Her uygulama bir `app_interface_t` vtable'ı uygular; aynı anda yalnızca bir uygulama aktiftir (LVGL ekranının sahibi odur):

```
typedef struct {
    void (*on_start)(lv_obj_t* screen); // ilk açılış: UI'yi bu ekrana kur
    void (*on_resume)(void);            // arka plandan öne gelme
    void (*on_pause)(void);             // başka uygulama devraldı
    void (*on_stop)(void);              // tamamen kapat, kaynakları bırak
    bool (*on_event)(const os_event_t*);// aktif uygulamaya olay teslimi
    const char* name;                   // log için isim
} app_interface_t;

void app_manager_register(app_id_t id, const app_interface_t* app);
void app_manager_launch(app_id_t id);   // geçiş animasyonlu
void app_manager_back(void);            // önceki uygulamaya dön
app_id_t app_manager_current_id(void);
```

Ayrıca 50 ms'lik bir olay zamanlayıcısı çalıştırır: jestleri okur, olay kuyruklarını boşaltır ve aktif uygulamaya iletir.

### button_handler

İki fiziksel butonu (üst/alt, aktif LOW) debounce edip kısa/uzun basış ayrımıyla `EVENT_BTN_TOP` / `EVENT_BTN_BOTTOM` olarak yayınlar.

---

## Servis katmanı (`src/services/`)

| Servis | Görev |
|---|---|
| `ui_service` | LVGL başlatma, render döngüsü, LVGL mutex sahipliği, dokunmatik girdi kaydı |
| `power_service` | Batarya izleme, ekran uyku/uyandırma, aktivite takibi |
| `time_service` | RTC'den saat okuma + WiFi varken NTP senkronu; `EVENT_TIME_UPDATED` yayını |
| `wifi_service` | Bağlantı yönetimi, asenkron ağ taraması, kullanıcı tercihi (NVS'te kalıcı) |
| `ble_service` | BLE altyapısı (NimBLE GATT sunucu) — bildirim köprüsü ve komut arayüzünün temeli |
| `audio_service` | Buzzer/ses çıkışı (alarm ve bildirim sesleri) |
| `sensor_service` | IMU örnekleme görevi (Core 1); adım/jest olayları üretir |
| `ota_service` | Sürüm manifest'i sorgulama, imaj indirme, A/B güncelleme |
| `alarm_service` | Zamanlayıcı/alarm çekirdeği (LVGL timer tabanlı) |
| `telemetry_service` | Periyodik sağlık + WiFi tarama raporu (bkz. Telemetri ve Konumlandırma) |
| `command_service` | USB/BLE komut arayüzü (bkz. Komut Arayüzü) |

### ui_service API'si (en sık kullanılan)

```
void     ui_service_init(void);        // LVGL + display driver + mutex
void     ui_service_register_touch();  // dokunmatiği LVGL'e bağla
uint32_t ui_service_tick(void);        // UI görev döngüsü (mutex içinde
                                       //   lv_timer_handler)
void     ui_service_lock(void);        // görevler arası UI güncellemesi için
void     ui_service_unlock(void);
```

### Yeni servis eklemek

Servisler **opsiyoneldir ve genişletilebilir**: `services/` altına bir çift dosya ekleyip boot akışında (`main.cpp`) init çağırmak yeterlidir. Servis, sonucunu event bus'a yayınlar; kimse kimseyi doğrudan çağırmaz.

```
// services/my_service.cpp
esp_err_t my_service_init(void) {
    xTaskCreatePinnedToCore(my_task, "my_svc", 4096, NULL, 3, NULL, 1 /* Core 1 */);
    return ESP_OK;
}
```

<div style="display:flex;gap:22px;align-items:flex-start;flex-wrap:wrap;margin:18px 0"><figure style="flex:0 0 190px;margin:0;text-align:center"><img src="GORSEL:guc-raylari.gif" style="width:100%;display:block;border-radius:10px" /><figcaption style="font-size:12.5px;color:inherit;opacity:.72;margin-top:8px">Ayarlar → Power Rails</figcaption></figure><div style="flex:1 1 320px;min-width:280px"><p><code>power_service</code>, AXP2101 PMIC raylarını çalışma anında tek tek açıp kapatabilir. Ayarlardaki bu sayfa <strong>dokuz rayı</strong> ayrı anahtarlara bağlar; en altta saati tamamen kapatan <em>Soft Power Off</em> eylemi onay dialog'u arkasındadır.</p><p>Etiketler cihaz üstünde fiziksel testle doğrulanmıştır — örneğin ALDO1 ses değil <strong>sensör</strong> rayıdır (kapatınca dokunmatik ölür), ALDO2 ve DCDC2 ekranı besler.</p><div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">DCDC1 listelenmez</div><div style="line-height:1.6;color:inherit">Sistem 3.3V rayıdır; kapatılması ESP32'yi öldürür — bunu zaten Soft Power Off yapar, iki ayrı kontrol kafa karıştırıcı olurdu.</div></div></div></div>
