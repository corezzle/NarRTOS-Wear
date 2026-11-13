Adding a new application to the system takes **4 steps**. The example below adds an empty application called "MyApp" end to end.

## Step 1 — Write the application files: `src/apps/myapp/`

Every application consists of two files and exposes a single function to the outside: `*_get_interface()` (all built-in applications follow this pattern).

```
// src/apps/myapp/myapp_app.h
#pragma once
#include "../../core/app_manager.h"
const app_interface_t* myapp_get_interface(void);
```

```
// src/apps/myapp/myapp_app.cpp
#include "myapp_app.h"

static void my_on_start(lv_obj_t* screen) {
    lv_obj_t* lbl = lv_label_create(screen);
    lv_label_set_text(lbl, "Merhaba NarRTOS");
    lv_obj_center(lbl);
    app_manager_enable_gestures(screen);   // swipe'ların ekrana ulaşması için
}

static void my_on_stop(void) { /* on_start'ta ayrılan kaynakları bırak */ }

static bool my_on_event(const os_event_t* e) {
    // örn. EVENT_TIME_UPDATED geldiğinde etiketi tazele
    return false;                          // true = olayı tükettim
}

static const app_interface_t my_app = {
    .on_start  = my_on_start,
    .on_resume = NULL, .on_pause = NULL,
    .on_stop   = my_on_stop,
    .on_event  = my_on_event,
    .name      = "myapp",
};

const app_interface_t* myapp_get_interface(void) { return &my_app; }
```

## Step 2 — Define an identity: `core/os_config.h`

```
typedef enum {
    APP_ID_WATCHFACE    = 0,
    ...
    APP_ID_STORE        = 11,
    APP_ID_MYAPP        = 12,   // <-- YENİ: her zaman APP_ID_COUNT'tan önce
    APP_ID_COUNT
} app_id_t;
```

## Step 3 — Add it to the central registry: `core/app_manager.cpp`

All applications are registered in one place, inside `app_manager_init()` — `main.cpp` is left untouched:

```
#include "../apps/myapp/myapp_app.h"

void app_manager_init(void) {
    ...
    app_manager_register(APP_ID_WATCHFACE, watchface_app_get_interface());
    ...
    app_manager_register(APP_ID_MYAPP,     myapp_get_interface());   // <-- YENİ
}
```

## Step 4 — (Optional) Add a tile to the launcher

Registration makes the application **runnable**; to make it appear in the menu, add a tile (name + icon symbol + APP_ID) to the grid definition in `src/apps/launcher/launcher_app.cpp`. Even without a launcher tile, the application can be opened from elsewhere (a button event, an alarm, another application, the Command Interface):

```
app_manager_launch(APP_ID_MYAPP);
```

## Build and run

After every code change, clean first, then build and upload:

```
pio run -e esp32s3_wearable -t clean
pio run -e esp32s3_wearable -t upload    # cihazda: Launcher'da yeni kutucuk
```

Cleaning removes the leftovers of the previous build; the second command compiles from scratch and uploads to the board. A clean build takes a few minutes, and in return you never hit "my change did not show up" problems caused by stale object files.

---

## Rules to follow

1. **The UI is built only on the screen passed to `on_start`** — do not store the screen object and reach into other screens.

2. **UI updates from another task** are wrapped in `ui_service_lock/unlock` (see Architecture › LVGL concurrency).

3. **`on_stop` must release every resource `on_start` allocated** (timers, buffers, service subscriptions) — applications are opened and closed over and over.

4. **Do not poll services directly, listen for events.** A real example (WatchFace):

```
static bool on_event(const os_event_t* event)
{
    if (!event) return false;
    if (event->type == EVENT_WEATHER_UPDATE) {
        // Hava durumu arka plan görevi yeni veri yayınladı -> ikonu yenile.
        // UI task bağlamındayız; doğrudan render güvenli.
        if (s_activity_icon_cloud) {
            weather_icon_render(s_activity_icon_cloud,
                                (weather_condition_t)event->data);
        }
        return true;    // olay tüketildi
    }
    return false;       // ilgilenmiyorum -> diğer dinleyicilere geçsin
}
```

---

## Resolution portability (the size-constant pattern)

The pattern that lets applications run at both resolutions (410×502 / 240×240) **without being rewritten**: every application declares its screen-dependent sizes in **a single variant-guarded constant block**, and the UI code uses nothing but those constants.

```
// launcher_app.cpp — ölçü sabitleri (tek blok, uygulamanın başında)
#ifdef BOARD_VARIANT_LCD128          // 240×240 yuvarlak LCD
#define L_SCR_W         240
#define L_CELL_W        88           // grid hücresi
#define L_CELL_H        92
#define L_ICON          64           // uygulama ikonu
#define L_SYM_FONT      (&lv_font_montserrat_28)
#else                                // 410×502 AMOLED (varsayılan)
#define L_SCR_W         410
#define L_CELL_W        120
#define L_CELL_H        128
#define L_ICON          95
#define L_SYM_FONT      (&lv_font_montserrat_36)
#endif

// UI kodu her yerde SADECE sabiti kullanır:
lv_obj_set_size(cell, L_CELL_W, L_CELL_H);
```

For simple height/position differences an inline form is used as well:

```
#ifdef BOARD_VARIANT_LCD128
#define VIEW_SCR_H     240
#else
#define VIEW_SCR_H     502
#endif
lv_obj_set_size(sbox, LV_PCT(100), VIEW_SCR_H);
```

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Invariance rule</div><div style="line-height:1.6;color:inherit">The default (AMOLED) branch carries the original values verbatim on the <code>#else</code> side — so adding a variant does <strong>not change a single byte</strong> of the default build's binary after preprocessing. Supporting a new resolution = adding one branch to each application's constant block; the UI logic is never touched.</div></div>

### Practical notes for small screens

- Preserve the icon-to-circle ratio (e.g. a 24 px symbol in a 40 px circle → 16 px in a 26 px circle).

- Set the list row spacing (`STEP`) to the row height + ~8 px; carrying the AMOLED value over as-is produces enormous gaps.

- On a round display the usable width narrows towards the top and bottom edges — elements such as a title and a clock may need to be stacked **vertically** rather than side by side.
