/**
 * @file app_manager.cpp
 * @brief Modüler app yaşam döngüsü yöneticisi.
 *
 * Geçiş akışı (A → B):
 *   1. A->on_pause()
 *   2. lv_screen_load_anim ile yeni LVGL screen animasyonlu yükle
 *   3. B->on_start(new_screen)
 *   4. Eski screen anim tamamlandıktan sonra silinir
 *
 * Event Dispatch:
 *   app_manager, event_bus'a abone olur ve tüm olayları her 50ms'de bir
 *   mevcut uygulamanın on_event'ine iletir (LVGL timer = Core 0 güvenli).
 *   Uygulama on_event'ten false dönerse app_manager varsayılan navigasyon
 *   davranışını uygular:
 *     TOP kısa  → app_manager_back()
 *     TOP uzun  → WatchFace'e dön (stack temizle)
 *
 *   BOTTOM kısa → GLOBAL shortcut: uygulamanın on_event'inden ÖNCE
 *   işlenir; her zaman Launcher'a döner (control center açıksa kapatır,
 *   settings alt-görünümdeyse önce main'e geri iner, back stack'i sıfırlar).
 *
 * Back stack: Maksimum BACK_STACK_DEPTH derinlik.
 * Thread safety: Tüm çağrılar UI task'ından (Core 0) yapılmalı.
 */

#include "app_manager.h"
#include "event_bus.h"
#include "../services/ui_service.h"
#include "../services/control_center.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>

extern bool settings_app_handle_back(void);

static const char* TAG = "AppManager";

#define BACK_STACK_DEPTH    8
#define NUM_EVENT_TYPES     7
/* App-to-app slide süresi. Iterasyon (v1.2.1, 2026-05-18):
 *   80 ms (audit öncesi + final): kullanıcı fiziksel cihaz testi sonrası
 *     "hızlı geçsin" tercih etti. HIG'in 180-260 ms standardına göre kısa
 *     ama saat formfaktöründe sık yapılan watchface ↔ launcher gibi
 *     geçişlerde uzun süre "kuyruk hissi" veriyor.
 *   180 ms (audit Sprint 1, denendi): kullanıcı "yavaş" raporladı.
 *   120 ms (denendi): hâlâ yavaş.
 *   100 ms (denendi): hâlâ yavaş.
 *   80 ms: kullanıcı onayladı, final değer.
 *
 * Not: Selector mode (watchface_app MODE_SELECTOR) bu sabitten etkilenmez —
 * watchface içsel state, ayrı animation mekanizması. */
#define SCREEN_TRANSITION_MS 80
#define SCREEN_TRANSITION_DELAY_MS 0
/* Swipe block: transition süresi + 2-frame drain (~32 ms).
 * 120 + 32 = 152 ms — ardışık flick swipe'larda erken-bitme race'i
 * kapanır ama 200 ms altında, hissedilmez. */
#define SWIPE_BLOCK_US (SCREEN_TRANSITION_MS * 1000 + 32000)

// ─── State ────────────────────────────────────────────────────────────────────

typedef struct {
    const app_interface_t* iface;
    lv_obj_t*              screen;
    bool                   started;
} app_slot_t;

static app_slot_t s_apps[APP_ID_COUNT];

static app_id_t s_back_stack[BACK_STACK_DEPTH];
static int      s_back_top = -1;
static app_id_t s_current  = (app_id_t)(-1);
static int64_t  s_swipe_block_until_us = 0;

// Event dispatch
static const event_type_t s_event_types[NUM_EVENT_TYPES] = {
    EVENT_BATTERY_LEVEL,
    EVENT_CHARGING_STATE,
    EVENT_STEP_COUNT,
    EVENT_BTN_TOP,
    EVENT_BTN_BOTTOM,
    EVENT_APP_SWITCH,
    EVENT_WEATHER_UPDATE,
};
static QueueHandle_t s_event_queues[NUM_EVENT_TYPES];
static lv_timer_t*   s_dispatch_timer = NULL;

// ─── Back stack helpers ───────────────────────────────────────────────────────

static void back_stack_push(app_id_t id)
{
    if (s_back_top < BACK_STACK_DEPTH - 1) {
        s_back_stack[++s_back_top] = id;
    } else {
        memmove(s_back_stack, s_back_stack + 1,
                (BACK_STACK_DEPTH - 1) * sizeof(app_id_t));
        s_back_stack[BACK_STACK_DEPTH - 1] = id;
        ESP_LOGW(TAG, "Back stack overflow, oldest entry dropped");
    }
}

