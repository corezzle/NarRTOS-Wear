# Uygulama Geliştirme

Sisteme yeni bir uygulama eklemek **4 adımdır**. Aşağıdaki örnek "MyApp" adında boş
bir uygulamayı uçtan uca ekler.

## Adım 1 — Uygulama dosyalarını yaz: `src/apps/myapp/`

Her uygulama iki dosyadan oluşur ve dışarıya tek bir fonksiyon verir:
`*_get_interface()` (yerleşik uygulamaların tamamı bu kalıptadır).

```c
// src/apps/myapp/myapp_app.h
#pragma once
#include "../../core/app_manager.h"
const app_interface_t* myapp_get_interface(void);
```

```c
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

## Adım 2 — Kimlik tanımla: `core/os_config.h`

```c
typedef enum {
    APP_ID_WATCHFACE    = 0,
    ...
    APP_ID_STORE        = 11,
    APP_ID_MYAPP        = 12,   // <-- YENİ: her zaman APP_ID_COUNT'tan önce
    APP_ID_COUNT
} app_id_t;
```

## Adım 3 — Merkezî kayda ekle: `core/app_manager.cpp`

Tüm uygulamalar tek yerde, `app_manager_init()` içinde kaydedilir — `main.cpp`'ye
dokunulmaz:

```c
#include "../apps/myapp/myapp_app.h"

void app_manager_init(void) {
    ...
    app_manager_register(APP_ID_WATCHFACE, watchface_app_get_interface());
    ...
    app_manager_register(APP_ID_MYAPP,     myapp_get_interface());   // <-- YENİ
}
```

## Adım 4 — (İsteğe bağlı) Launcher'a kutucuk ekle

Kayıt, uygulamayı **çalıştırılabilir** yapar; menüde görünmesi için
`src/apps/launcher/launcher_app.cpp`'deki grid tanımına bir kutucuk (isim + ikon
sembolü + APP_ID) eklenir. Launcher'a eklemeden de uygulama başka bir yerden
(buton olayı, alarm, başka uygulama, [[Komut Arayüzü]]) açılabilir:

```c
app_manager_launch(APP_ID_MYAPP);
```

## Derle ve çalıştır

Kodda her değişiklikten sonra önce temizleyin, sonra derleyip yükleyin:

```bash
pio run -e esp32s3_wearable -t clean
pio run -e esp32s3_wearable -t upload    # cihazda: Launcher'da yeni kutucuk
```

Temizlik önceki derlemenin kalıntılarını siler; ikinci komut sıfırdan derleyip karta
yükler. Temiz derleme birkaç dakika sürer, karşılığında eski nesne dosyalarından
kaynaklanan "değişiklik yansımadı" sorunları yaşanmaz.

---

## Uyulması gereken kurallar

1. **UI yalnızca `on_start`'a verilen ekrana kurulur** — ekran nesnesini saklayıp
   başka ekranlara dokunulmaz.
2. **Başka görevden UI güncellemesi** `ui_service_lock/unlock` ile sarılır
   (bkz. [[Mimari]] › LVGL eşzamanlılık).
3. **`on_stop`, `on_start`'ta ayrılan her kaynağı bırakmalıdır** (timer, buffer,
   servis aboneliği) — uygulamalar tekrar tekrar açılıp kapanır.
4. **Servisleri doğrudan yoklamayın, olayları dinleyin.** Gerçek örnek (WatchFace):

```c
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

## Çözünürlük uyumu (ölçü sabitleri kalıbı)

Uygulamaların iki çözünürlükte de (410×502 / 240×240) **yeniden yazılmadan** çalışmasını
sağlayan kalıp: her uygulama ekrana bağlı ölçülerini **tek bir varyant-korumalı sabit
bloğunda** tanımlar; UI kodu yalnızca bu sabitleri kullanır.

```c
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

Basit yükseklik/konum farkları için satır-içi hali de kullanılır:

```c
#ifdef BOARD_VARIANT_LCD128
#define VIEW_SCR_H     240
#else
#define VIEW_SCR_H     502
#endif
lv_obj_set_size(sbox, LV_PCT(100), VIEW_SCR_H);
```

> **Değişmezlik kuralı:** Varsayılan (AMOLED) dal, `#else` tarafında orijinal
> değerlerin birebir aynısını taşır — böylece varyant eklemek, preprocessor sonrası
> varsayılan build'ın binary'sini **tek byte değiştirmez.** Yeni bir çözünürlük
> desteklemek = her uygulamanın sabit bloğuna bir dal eklemek; UI mantığına dokunulmaz.

### Küçük ekran için pratik notlar

- İkon/daire oranını koru (ör. 40 px dairede 24 px sembol → 26 px dairede 16 px).
- Liste satır aralığını (`STEP`) satır yüksekliği + ~8 px olarak kur; AMOLED değerini
  olduğu gibi taşımak devasa boşluklar üretir.
- Yuvarlak ekranda üst/alt kenarlarda görünür genişlik daralır — başlık ve saat gibi
  öğeleri yan yana değil, **alt alta** yerleştirmek gerekebilir.
