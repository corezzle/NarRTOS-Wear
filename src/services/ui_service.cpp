/**
 * @file ui_service.cpp
 * @brief LVGL başlatma ve render döngüsü köprüsü.
 *
 * Thread safety:
 *  LVGL 9.x varsayılan olarak thread-safe değildir.
 *  Tüm lv_* çağrıları ya UI task'ından (Core 0) yapılmalı ya da
 *  ui_service_lock() / ui_service_unlock() ile korunmalıdır.
 *
 *  ui_service_tick() her çağrısında mutex alır/bırakır — UI task'ı
 *  bu fonksiyonu çağırdığı sürece diğer tasklar unlock penceresinde
 *  güvenle lv_* çağırabilir.
 */

#include "ui_service.h"
#include "control_center.h"
#include "power_service.h"
#include "../drivers/display/co5300_driver.h"
#include "../drivers/touch/ft3168_driver.h"
#include "../core/os_tick.h"
#include "../core/os_config.h"
#include "../../include/wear_motion.h"   /* v1.2.1: wake glow path için */

#include <lvgl.h>
#include <string.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>

static const char* TAG = "UIService";

static SemaphoreHandle_t s_lvgl_mutex = NULL;
static lv_display_t*     s_disp      = NULL;
static lv_indev_t*       s_touch_dev = NULL;

// ─── Swipe tespiti (LVGL gesture sistemi FT3168 ile çalışmadığından) ─────────
// FT3168, her örnek arasında INT pinini kısa süre HIGH yapar.
// Bu yüzden parmak kalkmadan önce debounce süresi bekliyoruz.
//
// Erken tespit (early detection) mantığı:
//   Yatay swipe intent parmak basılıyken tespit edilir. Threshold aşılınca
//   LVGL'ye anında RELEASED gönderilir → item üzerindeki bekleyen CLICKED
//   iptal olur. Yön bilgisi saklanır, parmak kalkınca g_swipe_dir'e uygulanır.
//   Bu sayede menü item'ları üzerinden yapılan swipe gesture'ları tap olarak
//   yanlış algılanmaz.
#define SWIPE_THRESHOLD       10    // px — minimum kaydırma mesafesi
#define SWIPE_RATIO           1.2f  // yatay/dikey baskınlık oranı
#define TOUCH_DEBOUNCE_US     12000 // 12ms — INT HIGH'ı gerçek "kalktı" saymak için
#define CC_CLOSE_DRAG_PX       6    // control center açıkken yukarı sürükleme eşiği
#define CC_TOUCH_DEBOUNCE_US  20000 // control center açıkken sahte release filtreleme

// Erken swipe penceresi: dokunuştan sonraki ilk N ms içinde threshold aşılırsa
// swipe sayılır. Sonrasında parmak "yerleşmiş" sayılır (long-press niyeti) ve
// kapasitif jitter swipe'ı yanlışlıkla tetiklemez. LVGL long-press 350 ms;
// swipe niyeti normalde ilk 200 ms içinde fiziksel olarak threshold'u aşar
// (tipik swipe hızı ≥200 px/sn → 200 ms'de 40 px).
#define SWIPE_EARLY_WINDOW_US 250000  // 250 ms

// ─── Jitter lock (koordinat smoothing) ────────────────────────────────────────
// Kapasitif touch sensörleri (FT3168 dahil) parmak sabit dururken bile sample
// başına 1–5 px tipik gürültü üretir. LVGL'nin iç scroll detection mekanizması
// (lv_indev.c: pointer.vect > scroll_limit ⇒ press_moved=1) bu gürültüyü gerçek
// hareket sanır ve şu sonuçları doğurur:
//   • CLICKED bastırılır → tap'lar bazen "kayboluyor"
//   • LONG_PRESSED tetiklenmez → basılı tutma 1. denemede çalışmaz
//
// Çözüm: parmak son raporlanan ("lock") noktanın JITTER_RADIUS yarıçapındaki
// dairede kaldığı sürece LVGL'ye lock noktasını GÖNDER (raw noktayı değil).
// Böylece pointer.vect = 0 olur ve LVGL parmağı gerçekten sabit görür.
// Sample radius dışına çıkarsa gerçek hareket başlamış demektir → lock yeni
// noktaya taşınır, normal scroll/drag işler.
//
// İç swipe takibi (s_ts_lx/ly) ham koordinatlarla devam eder; smoothing yalnızca
// LVGL'ye gönderilen veriyi etkiler — kendi swipe detection'ımız bozulmaz.
#define JITTER_RADIUS  4    // px — bu dairede lock noktası raporlanır

volatile int8_t g_swipe_dir   = 0;   // 0=yok -1=sol +1=sağ -2=yukarı +2=aşağı
static volatile bool s_swipe_locked = false;

void ui_service_lock_swipe(bool lock) { s_swipe_locked = lock; }
bool ui_service_is_swipe_locked(void)  { return s_swipe_locked; }

