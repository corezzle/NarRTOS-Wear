## Core components (`src/core/`)

### event_bus — pub/sub event bus

The only channel for inter-task communication. Every subscriber owns its FreeRTOS queue; a
published event is distributed to all subscriber queues and processed **in the subscriber's own
task context** (ISR-safe publishing).

```
// Olay grupları (event_bus.h):
EVENT_BATTERY_LEVEL / EVENT_CHARGING_STATE / EVENT_LOW_BATTERY   // güç
EVENT_STEP_COUNT / EVENT_GESTURE_WRIST_UP / EVENT_SHAKE          // sensör
EVENT_TOUCH_DOWN / EVENT_BTN_TOP / EVENT_BTN_BOTTOM              // girdi
EVENT_TIME_UPDATED / EVENT_APP_SWITCH / EVENT_WEATHER_UPDATE     // sistem

void event_bus_publish(event_type_t type, uint32_t data);
```

The thread-safe way to open one application from another task is also this:
`event_bus_publish(EVENT_APP_SWITCH, APP_ID_WEATHER)` → `app_manager` opens the application
in its own timer (the LVGL task).

### app_manager — application lifecycle

Every application implements an `app_interface_t` vtable; only one application is active at a
time (it owns the LVGL screen):

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

It also runs a 50 ms event timer: it reads gestures, drains event queues and delivers them to
the active application.

### button_handler

Debounces the two physical buttons (top/bottom, active LOW) and publishes them as
`EVENT_BTN_TOP` / `EVENT_BTN_BOTTOM`, distinguishing short and long presses.

---

## Service layer (`src/services/`)

| Service | Job |
|---|---|
| `ui_service` | LVGL startup, render loop, LVGL mutex ownership, touch input registration |
| `power_service` | Battery monitoring, display sleep/wake, activity tracking |
| `time_service` | Reading the clock from the RTC + NTP sync when WiFi is available; publishes `EVENT_TIME_UPDATED` |
| `wifi_service` | Connection management, asynchronous network scanning, user preference (persisted in NVS) |
| `ble_service` | BLE stack (NimBLE GATT server) — the basis of the notification bridge and command interface |
| `audio_service` | Buzzer/sound output (alarm and notification tones) |
| `sensor_service` | IMU sampling task (Core 1); produces step and gesture events |
| `ota_service` | Querying the version manifest, downloading images, A/B updating |
| `alarm_service` | Timer/alarm core (LVGL timer based) |
| `telemetry_service` | Periodic health + WiFi scan reporting (see Telemetry and Positioning) |
| `command_service` | USB/BLE command interface (see Command Interface) |

### ui_service API (most used)

```
void     ui_service_init(void);        // LVGL + display driver + mutex
void     ui_service_register_touch();  // dokunmatiği LVGL'e bağla
uint32_t ui_service_tick(void);        // UI görev döngüsü (mutex içinde
                                       //   lv_timer_handler)
void     ui_service_lock(void);        // görevler arası UI güncellemesi için
void     ui_service_unlock(void);
```

### Adding a new service

Services are **optional and extensible**: adding a pair of files under `services/` and calling
init in the boot flow (`main.cpp`) is enough. A service publishes its result to the event bus;
nobody calls anybody directly.

```
// services/my_service.cpp
esp_err_t my_service_init(void) {
    xTaskCreatePinnedToCore(my_task, "my_svc", 4096, NULL, 3, NULL, 1 /* Core 1 */);
    return ESP_OK;
}
```

<div style="display:flex;gap:22px;align-items:flex-start;flex-wrap:wrap;margin:18px 0"><figure style="flex:0 0 190px;margin:0;text-align:center"><img src="GORSEL:power-rails.gif" style="width:100%;display:block;border-radius:10px" /><figcaption style="font-size:12.5px;color:inherit;opacity:.72;margin-top:8px">Settings → Power Rails</figcaption></figure><div style="flex:1 1 320px;min-width:280px"><p><code>power_service</code> can switch AXP2101 PMIC rails on and off individually at runtime. This page in Settings binds <strong>nine rails</strong> to separate toggles; at the bottom, the <em>Soft Power Off</em> action that shuts the watch down completely sits behind a confirmation dialog.</p><p>The labels were verified by physical testing on the device — for example ALDO1 is not the audio rail but the <strong>sensor</strong> rail (turning it off kills touch), while ALDO2 and DCDC2 feed the display.</p><div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">DCDC1 is not listed</div><div style="line-height:1.6;color:inherit">It is the system 3.3V rail; switching it off kills the ESP32 — which is exactly what Soft Power Off already does. Two separate controls would only confuse.</div></div></div></div>