static app_id_t back_stack_pop(void)
{
    if (s_back_top < 0) return APP_ID_WATCHFACE;
    return s_back_stack[s_back_top--];
}

static bool back_stack_empty(void)
{
    return s_back_top < 0;
}

// ─── Swipe algılama (ui_service'teki touch_read_cb tarafından doldurulur) ────

static void check_swipe(void)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us < s_swipe_block_until_us) {
        g_swipe_dir = 0;
        return;
    }
    /* Backstop: bir slider/drag widget swipe lock aldıysa swipe nav'ı yutma.
     * Brightness bar drag boyunca lock_swipe(true) set ediliyor; release
     * sonrası bir tick içinde lock_swipe(false) yapılıyor ama touch_read_cb
     * release detection arada g_swipe_dir set edebiliyor. Lock TRUE iken
     * herhangi bir swipe yön değerini direkt yutmak en garantili yol. */
    if (ui_service_is_swipe_locked()) {
        g_swipe_dir = 0;
        return;
    }
    if (g_swipe_dir == 0) return;

    int8_t dir  = g_swipe_dir;
    g_swipe_dir = 0;   // okundu, sıfırla

    if (control_center_is_open()) {
        if (dir == -2 || dir == -1 || dir == +1) {
            control_center_close();
        }
        return;
    }

    /* Hardware mapping: touch x-axis görsel x-axis'le ters → dir=-1 kullanıcı
     * algısıyla "sağa swipe", dir=+1 "sola swipe".
     *
     * Davranış:
     *   - WATCHFACE: sağa (dir=-1) → Launcher; sola (dir=+1) → HQ Mesaj
     *     (v1.2.9 — Genel Müdürlük mesaj ekranı).
     *   - DİĞER EKRANLAR: her iki yön de "back" olarak çalışır
     *     (Apple Watch'ta back = right-swipe-from-edge ama bizde tek elle
     *     kullanım için iki yön de back hızlandırır). */
    switch (dir) {
        case -1:   // → kullanıcı algısı SAĞA: WatchFace → Launcher, diğer ekranlarda back
            if (s_current == APP_ID_WATCHFACE) {
                app_manager_launch(APP_ID_LAUNCHER);
            } else {
                if (s_current == APP_ID_SETTINGS && settings_app_handle_back()) return;
                app_manager_back();
            }
            break;
        case +1:   // ← kullanıcı algısı SOLA: WatchFace'te HQ Mesaj (v1.2.9), diğer ekranlarda back
            if (s_current == APP_ID_WATCHFACE) {
                app_manager_launch(APP_ID_HQMSG);   /* v1.2.9 */
            } else {
                if (s_current == APP_ID_SETTINGS && settings_app_handle_back()) return;
                app_manager_back();
            }
            break;
        case -2:   // ↑ yukarı: Watchface'te no-op (kullanıcı isteği — alternatif kadran kapatıldı).
            break;
        case +2:   // ↓ aşağı: Watchface'te no-op (kullanıcı isteği — orta-ekrandan
                   // aşağı sürükleme ile CC açılması iptal edildi). CC sadece üst
                   // kenardan (y0 ≤ 40) aşağı sürükleme ile açılır → ui_service.cpp.
            break;
        default: break;
    }
}

// ─── Event dispatch timer (Core 0, her 50ms) ─────────────────────────────────

