User interaction in NarRTOS rests on three inputs: **touch gestures**, **two physical buttons** and **motion** (wrist raise).

## Gesture (swipe) system

Because the touch controller does not report hardware gesture data, swipe detection is done **in software**: the touch read callback tracks the finger, derives a direction and writes it into the global `g_swipe_dir` variable (-1 left, +1 right, -2 up, +2 down). The 50 ms event timer in `app_manager` reads that value and turns it into navigation.

| Context | Gesture | Behaviour |
|---|---|---|
| Watch face | Swipe right | Opens the launcher (application menu) |
| Watch face | Left / up / down | Deliberately unassigned (prevents accidental screen changes) |
| All other screens | Swipe right or left | Back — both directions go back, for one-handed use |
| Top edge (y ≤ 40 px) | Pull down | Opens the Control Center |
| Control Center open | Swipe up/right/left, or tap the dimmed backdrop | Closes the panel |

The code behind it (`app_manager.cpp`, simplified):

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

**Swipe lock:** Drag-based widgets (e.g. the brightness slider) lock swipe navigation for the duration of the drag (`ui_service_is_swipe_locked`) — so using a slider never accidentally triggers "back".

## Control Center

The quick-settings panel pulled down from the top. It is drawn **on top of** the active application (`lv_layer_top()`) — the application screen is left intact. Contents: **WiFi | Bluetooth | Airplane mode | Flashlight** quick toggles.

## Physical buttons

| Button | Pin | Role |
|---|---|---|
| Top button | GPIO 0 | Wake / return to home screen (short and long press are delivered to the application over the event bus) |
| Bottom button | GPIO 10 | Wake / application-specific function |

## Power states and wake-up

`power_service` runs a four-state machine; every user interaction resets the counter:

```
ACTIVE --(30 sn hareketsizlik)--> DIM --(5 sn)--> DISPLAY_OFF
   ^                                                  |
   +---- dokunma / buton / bilek kaldırma (WOM) ------+

DISPLAY_OFF --(10 dk hareketsizlik)--> DEEP_SLEEP
   (derin uykudan uyandırma: buton, RTC alarmı)
```

Wake sources are distinguished in the logs (touch, top/bottom button, wrist raise) — to help diagnose spurious wake-ups. Wrist raise arrives as the IMU's wake-on-motion (WOM) interrupt.

## Built-in applications

| Application | APP_ID | What it does / services it uses |
|---|---|---|
| WatchFace | 0 | Default home screen: time, date, battery, steps. Updated live from events |
| StepCounter | 1 | Step counter screen — IMU data from `sensor_service` |
| Settings | 2 | Multi-page settings: WiFi (scan + handwriting/keyboard password entry), display, OTA, system info |
| Launcher | 3 | 3×3 grid application menu (swipe right from the watch face) |
| HeartRate | 5 | Skeleton (future application) |
| Weather | 6 | City + temperature + UV/wind/humidity rings; `weather_service` (requires WiFi) |
| Timer | 7 | Skeleton (future application) |
| Notifications | 8 | Phone notifications (BLE/ANCS): list + detail + dismiss |
| Gallery | 9 | Images in device storage; upload page over WiFi |
| Alarm | 10 | Alarm list + setup screen; `alarm_service` + `audio_service` |
| Store | 11 | Application catalogue (metadata-only architecture) |

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Design note</div><div style="line-height:1.6;color:inherit">Applications never hard-code screen sizes; the size constants are chosen by board variant. See Application Development › Resolution portability.</div></div>

<div style="display:flex;gap:30px;justify-content:center;flex-wrap:wrap;margin:30px 0"><figure style="margin:0;max-width:260px;flex:0 1 auto;text-align:center"><img src="GORSEL:kadran-secici.gif" alt="Watch face picker" style="width:100%;display:block;border-radius:0" /><figcaption style="font-size:12.5px;color:inherit;opacity:.72;margin-top:4px">Scrolling horizontally between watch faces</figcaption></figure><figure style="margin:0;max-width:260px;flex:0 1 auto;text-align:center"><img src="GORSEL:uygulama-acilis.gif" alt="Opening an application" style="width:100%;display:block;border-radius:0" /><figcaption style="font-size:12.5px;color:inherit;opacity:.72;margin-top:4px">From the home screen into an application and back</figcaption></figure></div>

<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(148px,1fr));gap:22px 18px;margin:28px 0"><figure style="margin:0;text-align:center"><img src="GORSEL:kadran-analog.png" alt="Analog watch face" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Analog watch face</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">FACE_ANALOG</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:kadran-dijital.png" alt="Digital watch face" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Digital watch face</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">FACE_DIGITAL</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:kadran-activity.png" alt="Activity watch face" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Activity watch face</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">FACE_ACTIVITY</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:kadran-io-control.png" alt="Output control" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Output control</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">FACE_IO_CONTROL</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:launcher.png" alt="Home screen" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Home screen</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">3×3 launcher</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:adim-sayar.png" alt="Step counter" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Step counter</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">daily goal ring</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:hava-durumu.png" alt="Weather" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Weather</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">multiple cities, live data</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:galeri.png" alt="Gallery" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Gallery</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">photo browser</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:alarm.png" alt="Alarms" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Alarms</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">setup screen</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:bildirimler.png" alt="Notifications" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Notifications</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">ANCS list</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:bildirim-detay.png" alt="Notification detail" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Notification detail</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">body + dismiss</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:store-apps.png" alt="Store" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Store</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">Apps tab</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:store-detay.png" alt="Store detail" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Store detail</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">application card</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:ayarlar.png" alt="Settings" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Settings</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">root menu</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:guc-raylari.png" alt="Power rails" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Power rails</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">9 toggles + close</span></figure><figure style="margin:0;text-align:center"><img src="GORSEL:ota.png" alt="Software update" style="width:100%;display:block;border-radius:0" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Software update</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">OTA flow</span></figure></div>