static int16_t  s_ts_x0             = -1;   // swipe başlangıcı
static int16_t  s_ts_y0             = -1;
static int16_t  s_ts_lx             = -1;   // son bilinen ham konum (swipe detection için)
static int16_t  s_ts_ly             = -1;
static int16_t  s_lock_x            = -1;   // LVGL'ye gönderilen smoothed konum
static int16_t  s_lock_y            = -1;
static bool     s_ts_down           = false;
/* Session 33 (C6 Session 17 portu): bu dokunuşta parmağın başlangıç noktasından
 * en fazla ne kadar uzaklaştığı (HAM koordinat). LVGL'ye yumuşatılmış/kilitli
 * koordinat gönderdiğimiz için LVGL'nin kendi "press_moved" ölçümü gerçek
 * hareketi olduğundan az görüyor → kaydırırken tıklama sayılabiliyordu. */
static int16_t  s_ts_travel_max     = 0;
/* Son gerçek "parmak kalktı" anı. Kaydırırken dokunmatik kısa süre "parmak yok"
 * raporlayınca dokunuş PARÇALANIYOR; yeni parça sapmayı sıfırdan sayıp tıklama
 * gibi görünüyordu. 250 ms içinde gelen yeni dokunuş, öncekinin devamı sayılır. */
static int64_t  s_ts_up_us          = 0;
/* Kaydırılabilir kaplar LV_EVENT_SCROLL'da ui_service_note_scroll() çağırır;
 * tıklama işleyicileri "az önce kaydırıldı mı" diye bakar. */
static int64_t  s_last_scroll_us    = 0;
static int64_t  s_ts_last_us        = 0;    // son "pressed" zamanı (µs)
static int64_t  s_ts_down_us        = 0;    // touchdown anı (erken swipe penceresi için)
static bool     s_swipe_early       = false; // parmak basılıyken yatay swipe algılandı
static int8_t   s_swipe_early_dir   = 0;    // erken tespit edilen yön
static bool     s_cc_opened_in_gesture = false; // bu dokunuşta CC'yi açtık → aynı jestte kapatma

// ─── Touch input callback (FT3168 → LVGL) ────────────────────────────────────
//
// FT3168 davranışı:
//   - Parmak basılıyken INT LOW, her ~10ms'de kısa süre HIGH (sample arası)
//   - Parmak kalkınca INT kalıcı HIGH
//
// Kritik sorun: INT geçici HIGH olduğunda data->state = RELEASED göndermek
//   LVGL'nin scroll state'ini kırar. LVGL sürekli PRESSED görmeden scroll başlatamaz.
//
// Çözüm: s_ts_down true iken INT HIGH → debounce dolmamışsa PRESSED (son koordinat)
//          Yalnızca debounce süresi geçince gerçek RELEASED gönderilir.

/* ── Sentetik dokunuş (komut servisi `tap` / `drag`) ────────────────────────
 * C6'dan port (Session 9). touch_read_cb'nin EN BAŞINDA gerçek dokunmatik
 * yolunu tamamen baypas eder → otonom UI testi gerçek tık gibi işlenir. */
static volatile int32_t s_syn_x = 0, s_syn_y = 0;       /* başlangıç / tap noktası */
static volatile int32_t s_syn_x1 = 0, s_syn_y1 = 0;     /* drag bitiş noktası */
static volatile int64_t s_syn_start_us = 0;
static volatile int64_t s_syn_until_us = 0;             /* 0 = aktif sentetik yok */
static volatile bool    s_syn_is_drag  = false;

void ui_service_inject_tap(int32_t x, int32_t y, uint32_t hold_ms)
{
    s_syn_x = x; s_syn_y = y; s_syn_x1 = x; s_syn_y1 = y;
    s_syn_is_drag  = false;
    s_syn_start_us = esp_timer_get_time();
    s_syn_until_us = s_syn_start_us + (int64_t)hold_ms * 1000;
}

void ui_service_inject_drag(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                            uint32_t dur_ms)
{
    s_syn_x = x0; s_syn_y = y0; s_syn_x1 = x1; s_syn_y1 = y1;
    s_syn_is_drag  = true;
    s_syn_start_us = esp_timer_get_time();
    s_syn_until_us = s_syn_start_us + (int64_t)dur_ms * 1000;
}

static void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    /* Sentetik tap/drag (Session 39): eskiden gerçek yolu tamamen baypas
     * edip LVGL'ye doğrudan nokta veriyordu → swipe tespiti, Control Center
     * jesti, jitter kilidi ve sapma sayacı sentetik dokunuşta ÇALIŞMIYORDU
     * (CC jesti komutla test edilemiyordu, sapma sayacı elle güncelleniyordu).
     * Artık sentetik nokta, donanım okumasının YERİNE geçer ve aşağıdaki
     * gerçek boru hattından aynen akar: `drag` = gerçek parmak. */
    bool    syn_live = false;
    int32_t syn_px = 0, syn_py = 0;
    if (s_syn_until_us) {
        int64_t now = esp_timer_get_time();
        int32_t px = s_syn_x, py = s_syn_y;
        if (s_syn_is_drag) {
            int64_t span = s_syn_until_us - s_syn_start_us;
            int64_t el   = now - s_syn_start_us;
            if (el < 0)    el = 0;
            if (el > span) el = span;
            if (span > 0) {
                px = s_syn_x + (int32_t)((s_syn_x1 - s_syn_x) * el / span);
                py = s_syn_y + (int32_t)((s_syn_y1 - s_syn_y) * el / span);
            }
        }
        if (now < s_syn_until_us) {
            syn_live = true; syn_px = px; syn_py = py;
        } else {
            s_syn_until_us = 0;   /* bu döngü: parmak yok → gerçek bırakma yolu */
        }
    }