static void event_dispatch_cb(lv_timer_t* t)
{
    (void)t;
    os_event_t ev;

    // Manuel swipe algılama (FT3168 gesture verisi göndermediğinden)
    check_swipe();

    for (int qi = 0; qi < NUM_EVENT_TYPES; qi++) {
        if (!s_event_queues[qi]) continue;

        while (event_bus_receive(s_event_queues[qi], &ev, 0)) {

            // APP_SWITCH — doğrudan app_manager işler
            if (ev.type == EVENT_APP_SWITCH) {
                if ((app_id_t)ev.data < APP_ID_COUNT) {
                    app_manager_launch((app_id_t)ev.data);
                }
                return;  // geçiş başladı, geri kalanı sonraki tick'e bırak
            }

            // BOTTOM (kısa VEYA uzun basış) → GLOBAL shortcut: her zaman Launcher.
            // Uygulamanın on_event'inden ÖNCE işlenir ki hiçbir app
            // bu navigasyonu tüketemesin ("hangi alanda olursam olayım").
            // NOT: Kullanıcının "normal tık"ı bile 600-800ms sürebiliyor (saat
            // butonları insan kullanım ergonomisine göre), bu da long-press
            // eşiğini aşıyor. Bu yüzden data=0/1 ayırmıyoruz.
            if (ev.type == EVENT_BTN_BOTTOM) {
                // Control center açıksa önce onu kapat
                if (control_center_is_open()) {
                    control_center_close();
                }
                // Settings alt-görünümlerinden çık (main view'a dön)
                if (s_current == APP_ID_SETTINGS) {
                    settings_app_handle_back();
                }
                // Back stack'i temizle: launcher artık kökte olacak
                s_back_top = -1;
                if (s_current != APP_ID_LAUNCHER) {
                    app_manager_launch(APP_ID_LAUNCHER);
                }
                return;
            }

            // Mevcut uygulamaya ilet
            bool consumed = false;
            if ((int)s_current >= 0 && s_current < APP_ID_COUNT) {
                app_slot_t* cur = &s_apps[s_current];
                if (cur->iface && cur->iface->on_event) {
                    consumed = cur->iface->on_event(&ev);
                }
            }

            // Uygulama tüketmediyse varsayılan buton navigasyonu
            if (!consumed) {
                if (ev.type == EVENT_BTN_TOP) {
                    if (ev.data == 0) {
                        // Kısa basış → geri
                        if (s_current == APP_ID_SETTINGS && settings_app_handle_back()) return;
                        app_manager_back();
                        return;
                    } else {
                        // Uzun basış → WatchFace'e dön, stack'i temizle
                        s_back_top = -1;
                        if (s_current != APP_ID_WATCHFACE) {
                            app_manager_launch(APP_ID_WATCHFACE);
                            return;
                        }
                    }
                }
            }
        }
    }
}

// ─── app_manager_init ────────────────────────────────────────────────────────

void app_manager_init(void)
{
    memset(s_apps, 0, sizeof(s_apps));
    s_back_top = -1;
    s_current  = (app_id_t)(-1);

    // Uygulama modüllerini kaydet
    extern const app_interface_t* watchface_app_get_interface(void);
    extern const app_interface_t* step_counter_app_get_interface(void);
    extern const app_interface_t* settings_app_get_interface(void);
    extern const app_interface_t* launcher_app_get_interface(void);
    extern const app_interface_t* heartrate_app_get_interface(void);
    extern const app_interface_t* weather_app_get_interface(void);
    extern const app_interface_t* timer_app_get_interface(void);
    extern const app_interface_t* notifications_app_get_interface(void);   /* v1.2.8 — gerçek ANCS UI */
    extern const app_interface_t* gallery_app_get_interface(void);
    extern const app_interface_t* alarm_app_get_interface(void);
    extern const app_interface_t* store_app_get_interface(void);   /* v1.2.4 */
    extern const app_interface_t* hq_message_app_get_interface(void);   /* v1.2.9 */

    app_manager_register(APP_ID_WATCHFACE,    watchface_app_get_interface());
    app_manager_register(APP_ID_STEP_COUNTER, step_counter_app_get_interface());
    app_manager_register(APP_ID_SETTINGS,     settings_app_get_interface());
    app_manager_register(APP_ID_LAUNCHER,     launcher_app_get_interface());
    app_manager_register(APP_ID_HEARTRATE,    heartrate_app_get_interface());
    app_manager_register(APP_ID_WEATHER,      weather_app_get_interface());
    app_manager_register(APP_ID_TIMER,        timer_app_get_interface());
    app_manager_register(APP_ID_NOTIFY,       notifications_app_get_interface());
    app_manager_register(APP_ID_GALLERY,      gallery_app_get_interface());
    app_manager_register(APP_ID_ALARM,        alarm_app_get_interface());
    app_manager_register(APP_ID_STORE,        store_app_get_interface());   /* v1.2.4 */
    app_manager_register(APP_ID_HQMSG,        hq_message_app_get_interface());   /* v1.2.9 */

    // Event bus abonelikleri (tüm event türleri tek yerden takip edilir)
    for (int i = 0; i < NUM_EVENT_TYPES; i++) {
        s_event_queues[i] = event_bus_subscribe(s_event_types[i]);
    }

    // LVGL timer → event dispatch (Core 0'da çalışır, LVGL thread-safe)
    s_dispatch_timer = lv_timer_create(event_dispatch_cb, 20, NULL);

    control_center_init();

    ESP_LOGI(TAG, "App manager initialized (%d apps registered)", APP_ID_COUNT);
}

// ─── app_manager_register ────────────────────────────────────────────────────

