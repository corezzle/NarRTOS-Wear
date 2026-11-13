#pragma once
/**
 * @brief UI Service - LVGL initialization and render loop bridge.
 *
 * Owns the LVGL mutex. All lv_* calls from any task must go through
 * ui_service_lock() / ui_service_unlock() to be thread-safe.
 */

#include <lvgl.h>
#include <stdint.h>

/** Init LVGL, register display driver (co5300_lvgl_flush_cb). */
void ui_service_init(void);

/** Register FT3168 as LVGL input device. */
void ui_service_register_touch(void);

/**
 * Call in the UI task loop.
 * Acquires mutex, calls lv_timer_handler(), releases mutex.
 * @return ms until next lv_timer_handler() call is needed.
 */
uint32_t ui_service_tick(void);

/** Acquire LVGL mutex (for cross-task UI updates, e.g., from sensor task). */
void ui_service_lock(void);

/** Release LVGL mutex. */
void ui_service_unlock(void);

/**
 * Swipe yönü — touch_read_cb tarafından set edilir, okunduktan sonra sıfırlanmalı.
 *  0 = yok,  -1 = sol,  +1 = sağ,  -2 = yukarı,  +2 = aşağı
 */
extern volatile int8_t g_swipe_dir;

/**
 * Swipe tespitini geçici olarak kilitle/aç.
 * Slider gibi yatay sürükleme kullanan widgetlar basılıyken kilitleyin,
 * bırakılınca açın — sürüklemenin yanlışlıkla "geri" swipe sayılmasını engeller.
 */
void ui_service_lock_swipe(bool lock);

/** Mevcut swipe lock durumunu döner. app_manager swipe-driven nav'i bypass
 *  etmek için kullanır (slider drag sırasında release sonrası fake nav'ı önler). */
bool ui_service_is_swipe_locked(void);

/**
 * Apple Watch tarzı uyandırma efekti.
 * Ekran karanlıktan açıldığında çağırılır: ekranın kenarında kısa süreliğine
 * mavi bir çerçeve belirir ve yumuşakça kaybolur.
 *
 * Thread-safe: kendi içinde ui_service_lock() alır.
 */
void ui_service_show_wake_glow(void);

/* Session 33: bu dokunuşta parmağın başlangıçtan en fazla kaç piksel
 * uzaklaştığı (HAM dokunmatik verisi). Kaydırılabilir listeler tıklamayı
 * yok saymak için kullanır — LVGL'nin kendi ölçümü, jitter yumuşatması
 * yüzünden gerçek hareketi olduğundan az görüyor. */
int32_t ui_service_touch_travel_px(void);

/* Kaydırılabilir kaplar LV_EVENT_SCROLL'da ui_service_note_scroll() çağırır;
 * tıklama işleyicileri ui_service_scroll_recent(ms) ile "az önce kaydırıldı mı"
 * diye bakar. Dokunuşun sürücü tarafında parçalanmasına karşı dayanıklıdır. */
void ui_service_note_scroll(void);
bool ui_service_scroll_recent(uint32_t within_ms);

/** GEÇİCİ TEŞHİS (Session 13 akıcılık ölçümü): dokunmatik durum bayraklarını
 *  tek satır string'e yazar — [beat] loguna eklenir. */
void ui_service_debug_state(char* out, unsigned n);

/* ── Otonom test yardımcıları (komut servisi `tap`, `drag`, `ui dump`) ──────
 * inject_tap: touch_read_cb'ye sentetik PRESSED(x,y) hold_ms boyunca, sonra
 * RELEASED verir → LVGL gerçek tık gibi işler (CLICKED). Herhangi bir
 * task'tan çağrılabilir (volatile). */
void ui_service_inject_tap(int32_t x, int32_t y, uint32_t hold_ms);

/* inject_drag: (x0,y0)→(x1,y1) doğrusal sürükleme, dur_ms boyunca PRESSED
 * (ara noktalar enterpole edilir), sonra RELEASED. Scroll testi için. */
void ui_service_inject_drag(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            uint32_t dur_ms);

/* dump_tree: aktif ekranın görünür LVGL nesnelerini (sınıf, koordinat,
 * etiket metni + piksel genişliği) satır satır `out`'a yazar. Kendi içinde
 * ui_service_lock() alır — LVGL task'ından ÇAĞIRMA. */
typedef void (*ui_dump_line_fn_t)(const char* line, void* ctx);
void ui_service_dump_tree(ui_dump_line_fn_t out, void* ctx);