#if defined(BOARD_VARIANT_LCD128) || defined(BOARD_VARIANT_NAR)
    /* CST816S (LCD128 board) / CST820 (NAR board, aynı pulse davranışı): INT davranışı FT3168'in "basılıyken sürekli
     * LOW" seviyesinden farklı (kısa pulse / doğrulanamayan pin) — INT
     * pinine gate'lenen akış dokunuşları hiç göremedi (cihaz testi
     * 2026-07-13). Init'te auto-sleep kapatıldığı için chip her cycle
     * güvenle poll'lanabilir: "parmak var" bilgisi pinden değil chip'in
     * kendi raporundan türetilir (~0.2 ms I2C @400kHz; LVGL tick 3-16 ms).
     * Basılıyken kesintisiz true döner — debounce mantığı FT'nin geçici
     * HIGH'larını hiç görmemiş gibi çalışır. AMOLED yolu değişmez. */
    bool int_active = syn_live;
    if (!syn_live) {
        ft3168_point_t int_poll;
        int_active = (ft3168_read(&int_poll) == ESP_OK) && int_poll.pressed;
    }
#else
    bool int_active = syn_live || (gpio_get_level((gpio_num_t)PIN_TOUCH_INT) == 0);
#endif
    int64_t now_us  = esp_timer_get_time();
    power_state_t pwr_state = power_service_get_state();

    // ── Display KAPALI iken (SLPIN) ilk dokunuş = sadece uyandır ─────────────
    // Display SLPIN'deyken FT3168'in I2C'den geçerli koordinat dönmesi ilk
    // örnekleme döngüsünden sonra olur; o nedenle ilk tap'ta pt.pressed=false
    // gelebilir → wake hiç tetiklenmez ve kullanıcı iki kez dokunmak zorunda
    // kalır. Çözüm: DISPLAY_OFF iken sadece INT pininin LOW olması wake
    // sinyali olarak kabul edilir, koordinat okuma beklenmez. O tap LVGL'ye
    // iletilmez (Apple Watch davranışı: kapalı ekrandan ilk tap sadece uyandırır).
    //
    // NOT: Daha önce 30 ms continuous-LOW hold-time guard eklenmişti, ama
    // FT3168 gerçek dokunuş sırasında INT'i her ~10 ms'de kısa süre HIGH
    // yapar (sample arası — bkz comment block aşağıda). Bu davranış 30 ms
    // continuous-LOW şartını imkansız kılıyordu → gerçek tap'ler ekrana
    // ulaşmıyordu. Spurious wake'in asıl kaynağı deep sleep EXT1'di
    // (GPIO 10 donanım sorunu, axp2101_driver.cpp'de mask'tan çıkarıldı);
    // bu guard'ın artık bir görevi yok. Tek INT-LOW örneği wake için yeterli.
    if (pwr_state == POWER_STATE_DISPLAY_OFF) {
        /* 2026-05-12: DISPLAY_OFF'tan touch ile uyanmada FT3168'in
         * touch_count register'ını oku → gerçekten parmak var mı doğrula.
         * Eski davranış: tek INT LOW okuması yetiyordu → USB kablosundan
         * capacitive coupling phantom INT'leri (kullanıcı raporu 2026-05-12
         * "ekran her ~60sn'de kendi kendine açılıyor") saati uyandırıyordu.
         * Yeni: INT fire etse bile FT3168 "parmak yok" diyorsa ignore. */
        if (int_active) {
            ft3168_point_t p;
            if (ft3168_read(&p) == ESP_OK && p.pressed) {
                /* Gerçek parmak teması — uyan. */
                power_service_user_activity(POWER_WAKE_SRC_TOUCH_OFF);
                s_ts_down              = false;
                s_swipe_early          = false;
                s_swipe_early_dir      = 0;
                s_cc_opened_in_gesture = false;
                s_ts_x0 = s_ts_y0 = -1;
                s_lock_x = s_lock_y    = -1;
            }
            /* p.pressed=false → spurious INT, ignore et, ekran kapalı kalsın */
        }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // ── DIM iken ekran GÖRÜNÜR (sadece parlaklık %15) ────────────────────────
    // FT3168 normal çalışır, koordinatlar geçerli → tap'ı YUTMA, parlaklığı
    // geri yükle ama dokunuşu LVGL'ye iletmeye DEVAM ET. Önceki davranış
    // (state != ACTIVE iken hep yut) kullanıcıya "bazen ilk dokunuşu algılamıyor"
    // hissi veriyordu — 8 sn idle'dan sonra ekrana bakıp tap atınca tap kayboluyordu.
    // Apple Watch'da da dim ekrandan doğrudan widget'a basabilirsin.
    if (int_active && pwr_state == POWER_STATE_DIM) {
        power_service_user_activity(POWER_WAKE_SRC_TOUCH_DIM);   // brightness 40 → 255, state → ACTIVE
        // RETURN ETME — touch akışı aşağıda normal işlesin.
    }

    // ── Parmak YOK veya debounce bölgesi ─────────────────────────────────────
    if (!int_active) {
        if (s_ts_down) {
            // Güvenlik: 3 saniye boyunca INT hiç LOW olmadıysa state'i sıfırla
            if ((now_us - s_ts_last_us) >= 3000000) {
                s_ts_down              = false;
                s_swipe_early          = false;
                s_swipe_early_dir      = 0;
                s_cc_opened_in_gesture = false;
                s_ts_x0 = s_ts_y0 = -1;
                s_lock_x = s_lock_y    = -1;
                ESP_LOGW(TAG, "Touch state reset (stuck)");
                data->state = LV_INDEV_STATE_RELEASED;
                return;
            }

            uint32_t debounce_us = control_center_is_open() ? CC_TOUCH_DEBOUNCE_US : TOUCH_DEBOUNCE_US;
            bool debounced = (now_us - s_ts_last_us) >= (int64_t)debounce_us;

            if (!debounced) {
                // FT3168'in geçici HIGH'ı — parmak hâlâ ekranda.
                // LVGL'ye smoothed (lock) koordinatla PRESSED gönder → scroll sürekliliği
                // korunur ve LVGL bu örnekte 0-vect görür (jitter yok).
                if (s_lock_x >= 0) {
                    data->state   = LV_INDEV_STATE_PRESSED;
                    data->point.x = s_lock_x;
                    data->point.y = s_lock_y;
                } else if (s_ts_lx >= 0) {
                    data->state   = LV_INDEV_STATE_PRESSED;
                    data->point.x = s_ts_lx;
                    data->point.y = s_ts_ly;
                } else {
                    data->state = LV_INDEV_STATE_RELEASED;
                }
                return;
            }

            // Debounce geçti → parmak gerçekten kalktı
            if (!s_swipe_locked) {
                if (s_swipe_early && s_swipe_early_dir != 0) {
                    g_swipe_dir = s_swipe_early_dir;
                    ESP_LOGI(TAG, "SWIPE (early): %s", s_swipe_early_dir < 0 ? "LEFT" : "RIGHT");
                } else if (s_ts_x0 >= 0) {
                    int dx  = s_ts_lx - s_ts_x0;
                    int dy  = s_ts_ly - s_ts_y0;
                    int adx = dx < 0 ? -dx : dx;
                    int ady = dy < 0 ? -dy : dy;
                    if (adx >= SWIPE_THRESHOLD && adx > (int)(ady * SWIPE_RATIO)) {
                        g_swipe_dir = (dx < 0) ? -1 : +1;
                        ESP_LOGI(TAG, "SWIPE: %s (dx=%d)", dx < 0 ? "LEFT" : "RIGHT", dx);
                    } else if (ady >= SWIPE_THRESHOLD && ady > (int)(adx * SWIPE_RATIO)) {
                        g_swipe_dir = (dy < 0) ? -2 : +2;
                        ESP_LOGI(TAG, "SWIPE: %s (dy=%d)", dy < 0 ? "UP" : "DOWN", dy);
                    }
                }
            }
            s_ts_down              = false;
            s_ts_up_us             = now_us;   /* Session 33: parçalanma penceresi */
            s_swipe_early          = false;
            s_swipe_early_dir      = 0;
            s_cc_opened_in_gesture = false;  // parmak kalktı → yeni jestte CC kapatılabilir
            s_ts_x0 = s_ts_y0 = -1;
            s_lock_x = s_lock_y    = -1;
        }

        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // ── Parmak VAR ────────────────────────────────────────────────────────────
    ft3168_point_t pt;
    bool have_pt = syn_live;
    if (syn_live) {
        pt.x = (uint16_t)syn_px; pt.y = (uint16_t)syn_py; pt.pressed = true;
    } else {
        have_pt = (ft3168_read(&pt) == ESP_OK) && pt.pressed;
    }
    if (!have_pt) {
        // INT aktif ama okuma başarısız — son lock noktasıyla PRESSED tut
        if (s_ts_down && s_lock_x >= 0) {
            data->state   = LV_INDEV_STATE_PRESSED;
            data->point.x = s_lock_x;
            data->point.y = s_lock_y;
        } else if (s_ts_down && s_ts_lx >= 0) {
            data->state   = LV_INDEV_STATE_PRESSED;
            data->point.x = s_ts_lx;
            data->point.y = s_ts_ly;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
        return;
    }

    s_ts_last_us = now_us;

    if (!s_ts_down) {
        s_ts_x0           = (int16_t)pt.x;
        s_ts_y0           = (int16_t)pt.y;
        s_ts_down         = true;
        s_ts_down_us      = now_us;
        /* Yeni dokunuş sapmayı sıfırlar — AMA 250 ms içinde gelen dokunuş,
         * parçalanmış bir kaydırmanın devamı kabul edilir (sapma korunur). */
        if (s_ts_up_us == 0 || (now_us - s_ts_up_us) > 250000) s_ts_travel_max = 0;
        s_swipe_early     = false;
        s_swipe_early_dir = 0;
        // Lock noktasını touchdown örneğine sabitle. LVGL bu noktayı pressed olarak
        // görür; jitter dairesinden çıkana kadar HER tick'te aynı noktayı raporlarız.
        s_lock_x          = (int16_t)pt.x;
        s_lock_y          = (int16_t)pt.y;
        // Yeni dokunma → power state machine'e aktivite bildir.
        // (Buton ve WOM zaten event_bus üzerinden bildiriliyor; touch için
        //  ayrı event yok, doğrudan API ile sayaç sıfırlanır.)
        power_service_user_activity(POWER_WAKE_SRC_TOUCH);
    } else {
        // Jitter filter: ham örnek lock dairesinin dışına çıkmadıysa lock değişmez
        // (LVGL aynı noktayı görür → vect=0 → press_moved tetiklenmez).
        // Daire dışına çıkınca gerçek hareket var demektir; lock yeni noktaya zıplar.
        int dlx = (int)pt.x - (int)s_lock_x;
        int dly = (int)pt.y - (int)s_lock_y;
        int adlx = dlx < 0 ? -dlx : dlx;
        int adly = dly < 0 ? -dly : dly;
        if (adlx > JITTER_RADIUS || adly > JITTER_RADIUS) {
            s_lock_x = (int16_t)pt.x;
            s_lock_y = (int16_t)pt.y;
        }
    }
    s_ts_lx = (int16_t)pt.x;
    s_ts_ly = (int16_t)pt.y;

    /* Ham sapmayı izle (kaydırma-tıklama ayrımı) */
    if (s_ts_x0 >= 0) {
        int adx = (int)pt.x - (int)s_ts_x0; if (adx < 0) adx = -adx;
        int ady = (int)pt.y - (int)s_ts_y0; if (ady < 0) ady = -ady;
        int mx  = adx > ady ? adx : ady;
        if (mx > s_ts_travel_max) s_ts_travel_max = (int16_t)mx;
    }

    // Control Center KAPALI iken — ekranın üstünden (y0 ≤ 40) aşağı sürükle → anında aç.
    // Parmak kalkmadan CC açılır ("tek hareketle"). CC açıldıktan sonra parmak kalkana
    // kadar kapatma mantığı devre dışı — aksi hâlde aynı jest içinde hemen kapanıyor.
    if (!control_center_is_open() && !s_swipe_early && s_ts_y0 >= 0 && s_ts_y0 <= 40) {
        int dy  = (int)s_ts_ly - s_ts_y0;
        int dxc = (int)s_ts_lx - s_ts_x0;
        int adx = dxc < 0 ? -dxc : dxc;
        /* Session 39: yalnız "aşağı 18 px" yetmiyordu — üst banttan (y≤40)
         * başlayan YATAY bir swipe 18 px aşağı sapınca, erken yatay tespit
         * henüz eşiğe varmadan CC açılıyordu (kullanıcı: "sağa sola kaydırınca
         * control center açılıyor"). Dikey hareket yatayı SWIPE_RATIO kadar
         * aşmalı — swipe tespitiyle aynı baskınlık kuralı. */
        if (dy >= 18 && dy > (int)(adx * SWIPE_RATIO)) {
            control_center_open();
            s_cc_opened_in_gesture = true;   // bu parmak kalkana kadar kapatma yok
            g_swipe_dir = 0;
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
    }

    // Control Center açıkken drag'i taptan ayır:
    // - küçük hareketten sonra click üretme
    // - yukarı hareketi anında kapat
    // - AMA: eğer CC bu jest içinde açıldıysa, parmak kalkana kadar kapatma yapma
    if (control_center_is_open() && s_ts_y0 >= 0) {
        if (s_cc_opened_in_gesture) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }

        int dx = (int)s_ts_lx - s_ts_x0;
        int dy = (int)s_ts_ly - s_ts_y0;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;

        // Drag intent başladıysa LVGL'ye PRESSED vermeyi kes (tile click olmasın)
        if (adx >= 6 || ady >= 6) {
            data->state = LV_INDEV_STATE_RELEASED;
        }

        if (dy <= -CC_CLOSE_DRAG_PX) {
            control_center_close();
            g_swipe_dir = 0;
            s_ts_down = false;
            s_swipe_early = false;
            s_swipe_early_dir = 0;
            s_ts_x0 = s_ts_y0 = -1;
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
    }

    // ── Erken yatay swipe tespiti ─────────────────────────────────────────────
    // Yatay baskın hareket threshold'u aşınca LVGL'ye tek seferlik RELEASED gönder
    // → item üzerindeki CLICKED/LONG_PRESS iptal olur, swipe yönü kaydedilir.
    //
    // ZAMAN PENCERESİ: Yalnızca touchdown'dan sonraki ilk SWIPE_EARLY_WINDOW_US
    // (250 ms) içinde fire edilir. Sonrasında parmak yerleşmiş sayılır →
    // long-press (350 ms) sırasında biriken kapasitif jitter (10 px) swipe
    // tetiklemez. Geç swipe'lar (yavaş hareket) parmak kalkınca FINAL detection'da
    // (line ~135) hâlâ yakalanır.
    int64_t since_down_us = (s_ts_down_us > 0) ? (now_us - s_ts_down_us) : 0;
    if (!s_swipe_early && !s_swipe_locked && s_ts_x0 >= 0 &&
        since_down_us < SWIPE_EARLY_WINDOW_US) {
        int dx  = (int)s_ts_lx - s_ts_x0;
        int dy  = (int)s_ts_ly - s_ts_y0;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        if (adx >= SWIPE_THRESHOLD && adx > (int)(ady * SWIPE_RATIO)) {
            s_swipe_early     = true;
            s_swipe_early_dir = (dx < 0) ? -1 : +1;
            ESP_LOGI(TAG, "SWIPE early: %s (dx=%d)", dx < 0 ? "LEFT" : "RIGHT", dx);
            // Güncel noktayı da gönder ki LVGL tarafındaki tap/swipe ayrımı
            // (ör. settings nav_list_cb) gerçek dx/dy üzerinden çalışsın.
            // Aksi halde RELEASED, eşik altındaki son PRESSED noktasıyla
            // gelir ve "tap" gibi görünür → swipe sırasında menüye giriliyor.
            data->point.x = pt.x;
            data->point.y = pt.y;
            data->state = LV_INDEV_STATE_RELEASED;   // LVGL'ye iptal sinyali
            return;
        }
    }

    // Erken yatay swipe modunda LVGL'yi sustur (geçiş animasyonu sırasında)
    if (s_swipe_early) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // Normal dokunuş → LVGL scroll/click mekanizması devralır.
    // Smoothed (lock) koordinatı raporla: parmak sabitse aynı nokta tekrar tekrar
    // gönderilir → LVGL'nin pointer.vect=0 → press_moved=0 → CLICKED ve LONG_PRESSED
    // güvenilir tetiklenir. Gerçek hareket varsa lock yukarıda yeni noktaya alınmıştır,
    // LVGL gerçek bir vect görür ve scroll/drag normal işler.
    data->state   = LV_INDEV_STATE_PRESSED;
    data->point.x = (s_lock_x >= 0) ? s_lock_x : pt.x;
    data->point.y = (s_lock_y >= 0) ? s_lock_y : pt.y;
}

// ─── ui_service_init ─────────────────────────────────────────────────────────

void ui_service_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL...");

    // Recursive mutex: aynı task LVGL kilidini birden fazla seferden alabilsin.
    // Touch callback (UI task içinde, tick mutex'i tutuyor) ekrana dokunulduğunda
    // power_service_user_activity → ui_service_show_wake_glow çağrısı yapıyor;
    // wake_glow da kendi içinde lock alır → recursive olmazsa deadlock.
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_lvgl_mutex);

    lv_init();

    // Display oluştur
    s_disp = lv_display_create(CO5300_H_RES, CO5300_V_RES);

    // Default temayı dark modda başlat — başlatılmazsa LVGL scrollbar rengini
    // rastgele/yeşil kullanabiliyor. Dark modda scrollbar gri/saydam olur.
    lv_theme_t* th = lv_theme_default_init(s_disp,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_GREY),  // CYAN yeşil bileşeni nedeniyle scrollbar'ı yeşil gösterebilir
        true,                               // dark mode
        &lv_font_montserrat_14);
    lv_display_set_theme(s_disp, th);

    // ─── Display buffer allocation — smart ladder (v1.4.54, audit #6 fix) ──
    //
    // Boot heap fragmentation'a karşı dayanıklılık: önce DISPLAY_BUF_LINES
    // (target=80), başarısızsa graceful linear fallback (-20 step) ile
    // büyük adımlardan kaçın. Mevcut log seviyesinde "achieved vs target"
    // farkı + DMA-RAM contiguous miktar ile boot zamanı debug info.
    //
    // Ladder: 80 → 60 → 40 → 20 → 10 → 8 (regression dar penceresi).
    static const uint16_t k_lines_ladder[] = {
        DISPLAY_BUF_LINES,  // target (os_config.h: 80)
        60, 40, 20, 10, 8
    };

    size_t lines     = 0;
    void*  buf1      = NULL;
    void*  buf2      = NULL;
    size_t part_size = 0;
    bool   single_buf = false;

    /* Boot anındaki DMA-RAM contiguous max — fragmentation göstergesi */
    size_t dma_free_max = heap_caps_get_largest_free_block(
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "DMA-RAM contiguous max at boot: %u B (~%u KB)",
             (unsigned)dma_free_max, (unsigned)(dma_free_max / 1024));

    for (size_t i = 0; i < sizeof(k_lines_ladder) / sizeof(k_lines_ladder[0]); i++) {
        lines = k_lines_ladder[i];
        part_size = (size_t)lines * CO5300_H_RES * CO5300_COLOR_SIZE;

        /* Try double-buffer */
        buf1 = heap_caps_malloc(part_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        buf2 = heap_caps_malloc(part_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (buf1 && buf2) {
            single_buf = false;
            break;
        }
        if (buf1) { heap_caps_free(buf1); buf1 = NULL; }
        if (buf2) { heap_caps_free(buf2); buf2 = NULL; }

        /* Try single-buffer at this size */
        buf1 = heap_caps_malloc(part_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (buf1) {
            single_buf = true;
            break;
        }
    }

    configASSERT(buf1);

    const size_t target_lines = (size_t)DISPLAY_BUF_LINES;
    if (single_buf) {
        ESP_LOGW(TAG, "LVGL buffer: PARTIAL single (%u B, lines=%u) "
                      "FALLBACK from target=%u (DMA-RAM short — render perf degraded)",
                 (unsigned)part_size, (unsigned)lines, (unsigned)target_lines);
        lv_display_set_buffers(s_disp, buf1, NULL, part_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    } else if (lines == target_lines) {
        ESP_LOGI(TAG, "LVGL buffer: PARTIAL (%u B x2, lines=%u) target HIT",
                 (unsigned)part_size, (unsigned)lines);
        lv_display_set_buffers(s_disp, buf1, buf2, part_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    } else {
        ESP_LOGW(TAG, "LVGL buffer: PARTIAL (%u B x2, lines=%u) "
                      "FALLBACK from target=%u (DMA-RAM fragmentation)",
                 (unsigned)part_size, (unsigned)lines, (unsigned)target_lines);
        lv_display_set_buffers(s_disp, buf1, buf2, part_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    }
    lv_display_set_flush_cb(s_disp, co5300_lvgl_flush_cb);

    ESP_LOGI(TAG, "LVGL init OK");
}

// ─── ui_service_register_touch ───────────────────────────────────────────────

void ui_service_register_touch(void)
{
    s_touch_dev = lv_indev_create();
    lv_indev_set_type(s_touch_dev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_touch_dev, touch_read_cb);
    /* v1.2.6 (2026-05-22): 350 → 200 ms. Önceki değer çok uzundu — kullanıcı
     * Launcher edit mode için long-press yaparken 350 ms beklemeden parmağını
     * kaldırıyor → CLICKED tetikleniyor → app launch oluyor. 200 ms hala
     * normal tap'ten net ayırt edilebilir ama hissedilir kısa. */
    lv_indev_set_long_press_time(s_touch_dev, 200);

    /* TOUCH RESPONSIVENESS — Apple Watch level "first tap = action".
     * ─────────────────────────────────────────────────────────────────
     * LVGL default scroll_limit=10 px FT3168 jitter envelope'u (sample
     * başına 1-5 px) için fazla dar:
     *   - 2-3 sample boyunca biriken jitter 10 px'i aşar →
     *     LVGL press_moved=1 → CLICKED suppressed → kullanıcı 2-3 kez
     *     dokunmak zorunda.
     *
     * scroll_limit=20: jitter dairemizden (4 px lock + tolerance) büyük;
     * gerçek scroll niyetini yine yakalar ama jitter CLICKED suppress
     * etmez. (LVGL 9.2'de set_gesture_limit API'si yok — gesture eşiği
     * default 50 px kalır, jitter pratik olarak bunu aşmadığı için
     * sorun değil.) */
    lv_indev_set_scroll_limit(s_touch_dev, 20);

    ESP_LOGI(TAG, "Touch input registered (scroll_limit=20)");
}

// ─── ui_service_tick ─────────────────────────────────────────────────────────

uint32_t ui_service_tick(void)
{
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
    uint32_t next = lv_timer_handler();
    xSemaphoreGiveRecursive(s_lvgl_mutex);
    return next;
}

// ─── ui_service_lock / unlock ────────────────────────────────────────────────

void ui_service_lock(void)
{
    /* NULL-safe: sync wake modunda task_ui spawn edilmediği için
     * ui_service_init çağrılmamış olabilir → s_lvgl_mutex NULL kalır.
     * Eskiden bu durumda xSemaphoreTakeRecursive(NULL, ...) PANIC ediyordu
     * → saat 60 sn'de cycle reboot oluyordu (kullanıcı raporu 2026-05-12).
     * UI yoksa kilit gereksiz → no-op. */
    if (!s_lvgl_mutex) return;
    xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY);
}

void ui_service_unlock(void)
{
    if (!s_lvgl_mutex) return;
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

// ─── Wake glow (Apple Watch tarzı uyandırma çerçevesi) ───────────────────────
//
// Ekran DISPLAY_OFF'tan açılınca üst katmanda (lv_layer_top) tam ekran
// transparent bir obje oluştur, dış kenarına mavi border + outline ver,
// opacity'yi LV_OPA_COVER'dan 0'a animasyonla düşür → animasyon biter bitmez sil.
// LV_RADIUS_CIRCLE büyük yarıçap sayesinde 410×502 panele tam oturur ve
// AMOLED'in siyah maskesinin altında kalan köşeler görünmez.

static lv_obj_t* s_wake_glow = NULL;

static void wake_glow_opa_cb(void* var, int32_t v)
{
    // style_opa (cascade) — border ve outline dahil tüm boyamayı oranlar.
    lv_obj_set_style_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}

static void wake_glow_done_cb(lv_anim_t* a)
{
    lv_obj_t* obj = (lv_obj_t*)a->var;
    if (obj == s_wake_glow) {
        s_wake_glow = NULL;
    }
    lv_obj_delete(obj);
}

void ui_service_show_wake_glow(void)
{
    /* NULL-safe: ui_service_init çağrılmamışsa (sync wake mode) LVGL yok →
     * wake glow çağrılamaz, panic'i önle. */
    if (!s_lvgl_mutex) return;
    ui_service_lock();

    // Önceki overlay hâlâ varsa temizle (üst üste binme korumalı tetikleme)
    if (s_wake_glow) {
        lv_anim_delete(s_wake_glow, NULL);
        lv_obj_delete(s_wake_glow);
        s_wake_glow = NULL;
    }

    lv_obj_t* top = lv_layer_top();
    s_wake_glow = lv_obj_create(top);
    lv_obj_remove_style_all(s_wake_glow);
    lv_obj_set_size(s_wake_glow, CO5300_H_RES, CO5300_V_RES);
    lv_obj_set_pos(s_wake_glow, 0, 0);
    lv_obj_set_style_bg_opa(s_wake_glow, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_wake_glow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_wake_glow, LV_OBJ_FLAG_SCROLLABLE);

    // Mavi parlayan çerçeve — Apple Watch tarzı squircle (yuvarlatılmış-dikdörtgen).
    // Tek katman border, outline yok. (Outline farklı opacity'de fade ediyor ve
    // border söndükçe içte ikinci bir çizgi gibi gözüküyordu — "tekrar çiziliyor"
    // izlenimi.)
    lv_obj_set_style_radius(s_wake_glow, 70, 0);
    lv_obj_set_style_border_width(s_wake_glow, 6, 0);
    lv_obj_set_style_border_side(s_wake_glow, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_border_color(s_wake_glow, lv_color_make(0x0A, 0x84, 0xFF), 0);  // iOS systemBlue
    lv_obj_set_style_border_opa(s_wake_glow, LV_OPA_COVER, 0);

    // Başlangıçta tam görünür → ease-out ile saydamlaş.
    lv_obj_set_style_opa(s_wake_glow, LV_OPA_COVER, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_wake_glow);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    /* v1.2.1 (2026-05-18): 500 ms linear → 380 ms quintic ease-out.
     * Wake glow "modal sheet present/dismiss" kategorisinde (HIG ~350-400 ms);
     * 500 ms biraz uzun, linear "mekanik" his veriyordu. Quint son frame'lerde
     * yumuşak söner, wear_motion karakteriyle eşleşir. */
    lv_anim_set_time(&a, WEAR_ANIM_SLOW);   /* 380 ms */
    lv_anim_set_delay(&a, 60);              // ekran tam açılınca beliriyor gibi
    lv_anim_set_path_cb(&a, wear_anim_path_ease_out_quint);
    lv_anim_set_exec_cb(&a, wake_glow_opa_cb);
    lv_anim_set_completed_cb(&a, wake_glow_done_cb);
    lv_anim_start(&a);

    ui_service_unlock();
}


// ─── Kaydırma-tıklama ayrımı (C6 Session 17 portu) ───────────────────────────

int32_t ui_service_touch_travel_px(void)
{
    return s_ts_travel_max;
}

void ui_service_note_scroll(void)
{
    s_last_scroll_us = esp_timer_get_time();
}

bool ui_service_scroll_recent(uint32_t within_ms)
{
    if (s_last_scroll_us == 0) return false;
    return (esp_timer_get_time() - s_last_scroll_us) < (int64_t)within_ms * 1000;
}

// ─── Teşhis: dokunmatik durum satırı ([beat] logu) ───────────────────────────

void ui_service_debug_state(char* out, unsigned n)
{
    snprintf(out, n, "down=%d early=%d(dir%d) lock=%d gdir=%d cc=%d trv=%d scr=%d",
             (int)s_ts_down, (int)s_swipe_early, (int)s_swipe_early_dir,
             (int)s_swipe_locked, (int)g_swipe_dir,
             (int)control_center_is_open(),
             (int)s_ts_travel_max, (int)ui_service_scroll_recent(300));
}

// ─── LVGL ağaç dökümü (komut servisi `ui dump`) ──────────────────────────────

static void dump_obj_rec(lv_obj_t* obj, int depth, ui_dump_line_fn_t out, void* ctx,
                         int* budget)
{
    if (!obj || *budget <= 0) return;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t a; lv_obj_get_coords(obj, &a);
    const lv_obj_class_t* cls = lv_obj_get_class(obj);
    const char* cname = "obj";
    if (cls == &lv_label_class)         cname = "label";
    else if (cls == &lv_image_class)    cname = "img";
    else if (cls == &lv_button_class)   cname = "btn";
    else if (cls == &lv_arc_class)      cname = "arc";
    else if (cls == &lv_slider_class)   cname = "slider";
    else if (cls == &lv_textarea_class) cname = "textarea";
    char line[160];
    int n = snprintf(line, sizeof(line), "%*s%s %p [%d,%d %dx%d]%s",
                     depth * 2, "", cname, (void*)obj,
                     (int)a.x1, (int)a.y1,
                     (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1),
                     lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) ? " C" : "");
    if (cls == &lv_label_class && n > 0 && n < (int)sizeof(line)) {
        const char* t = lv_label_get_text(obj);
        /* tw = metnin gerçek piksel genişliği (tek satır varsayımı) */
        const lv_font_t* f = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        int32_t tw = t ? lv_text_get_width(t, (uint32_t)strlen(t), f,
                                           lv_obj_get_style_text_letter_space(obj, LV_PART_MAIN)) : 0;
        snprintf(line + n, sizeof(line) - n, " tw=%d \"%.40s\"", (int)tw, t ? t : "");
    }
    out(line, ctx);
    (*budget)--;
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt && *budget > 0; i++)
        dump_obj_rec(lv_obj_get_child(obj, i), depth + 1, out, ctx, budget);
}

void ui_service_dump_tree(ui_dump_line_fn_t out, void* ctx)
{
    if (!out) return;
    ui_service_lock();
    int budget = 400;   /* satır üst sınırı — seri kanalı boğmasın */
    lv_obj_t* scr = lv_screen_active();
    if (scr) dump_obj_rec(scr, 0, out, ctx, &budget);
    lv_obj_t* top = lv_layer_top();
    if (top && lv_obj_get_child_count(top) > 0) {
        out("-- top layer --", ctx);
        dump_obj_rec(top, 0, out, ctx, &budget);
    }
    ui_service_unlock();
    if (budget <= 0) out("-- (truncated) --", ctx);
}