void app_manager_register(app_id_t id, const app_interface_t* app)
{
    if (id >= APP_ID_COUNT || !app) {
        ESP_LOGE(TAG, "register: invalid id=%d or null interface", id);
        return;
    }
    s_apps[id].iface   = app;
    s_apps[id].screen  = NULL;
    s_apps[id].started = false;
    ESP_LOGD(TAG, "Registered app[%d]: %s", id, app->name);
}

// ─── app_manager_launch ──────────────────────────────────────────────────────
//
// v1.2.4 (2026-05-20): launch_no_anim varyantı eklendi. Bazı kaynaklardan
// (örn Store detail "Open" butonu) slide animation görsel artefakt yaratıyor
// (display buffer chunk timing ile MOVE_LEFT transition uyuşmazlığı → ekranda
// "ikiye bölünmüş" frame). Bu durumlarda `lv_screen_load` (animasyon yok)
// direct switch kullanılır. Diğer launch çağrıları mevcut slide davranışını
// korur — Internal `do_launch(id, animate)` fonksiyonu iki public API'yi
// besler. Mevcut `app_manager_launch` davranışı değişmedi.

static void do_launch(app_id_t id, bool animate)
{
    if (id >= APP_ID_COUNT) {
        ESP_LOGE(TAG, "launch: invalid id=%d", id);
        return;
    }

    app_slot_t* target = &s_apps[id];
    if (!target->iface) {
        ESP_LOGE(TAG, "launch: app[%d] not registered", id);
        return;
    }

    /* Session 13 (D): zaten önde olan app'i yeniden launch etmek (komut
     * servisi / EVENT_APP_SWITCH tekrarı / çift tık) gereksiz bir screen-load
     * animasyonu çalıştırıyor ve back-stack'e "kendi kendine" giriş itiyordu
     * → sonraki geri hareketi aynı ekranda kalıyormuş gibi görünüyor.
     * C6'da aynı kalıp ekranın iki kez kurulmasına ve LVGL havuzunun
     * tükenmesine yol açmıştı. Aynı app'e launch = no-op. */
    if (id == s_current && target->screen) {
        ESP_LOGD(TAG, "launch: %s already active — ignored", target->iface->name);
        g_swipe_dir = 0;
        return;
    }

    // Mevcut uygulamayı duraklat (screen'i koru)
    if ((int)s_current >= 0 && s_current < APP_ID_COUNT) {
        app_slot_t* cur = &s_apps[s_current];
        if (cur->iface && cur->iface->on_pause) {
            cur->iface->on_pause();
        }
        back_stack_push(s_current);
        ESP_LOGI(TAG, "Paused: %s", cur->iface->name);
    }

    lv_obj_t* new_screen = target->screen;
    if (!new_screen) {
        // Yeni LVGL screen oluştur
        new_screen = lv_obj_create(NULL);
        lv_obj_set_size(new_screen, LV_PCT(100), LV_PCT(100));
        lv_obj_clear_flag(new_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(new_screen, LV_SCROLLBAR_MODE_OFF);
        // Scrollbar'ı tüm yollarla gizle (yeşil şerit sorunu)
        lv_obj_set_style_width(new_screen,  0,           LV_PART_SCROLLBAR);
        lv_obj_set_style_height(new_screen, 0,           LV_PART_SCROLLBAR);
        lv_obj_set_style_opa(new_screen,    LV_OPA_TRANSP, LV_PART_SCROLLBAR);
        // Arka plan: solid siyah (her app kendi rengini override eder)
        lv_obj_set_style_bg_color(new_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(new_screen, LV_OPA_COVER, 0);

        target->screen  = new_screen;
        target->started = true;
        int64_t t_start = esp_timer_get_time();   /* Session 13: on_start ölçümü */
        if (target->iface->on_start) {
            target->iface->on_start(new_screen);
        }
        ESP_LOGI(TAG, "Starting: %s (on_start %lld ms)", target->iface->name,
                 (long long)((esp_timer_get_time() - t_start) / 1000));
    } else {
        if (target->iface && target->iface->on_resume) {
            target->iface->on_resume();
        }
        target->started = true;
        ESP_LOGI(TAG, "Resuming: %s", target->iface->name);
    }

    if (animate) {
        // Sola kaydırma animasyonu (önceki screen'i silme)
        lv_screen_load_anim(new_screen,
                            LV_SCR_LOAD_ANIM_MOVE_LEFT,
                            SCREEN_TRANSITION_MS,
                            SCREEN_TRANSITION_DELAY_MS,
                            false);
    } else {
        // Animasyon yok — direct switch (split artifact'ten kaçınmak için)
        lv_screen_load(new_screen);
    }

    g_swipe_dir = 0;
    s_swipe_block_until_us = esp_timer_get_time() + SWIPE_BLOCK_US;
    s_current = id;
    ESP_LOGI(TAG, "Active: %s%s", target->iface->name,
             animate ? "" : " (no-anim)");
}

void app_manager_launch(app_id_t id)
{
    do_launch(id, true);
}

/* v1.2.4: animasyonsuz launch — bkz. do_launch yorumu. */
void app_manager_launch_no_anim(app_id_t id)
{
    do_launch(id, false);
}

// ─── app_manager_back ────────────────────────────────────────────────────────

void app_manager_back(void)
{
    if (back_stack_empty()) {
        ESP_LOGD(TAG, "back: already at root");
        return;
    }

    /* Session 13 (D): yığının tepesinde mevcut app'in KENDİSİ varsa (ardışık
     * tekrar) atla — aynı app'e "geri dönmek" ekranı yeniden yüklemek demek,
     * kullanıcıya hiçbir şey olmamış gibi görünür. */
    while (!back_stack_empty() && s_back_stack[s_back_top] == s_current) {
        (void)back_stack_pop();
        ESP_LOGW(TAG, "back: dropped duplicate self-entry");
    }
    if (back_stack_empty()) {
        ESP_LOGD(TAG, "back: stack only had self — staying");
        g_swipe_dir = 0;
        return;
    }

    // Mevcut uygulamayı duraklat (screen'i koru)
    if ((int)s_current >= 0 && s_current < APP_ID_COUNT) {
        app_slot_t* cur = &s_apps[s_current];
        if (cur->iface && cur->iface->on_pause) {
            cur->iface->on_pause();
        }
    }

    app_id_t prev_id = back_stack_pop();
    app_slot_t* prev = &s_apps[prev_id];

    lv_obj_t* new_screen = prev->screen;
    if (!new_screen) {
        // Önceki uygulama için yeni screen oluştur
        new_screen = lv_obj_create(NULL);
        lv_obj_set_size(new_screen, LV_PCT(100), LV_PCT(100));
        lv_obj_clear_flag(new_screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(new_screen, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_width(new_screen,  0,           LV_PART_SCROLLBAR);
        lv_obj_set_style_height(new_screen, 0,           LV_PART_SCROLLBAR);
        lv_obj_set_style_opa(new_screen,    LV_OPA_TRANSP, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(new_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(new_screen, LV_OPA_COVER, 0);
        prev->screen  = new_screen;
        prev->started = true;

        int64_t t_start = esp_timer_get_time();   /* Session 13: on_start ölçümü */
        if (prev->iface && prev->iface->on_start) {
            prev->iface->on_start(new_screen);
        }
        ESP_LOGI(TAG, "Back rebuild: %s (on_start %lld ms)",
                 prev->iface ? prev->iface->name : "?",
                 (long long)((esp_timer_get_time() - t_start) / 1000));
    } else {
        if (prev->iface && prev->iface->on_resume) {
            prev->iface->on_resume();
        }
    }

    // Sağa kaydırma animasyonu (geri hareketi) - screen'i koru
    lv_screen_load_anim(new_screen,
                        LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                        SCREEN_TRANSITION_MS,
                        SCREEN_TRANSITION_DELAY_MS,
                        false);

    g_swipe_dir = 0;
    s_swipe_block_until_us = esp_timer_get_time() + SWIPE_BLOCK_US;
    ESP_LOGI(TAG, "Back to: %s", prev->iface ? prev->iface->name : "?");
    s_current = prev_id;
}

// ─── app_manager_current_id ──────────────────────────────────────────────────

app_id_t app_manager_current_id(void)
{
    return s_current;
}

// ─── app_manager_enable_gestures ─────────────────────────────────────────────

static void bubble_recursive(lv_obj_t* obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        bubble_recursive(lv_obj_get_child(obj, i));
    }
}

void app_manager_enable_gestures(lv_obj_t* screen)
{
    // Screen'in tüm child'larına GESTURE_BUBBLE ekle
    // (screen'in kendisine eklemeye gerek yok, zaten event hedefi o)
    uint32_t cnt = lv_obj_get_child_count(screen);
    for (uint32_t i = 0; i < cnt; i++) {
        bubble_recursive(lv_obj_get_child(screen, i));
    }
}
