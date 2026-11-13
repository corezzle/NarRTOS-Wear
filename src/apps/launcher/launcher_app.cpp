/**
 * @file launcher_app.cpp
 * @brief Ana ekran — Apple Watch tarzı BAL PETEĞİ (honeycomb) launcher
 *
 * Session 33: Kare-ızgara (3 sütun, ikon + altında isim) düzeni bırakıldı;
 * yerine Apple Watch'un "grid view" bal peteği geldi. Düzen kardeş proje
 * narRtos-wear-c6'dan (Session 23) portlandı; board sabitleri bu depodaki
 * üç varyanta göre ayarlandı (240 yuvarlak / 368x448 / 410x502).
 *
 *   • Daire ikonlar altıgen kafeste — satırlar sırayla 3 / 2 / 3 / 2 …
 *     kapasiteli, her satır kendi içinde ortalanır → komşu satırlar yarım
 *     adım kaymış olur (petek görünümü).
 *   • BALIK GÖZÜ (fisheye) ölçekleme: ekran merkezine yakın ikon büyük,
 *     kenara gidildikçe küçülür. s = R² / (R² + k·d²) — karekök yok,
 *     C6'da (FPU'suz RISC-V) tamsayı aritmetiğiyle çalışır.
 *   • Dikey kaydırma: uygulama sayısı arttıkça petek yukarı/aşağı kayar,
 *     ikonlar kaydıkça yeniden ölçeklenir. YATAY kaydırma YOK — yatay
 *     swipe global "geri" jesti olarak kalmalı (app_manager check_swipe).
 *   • İsim etiketi yok (Apple Watch'ta da yok); ikona basılı tutulunca
 *     ikonun yanında beyaz bir "pill" içinde uygulama adı belirir.
 *
 * Korunanlar: watch/wifi/batarya üst şeridi, uzun-bas → düzenleme modu,
 * (-) rozeti + kaldırma onayı, batarya event'i, geri jesti.
 */

#include <sdkconfig.h>
#include "launcher_app.h"
#include "../../core/app_manager.h"
#include "../../core/os_config.h"
#include "../../core/event_bus.h"
#include "../../services/ui_service.h"
#include "../../services/wifi_service.h"
#include "../../drivers/power/axp2101_driver.h"
#include "../../assets/partly_cloud_icon.h"
#include "../../apps/store/store_state.h"   /* v1.2.6: downloadable tile runtime */
#include "../../../include/wear_motion.h"
#include <esp_timer.h>   /* v1.2.6: manuel long-press timing */
#include <lvgl.h>
#include <esp_log.h>
#include <time.h>

static const char* TAG = "Home";

/* ╔═══════════════════════════════════════════════════════════════════════╗
 * ║ GEÇİCİ TEST (Session 43): petek dikey kaydırmasını görmek için sahte  ║
 * ║ ikonlar. Tıklanınca hiçbir şey açmaz (id=APP_ID_COUNT → launch reddi).║
 * ║ Test bitince 0 yapın / bloğu silin.                                   ║
 * ╚═══════════════════════════════════════════════════════════════════════╝ */
#define LAUNCHER_DEMO_ICONS 0

/* Saat label'ı tek parça olduğunda Montserrat (proportional) fontunda
 * "1" ve "0" gibi rakamların glyph genişlikleri farklı olduğu için
 * dakika değiştikçe rakamlar yan yana kayıp bulanık görünüyordu.
 * Watchface'deki gibi her digit kendi sabit-konum label'ına alındı. */
static lv_obj_t*   s_t_h0      = NULL;  /* saat onlar  */
static lv_obj_t*   s_t_h1      = NULL;  /* saat birler */
static lv_obj_t*   s_t_m0      = NULL;  /* dakika onlar  */
static lv_obj_t*   s_t_m1      = NULL;  /* dakika birler */
static lv_obj_t*   s_lbl_bat   = NULL;
static lv_obj_t*   s_lbl_wifi  = NULL;
static lv_timer_t* s_clk_timer = NULL;
static lv_obj_t*   s_scroll    = NULL;   /* dikey kaydırılan görüş penceresi */
static lv_obj_t*   s_canvas    = NULL;   /* sabit boyutlu petek tuvali */
static lv_obj_t*   s_pill      = NULL;   /* basılı tutunca çıkan isim balonu */
static lv_obj_t*   s_pill_lbl  = NULL;

/* ─── Batarya ikon güncelleme yardımcısı ──────────────────────────────────────
 *
 * Üst-sağ köşedeki batarya etiketini gerçek PMIC durumuna göre günceller.
 * Event geldiğinde event içeriğini kullanmak yerine doğrudan AXP2101'i
 * sorguluyoruz — bu sayede:
 *   1) Pil takılı değilse (USB-only)            → LV_SYMBOL_USB    (cyan)
 *   2) Şarj olurken                              → LV_SYMBOL_CHARGE (yeşil)
 *   3) Yüzdeye göre 5 kademeli batarya simgesi  → EMPTY/1/2/3/FULL
 *
 * I2C maliyeti: bir kaç register read; power_service zaten saniyede bir kez
 * tetikliyor (event), launcher tarafı bu kadar taze veri işlemekle yükümlü.
 */
static void apply_battery_visual(void)
{
    if (!s_lbl_bat) return;

    axp2101_bat_status_t bat = {};
    if (axp2101_get_bat_status(&bat) != ESP_OK) {
        lv_obj_set_style_text_color(s_lbl_bat, lv_color_make(0x77, 0x77, 0x77), 0);
        lv_label_set_text(s_lbl_bat, LV_SYMBOL_BATTERY_FULL);
        return;
    }

    lv_color_t  col;
    const char* sym;

    if (!bat.bat_present) {
        // Pil takılı değil; sadece USB'den besleniyor
        col = lv_color_make(0x4A, 0xC0, 0xFF);   // cyan
        sym = LV_SYMBOL_USB;
    } else if (bat.charge_state == AXP_CHARGE_CHARGING) {
        col = lv_color_make(0x4F, 0xE0, 0x9D);
        sym = LV_SYMBOL_CHARGE;
    } else if (bat.percent < 10) {
        col = lv_color_make(0xFF, 0x44, 0x44);
        sym = LV_SYMBOL_BATTERY_EMPTY;
    } else if (bat.percent < 30) {
        col = lv_color_make(0xFF, 0x88, 0x44);
        sym = LV_SYMBOL_BATTERY_1;
    } else if (bat.percent < 60) {
        col = lv_color_make(0xFF, 0xCC, 0x00);
        sym = LV_SYMBOL_BATTERY_2;
    } else if (bat.percent < 85) {
        col = lv_color_make(0xCC, 0xCC, 0xCC);
        sym = LV_SYMBOL_BATTERY_3;
    } else {
        col = lv_color_make(0xCC, 0xCC, 0xCC);
        sym = LV_SYMBOL_BATTERY_FULL;
    }

    lv_obj_set_style_text_color(s_lbl_bat, col, 0);
    lv_label_set_text(s_lbl_bat, sym);
}

// ─── Gesture (sola kaydır → saat kadranına dön) ───────────────────────────────

static void gesture_cb(lv_event_t* e)
{
    (void)e;
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_LEFT) {
        app_manager_back();
    }
}

// ─── Tile tıklama ─────────────────────────────────────────────────────────────

/* v1.2.6 (2026-05-22): Apple iOS Home Screen "edit mode" pattern.
 * Downloadable installed tile'a long-press → edit mode aktif. Tüm
 * downloadable tile'ların üzerinde kırmızı (-) badge çıkar. Badge tap →
 * uninstall confirm modal. Edit mode'da herhangi bir tile'a normal tap →
 * edit mode kapanır (launch yok). System tile'lar (Settings, Gallery,
 * Weather, Alarms, Store) long-press'i ignore eder. */
static bool      s_edit_mode        = false;
static lv_obj_t* s_uninstall_modal  = NULL;
static app_id_t  s_uninstall_target = (app_id_t)0;

/* v1.2.6 (2026-05-22): Manuel long-press timing.
 *
 * LVGL'in LONG_PRESSED event'i hassas — kullanıcı 150-250 ms arası tutarsız
 * süre basıyor, threshold'un altında kalan basışlar CLICKED olarak geliyordu.
 * Daha güvenilir: tile_press_cb'de timestamp kaydet, tile_click_cb'de geçen
 * süreyi kontrol et. >= LONG_PRESS_MS ise long-press olarak yorumla. */
static int64_t s_press_start_us = 0;
/* v1.2.6: 250 → 220 ms. Kullanıcı 240 ms civarı basıp kalktığında 250 eşiği
 * geçilmiyor, normal tap olarak yorumlanıp launch oluyordu. 220 ms daha geniş
 * margin verir — long-press tetikleme daha güvenilir. Normal tap (100-150 ms)
 * hala net ayırt edilebilir. */
#define LONG_PRESS_MS 220

/* ─── Board-varyant petek sabitleri (Session 23) ──────────────────────────────
 *
 * HC_TOP       : watch/wifi/batarya şeridinin kapladığı üst bant
 * HC_ICON_D    : merkezdeki (tam ölçek) daire çapı
 * HC_PITCH_X   : aynı satırdaki iki ikon merkezi arası
 * HC_PITCH_Y   : satır aralığı ≈ HC_PITCH_X · √3/2 (düzgün altıgen kafes)
 * HC_FISHEYE_R : balık gözü yarıçapı — merkeze bu kadar uzaklıkta ölçek
 *                ~1/(1+k) olur. Küçültmek küçülmeyi keskinleştirir.
 * HC_MIN_S     : 8.8 sabit-nokta alt sınır (256 = 1.00×)
 *
 * Satır genişliği kontrolü: (n-1)·PITCH_X + ICON_D ≤ SCR_W olmalı.
 *   LCD128 : 2·72 + 62 = 206 ≤ 240 ✓
 *   1.8"   : 2·110 + 92 = 312 ≤ 368 ✓
 *   AMOLED : 2·122 + 102 = 346 ≤ 410 ✓
 */
#ifdef BOARD_VARIANT_LCD128
#define HC_SCR_W        240
#define HC_SCR_H        240
#define HC_TOP          44
#define HC_ICON_D       56   /* Session 43b: tüm daireler eşit (balık gözü kapalı) */
#define HC_PITCH_X      72
#define HC_PITCH_Y      62
#define HC_FISHEYE_R    110
#define HC_WEATHER_SCALE 192          /* Session 43: 40px asset → 30px (0.48 × 62) —
                                       * glyph ikonlarla aynı doluluk; 320'de bulut
                                       * komşularının iki katı görünüyordu */
#define HC_PAD_TOP      12
#define HC_BADGE_D      20
#define L_MODAL_W       220
#define L_MODAL_H       170
#define L_MODAL_TXT_W   180
#define L_MODAL_DESC_Y  64
#elif defined(DISPLAY_VARIANT_18INCH)
/* ESP32-S3-Touch-AMOLED-1.8 (368×448) */
#define HC_SCR_W        368
#define HC_SCR_H        448
#define HC_TOP          50
#define HC_ICON_D       92
#define HC_PITCH_X      110
#define HC_PITCH_Y      95
#define HC_FISHEYE_R    200
#define HC_WEATHER_SCALE 470          /* 40px asset → ~73px (0.80 × 92) */
#define HC_PAD_TOP      28
#define HC_BADGE_D      26
#define L_MODAL_W       290
#define L_MODAL_H       210
#define L_MODAL_TXT_W   250
#define L_MODAL_DESC_Y  72
#else
#define HC_SCR_W        410
#define HC_SCR_H        502
#define HC_TOP          50
#define HC_ICON_D       102
#define HC_PITCH_X      122
#define HC_PITCH_Y      106
#define HC_FISHEYE_R    225
#define HC_WEATHER_SCALE 520          /* 40px asset → ~81px (0.80 × 102) */
#define HC_PAD_TOP      30
#define HC_BADGE_D      26
#define L_MODAL_W       320
#define L_MODAL_H       230
#define L_MODAL_TXT_W   280
#define L_MODAL_DESC_Y  80
#endif

/* Balık gözü sertliği: s = R² / (R² + k·d²), k = HC_FISHEYE_K/256 */
#ifdef BOARD_VARIANT_LCD128
/* Session 43b: 240 yuvarlak ekranda balık gözü KAPALI — kullanıcı "Galeri
 * büyük, Ayarlar/Hava küçük, neden farklı?" dedi; küçük ekranda derinlik
 * hissi değil tutarsızlık gibi okunuyor. k=0 → her ikon tam boy (56 px).
 * Kaydırma ve satır yerleşimi aynen çalışıyor. */
#define HC_FISHEYE_K    0
#define HC_MIN_S        256
#else
#define HC_FISHEYE_K    230           /* ≈ 0.90 */
#define HC_MIN_S        110           /* 0.43× — en kenardaki ikon bundan küçülmez */
#endif
#define HC_VIEW_H       (HC_SCR_H - HC_TOP)
/* Balık gözü odağı: ekranın tam ortası değil, biraz yukarısı (2/5). İçerik
 * üste yaslandığı için odak ortada kalsaydı en büyük ikonlar grubun en altına
 * düşer, petek alt-ağır görünürdü. */
#define HC_FOCUS_Y      (HC_VIEW_H * 3 / 8)
#define HC_MAX_ICONS    16
#define HC_ROW_CAP(r)   (((r) & 1) ? 2 : 3)   /* 3 / 2 / 3 / 2 … */

/* ─── Petek ikon kaydı ─────────────────────────────────────────────────────── */
typedef struct {
    lv_obj_t*   obj;        /* daire */
    lv_obj_t*   sym;        /* glyph label (Weather'da NULL) */
    lv_obj_t*   img;        /* raster ikon (yalnız Weather) */
    lv_obj_t*   badge;      /* edit mode (-) rozeti, yoksa NULL */
    const char* name;
    app_id_t    id;
    int16_t     cx, cy;     /* tuval içindeki merkez */
    int16_t     cur_d;      /* uygulanmış çap (gereksiz invalidate'i önler) */
    int8_t      cur_font;   /* uygulanmış font kovası */
} hc_icon_t;

static hc_icon_t s_icons[HC_MAX_ICONS];
static int       s_icon_count = 0;

static void hc_build(void);               /* forward */
static void enter_edit_mode(void);
static void exit_edit_mode(void);
static void show_launcher_uninstall_confirm(app_id_t id);
static void hc_pill_hide(void);

/* v1.2.6: tile_press_cb manuel timing için zaman damgası alır + swipe lock. */
static bool is_downloadable_installed(app_id_t id)
{
    const size_t cn = store_catalog_count();
    for (size_t i = 0; i < cn; i++) {
        const store_app_entry_t* e = store_catalog_at(i);
        if (e && e->id == id) {
            return !e->is_system && store_is_installed(id);
        }
    }
    return false;
}

static void tile_click_cb(lv_event_t* e)
{
    /* Session 23: petek artık dikey kaydırılıyor — kaydırma sırasında parmak
     * bir ikonun üstünden kalkarsa LVGL bunu CLICKED sayabiliyor. Ham dokunuş
     * sapması ve "az önce kaydırıldı mı" bilgisiyle bu tıklamayı yut
     * (galeri/ayarlar listelerindeki kalıbın aynısı). */
    if (ui_service_touch_travel_px() > 10 || ui_service_scroll_recent(300)) {
        hc_pill_hide();
        return;
    }

    /* v1.2.6: Manuel long-press timing — LVGL LONG_PRESSED hassas, manuel
     * süre kontrolü daha güvenilir. PRESSED'de timestamp alındı; geçen
     * süre LONG_PRESS_MS eşiğini aşıyorsa long-press olarak yorumla. */
    int64_t duration_ms = 0;
    if (s_press_start_us > 0) {
        duration_ms = (esp_timer_get_time() - s_press_start_us) / 1000;
    }

    app_id_t id = (app_id_t)(uintptr_t)lv_event_get_user_data(e);

    if (duration_ms >= LONG_PRESS_MS) {
        /* Long press — downloadable installed tile ise edit mode toggle.
         * Sistem app long-press = no-op (silinemez). */
        if (is_downloadable_installed(id)) {
            if (s_edit_mode) exit_edit_mode();   /* zaten açıksa kapat */
            else             enter_edit_mode();
        }
        /* Long press launch yapmaz — system tile long-press de sessizce yutulur */
        return;
    }

    /* Normal kısa tap */
    if (s_edit_mode) {
        /* v1.4.64 (2026-06-08): Edit mode'da downloadable installed tile'a
         * tap = uninstall confirm. Önceden sadece sol-üst kırmızı (-)
         * badge'ine tıklamak gerekiyordu — kullanıcı raporu: "küçük silme
         * butonuna zor tıklanıyor". Apple iOS'un "tap-anywhere-on-icon"
         * davranışından bile daha cömert: tile'ın HER YERİ delete'i tetikler.
         * System app'ler (badge yok) → exit edit mode (eski davranış). */
        if (is_downloadable_installed(id)) {
            show_launcher_uninstall_confirm(id);
            return;
        }
        /* System tile (silinemez) → edit mode'dan çık (Apple "Done" eşdeğeri) */
        exit_edit_mode();
        return;
    }
    app_manager_launch(id);
}

/* (-) badge tap → uninstall confirm modal. user_data app_id taşır. */
static void minus_badge_click_cb(lv_event_t* e)
{
    lv_obj_t* badge = (lv_obj_t*)lv_event_get_current_target(e);
    app_id_t id = (app_id_t)(uintptr_t)lv_obj_get_user_data(badge);
    show_launcher_uninstall_confirm(id);
}

// ─── İsim balonu (basılı tutunca) ────────────────────────────────────────────
//
// Petek görünümünde ikonların altında isim yok (Apple Watch'ta da yok).
// Referans tasarımdaki gibi: parmak bir ikonun üstündeyken o ikonun yanında
// beyaz bir "pill" içinde uygulama adı belirir, parmak kalkınca kaybolur.

static void hc_pill_hide(void)
{
    if (s_pill) lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
}

static void hc_pill_show(const hc_icon_t* ic)
{
    if (!s_pill || !s_pill_lbl || !ic || !ic->name) return;
    if (s_edit_mode) return;   /* silme modunda balon gürültü yapmasın */

    lv_label_set_text(s_pill_lbl, ic->name);
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_pill);

    const int32_t sy = s_scroll ? lv_obj_get_scroll_y(s_scroll) : 0;
    const int pw = lv_obj_get_width(s_pill);
    const int ph = lv_obj_get_height(s_pill);
    const int d  = ic->cur_d > 0 ? ic->cur_d : HC_ICON_D;

    /* İkonun EKRAN koordinatındaki merkezi (pill screen'in child'ı) */
    const int scx = ic->cx;
    const int scy = HC_TOP + ic->cy - (int)sy;

    int px = scx - pw / 2;
    if (px < 6)                px = 6;
    if (px > HC_SCR_W - pw - 6) px = HC_SCR_W - pw - 6;

    /* Tercihen ikonun üstünde; üstte yer yoksa altında */
    int py = scy - d / 2 - ph - 8;
    if (py < HC_TOP + 2) py = scy + d / 2 + 8;
    if (py > HC_SCR_H - ph - 4) py = HC_SCR_H - ph - 4;

    lv_obj_set_pos(s_pill, px, py);
    lv_obj_move_foreground(s_pill);
}

static void tile_press_cb(lv_event_t* e)
{
    s_press_start_us = esp_timer_get_time();   /* v1.2.6: long-press timing */

    /* Session 23: swipe kilidi ARTIK ALINMIYOR. Petek yalnız dikey kayıyor;
     * yatay swipe global "geri" jesti ve ui_service zaten yatay-baskın
     * harekette LVGL'ye RELEASED gönderip tıklamayı iptal ediyor. Basışta
     * kilitlemek, ikondan başlayan geri swipe'ını yutuyordu. */
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_current_target(e);
    for (int i = 0; i < s_icon_count; i++) {
        if (s_icons[i].obj == obj) { hc_pill_show(&s_icons[i]); break; }
    }
}

static void tile_release_cb(lv_event_t* e)
{
    (void)e;
    hc_pill_hide();
}

// ─── Edit mode helper'ları ───────────────────────────────────────────────────

static void enter_edit_mode(void)
{
    if (s_edit_mode) return;
    s_edit_mode = true;
    ESP_LOGI(TAG, "Edit mode: ON");
    hc_pill_hide();
    hc_build();
}

static void exit_edit_mode(void)
{
    if (!s_edit_mode) return;
    s_edit_mode = false;
    ESP_LOGI(TAG, "Edit mode: OFF");
    hc_build();
}

/* Dairenin sol-üst yayına kırmızı (-) rozeti. CLICKABLE → tap'lenince
 * minus_badge_click_cb tetiklenir, uninstall confirm modal açılır.
 * Konum ölçekle birlikte hc_apply_scale() içinde güncellenir. */
static lv_obj_t* add_minus_badge(lv_obj_t* circle, app_id_t id)
{
    if (!circle) return NULL;
    lv_obj_t* badge = lv_obj_create(circle);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, HC_BADGE_D, HC_BADGE_D);
    lv_obj_set_style_bg_color(badge, lv_color_make(0xFF, 0x3B, 0x30), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(badge, 2, 0);
    lv_obj_set_style_border_color(badge, lv_color_white(), 0);
    lv_obj_set_style_border_opa(badge, LV_OPA_COVER, 0);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    /* v1.4.64: rozetin görsel boyutu zor tıklanıyordu — Apple HIG min 44×44.
     * ext_click_area 24 → isabet alanı rozet merkezinden her yöne +24 px.
     * Tile-tap fallback (tile_click_cb) zaten devreye giriyor ama bu
     * defansif olarak doğrudan rozet tıklamasını da kolaylaştırır. */
    lv_obj_set_ext_click_area(badge, 24);
    lv_obj_set_user_data(badge, (void*)(uintptr_t)id);
    lv_obj_add_event_cb(badge, minus_badge_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(badge);   /* sembolün üzerinde kalsın */

    /* (-) işareti — beyaz ince yatay çubuk, rozet merkezinde */
    lv_obj_t* line = lv_obj_create(badge);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, HC_BADGE_D / 2, 3);
    lv_obj_center(line);
    lv_obj_set_style_bg_color(line, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(line, 1, 0);
    lv_obj_clear_flag(line, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return badge;
}

// ─── Uninstall confirm modal ─────────────────────────────────────────────────

static void uninstall_confirm_cancel_cb(lv_event_t* /*e*/)
{
    if (s_uninstall_modal) {
        lv_obj_delete_async(s_uninstall_modal);
        s_uninstall_modal = NULL;
    }
    s_uninstall_target = (app_id_t)0;
}

static void uninstall_confirm_delete_cb(lv_event_t* /*e*/)
{
    app_id_t id = s_uninstall_target;
    ESP_LOGI(TAG, "Uninstall from launcher: id=%d", (int)id);
    store_set_installed(id, false);

    if (s_uninstall_modal) {
        lv_obj_delete_async(s_uninstall_modal);
        s_uninstall_modal = NULL;
    }
    s_uninstall_target = (app_id_t)0;

    /* Peteği yeniden kur — silinen ikon kalkacak, kafes yeniden dengelenecek.
     * Edit mode'dan çıkmıyoruz; kullanıcı diğer downloadable'ları da silebilir. */
    hc_build();
}

static void show_launcher_uninstall_confirm(app_id_t id)
{
    if (s_uninstall_modal) return;   /* zaten açık */
    s_uninstall_target = id;

    /* Catalog'tan app adı al */
    const char* name = "this app";
    const size_t cn = store_catalog_count();
    for (size_t i = 0; i < cn; i++) {
        const store_app_entry_t* e = store_catalog_at(i);
        if (e && e->id == id) { name = e->name; break; }
    }

    lv_obj_t* overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, HC_SCR_W, HC_SCR_H);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    s_uninstall_modal = overlay;

    lv_obj_t* card = lv_obj_create(overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, L_MODAL_W, L_MODAL_H);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_make(0x1C, 0x1C, 0x1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_clear_flag(card, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    /* Başlık — "Delete <name>?" */
    char title_buf[80];
    snprintf(title_buf, sizeof(title_buf), "Delete \"%s\"?", name);
    lv_obj_t* title = lv_label_create(card);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, L_MODAL_TXT_W);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(title, title_buf);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    /* Açıklama */
    lv_obj_t* desc = lv_label_create(card);
    lv_obj_set_style_text_color(desc, lv_color_make(0xC7, 0xC7, 0xCC), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(desc, 4, 0);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, L_MODAL_TXT_W);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(desc, "You can reinstall it later from the Store.");
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, L_MODAL_DESC_Y);
    lv_obj_clear_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    /* Cancel — sol alt, mavi text link */
    lv_obj_t* cancel = lv_label_create(card);
    lv_obj_set_style_text_color(cancel, lv_color_make(0x0A, 0x84, 0xFF), 0);
    lv_obj_set_style_text_font(cancel, &lv_font_montserrat_22, 0);
    lv_label_set_text(cancel, "Cancel");
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 24, -10);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(cancel, 28);
    lv_obj_add_event_cb(cancel, uninstall_confirm_cancel_cb, LV_EVENT_CLICKED, NULL);

    /* Delete — sağ alt, kırmızı text link */
    lv_obj_t* del = lv_label_create(card);
    lv_obj_set_style_text_color(del, lv_color_make(0xFF, 0x3B, 0x30), 0);
    lv_obj_set_style_text_font(del, &lv_font_montserrat_22, 0);
    lv_label_set_text(del, "Delete");
    lv_obj_align(del, LV_ALIGN_BOTTOM_RIGHT, -24, -10);
    lv_obj_add_flag(del, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(del, 28);
    lv_obj_add_event_cb(del, uninstall_confirm_delete_cb, LV_EVENT_CLICKED, NULL);
}

// ─── Saat güncelleme ──────────────────────────────────────────────────────────

static void clock_timer_cb(lv_timer_t* t)
{
    (void)t;
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    char d[2] = { '0', '\0' };
    if (s_t_h0) { d[0] = '0' + (ti.tm_hour / 10) % 10; lv_label_set_text(s_t_h0, d); }
    if (s_t_h1) { d[0] = '0' + (ti.tm_hour % 10);      lv_label_set_text(s_t_h1, d); }
    if (s_t_m0) { d[0] = '0' + (ti.tm_min / 10) % 10;  lv_label_set_text(s_t_m0, d); }
    if (s_t_m1) { d[0] = '0' + (ti.tm_min % 10);       lv_label_set_text(s_t_m1, d); }

    /* WiFi simgesi: bağlı değilken hiç gösterme. wifi_service_is_connected()
     * sadece bir EventGroup biti okur — saniyede bir polling maliyetsiz. */
    if (s_lbl_wifi) {
        bool connected = wifi_service_is_connected();
        if (connected) lv_obj_clear_flag(s_lbl_wifi, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag  (s_lbl_wifi, LV_OBJ_FLAG_HIDDEN);
    }
}

// ─── Balık gözü ölçekleme ────────────────────────────────────────────────────

/* Çapa göre sembol fontu. Glyph yüksekliği çapın ~%45'i olsun diye seçildi.
 * Sürekli ölçekleme yerine kova kullanmak, LVGL'de font değişiminin pahalı
 * olmasından değil, her scroll tick'inde gereksiz invalidate etmemek için. */
static const lv_font_t* hc_font_for(int d, int8_t* bucket_out)
{
    int8_t b;
    const lv_font_t* f;
#ifdef BOARD_VARIANT_LCD128
    /* Session 43: 240'ta daireler 26..62 px arasında; AMOLED eşikleri (36'nın
     * altı → 12 punto) kenar ikonlarını boş bırakıyor, 52→56 px geçişinde
     * 22→28 sıçratıyordu. Her kademe ≈ çapın %45-50'si olacak şekilde 7 adım. */
    if      (d >= 58) { b = 6; f = &lv_font_montserrat_28; }
    else if (d >= 50) { b = 5; f = &lv_font_montserrat_24; }
    else if (d >= 44) { b = 4; f = &lv_font_montserrat_22; }
    else if (d >= 38) { b = 3; f = &lv_font_montserrat_20; }
    else if (d >= 32) { b = 2; f = &lv_font_montserrat_16; }
    else if (d >= 28) { b = 1; f = &lv_font_montserrat_14; }
    else              { b = 0; f = &lv_font_montserrat_12; }
    if (bucket_out) *bucket_out = b;
    return f;
#endif
    if      (d >= 88) { b = 6; f = &lv_font_montserrat_44; }
    else if (d >= 76) { b = 5; f = &lv_font_montserrat_38; }
    else if (d >= 64) { b = 4; f = &lv_font_montserrat_32; }
    else if (d >= 54) { b = 3; f = &lv_font_montserrat_28; }
    else if (d >= 44) { b = 2; f = &lv_font_montserrat_22; }
    else if (d >= 36) { b = 1; f = &lv_font_montserrat_16; }
    else              { b = 0; f = &lv_font_montserrat_12; }
    if (bucket_out) *bucket_out = b;
    return f;
}

static void hc_apply_scale(hc_icon_t* ic, int d, int32_t s256)
{
    if (d == ic->cur_d) return;
    ic->cur_d = (int16_t)d;

    lv_obj_set_size(ic->obj, d, d);
    lv_obj_set_pos(ic->obj, ic->cx - d / 2, ic->cy - d / 2);

    if (ic->sym) {
        int8_t b = 0;
        const lv_font_t* f = hc_font_for(d, &b);
        if (b != ic->cur_font) {
            ic->cur_font = b;
            lv_obj_set_style_text_font(ic->sym, f, 0);
        }
    }
    if (ic->img) {
        /* Raster hava ikonu: tam ölçekteki değer × balık gözü katsayısı */
        int32_t sc = (int32_t)HC_WEATHER_SCALE * s256 / 256;
        if (sc < 40) sc = 40;
        lv_image_set_scale(ic->img, (uint32_t)sc);
    }
    if (ic->badge) {
        /* Rozeti dairenin sol-üst yayına (225°) otur: merkezden 0.35·d */
        const int off = (d * 35) / 100;
        lv_obj_align(ic->badge, LV_ALIGN_CENTER, -off, -off);
    }
}

/* Görüş penceresinin merkezine olan uzaklığa göre tüm ikonları ölçekler.
 * Karekök yok: s = R² / (R² + k·d²)  (8.8 sabit nokta). */
static void hc_relayout(void)
{
    if (!s_scroll) return;
    const int32_t sy  = lv_obj_get_scroll_y(s_scroll);
    const int     ccx = HC_SCR_W / 2;
    const int     ccy = HC_FOCUS_Y;
    const int32_t r2  = (int32_t)HC_FISHEYE_R * HC_FISHEYE_R;

    for (int i = 0; i < s_icon_count; i++) {
        hc_icon_t* ic = &s_icons[i];
        if (!ic->obj) continue;
        const int32_t dx = ic->cx - ccx;
        const int32_t dy = (ic->cy - sy) - ccy;
        const int32_t d2 = dx * dx + dy * dy;

        int32_t s = (int32_t)(((int64_t)256 * r2) /
                              (r2 + ((int64_t)d2 * HC_FISHEYE_K) / 256));
        if (s > 256)      s = 256;
        if (s < HC_MIN_S) s = HC_MIN_S;

        int d = (HC_ICON_D * s) / 256;
        d &= ~1;                       /* çift sayı → merkez tam ortada kalır */
        hc_apply_scale(ic, d, s);
    }
}

static void hc_scroll_cb(lv_event_t* e)
{
    (void)e;
    ui_service_note_scroll();   /* Session 17 kalıbı: kaydırma-tıklama ayrımı */
    hc_pill_hide();
    hc_relayout();
}

// ─── Petek ikon üretimi ──────────────────────────────────────────────────────

/* Ortak daire gövdesi. Konum/boyut hc_relayout() tarafından verilir; burada
 * yalnız tam ölçekli hâli kurulur. */
static hc_icon_t* hc_new_icon(lv_color_t c_top, lv_color_t c_bot,
                              const char* name, app_id_t id, bool removable)
{
    if (s_icon_count >= HC_MAX_ICONS || !s_canvas) return NULL;
    hc_icon_t* ic = &s_icons[s_icon_count];
    *ic = hc_icon_t{};
    ic->name = name;
    ic->id   = id;

    lv_obj_t* c = lv_obj_create(s_canvas);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, HC_ICON_D, HC_ICON_D);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_style_bg_color(c, c_top, 0);
    lv_obj_set_style_bg_grad_color(c, c_bot, 0);
    lv_obj_set_style_bg_grad_dir(c, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(c, tile_click_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)id);
    lv_obj_add_event_cb(c, tile_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(c, tile_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(c, tile_release_cb, LV_EVENT_PRESS_LOST, NULL);
    /* Apple Watch tap tactility: basınca kısa opaklık dip'i, bırakınca spring. */
    wear_press_feedback_attach(c);

    ic->obj      = c;
    ic->cur_d    = HC_ICON_D;
    ic->cur_font = -1;   /* ilk hc_relayout() fontu mutlaka yazsın */

    if (removable && s_edit_mode) ic->badge = add_minus_badge(c, id);

    s_icon_count++;
    return ic;
}

static void hc_add_glyph_icon(lv_color_t c_top, lv_color_t c_bot,
                              const char* symbol, const char* name,
                              app_id_t id, bool removable)
{
    hc_icon_t* ic = hc_new_icon(c_top, c_bot, name, id, removable);
    if (!ic) return;

    lv_obj_t* sym = lv_label_create(ic->obj);
    lv_obj_set_style_text_color(sym, lv_color_white(), 0);
    lv_obj_align(sym, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(sym, symbol);
    lv_obj_add_flag(sym, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(sym, LV_OBJ_FLAG_CLICKABLE);
    ic->sym = sym;
    if (ic->badge) lv_obj_move_foreground(ic->badge);
}

/* Hava Durumu — font glyph yerine gerçek raster ikon (partly_cloud_icon,
 * 40×40 ARGB8888). LVGL primitive'leriyle çizilen vektör versiyon küçük
 * boyutlarda kötü görünüyordu. */
static void hc_add_weather_icon(app_id_t id)
{
    hc_icon_t* ic = hc_new_icon(lv_color_make(0x4A, 0x9D, 0xF5),
                                lv_color_make(0x1F, 0x5B, 0xB8),
                                "Weather", id, false);
    if (!ic) return;

    lv_obj_t* img = lv_image_create(ic->obj);
    lv_image_set_src(img, &partly_cloud_icon);
    lv_image_set_pivot(img, 20, 20);            /* asset merkezinde pivot */
    lv_image_set_scale(img, HC_WEATHER_SCALE);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    /* Tıklamalar parent (daire) handler'ına ulaşsın diye bubble + non-clickable */
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    ic->img = img;
    if (ic->badge) lv_obj_move_foreground(ic->badge);
}

// ─── Petek kafesi ────────────────────────────────────────────────────────────

/* Satır kapasiteleri 3/2/3/2… ve her satır kendi içinde ortalandığı için
 * komşu satırlar doğal olarak yarım adım kayar. Son satır eksik kalırsa
 * (ör. 7 uygulama → 3,2,2) beklenen tekil/çift sayı bozulur ve iki satır
 * üst üste hizalanırdı; bu durumda satırı yarım adım ötelenir → petek korunur. */
static void hc_place_icons(void)
{
    const int n = s_icon_count;
    if (n <= 0) return;

    /* Satır dağılımı */
    int rows = 0, counts[HC_MAX_ICONS];
    for (int placed = 0; placed < n; rows++) {
        int cap = HC_ROW_CAP(rows);
        int k   = (n - placed < cap) ? (n - placed) : cap;
        counts[rows] = k;
        placed += k;
    }

    /* Session 23b: içerik ARTIK dikey ortalanmıyor, üste yaslanıyor.
     * 5 uygulamada petek ekranın ortasından başlıyordu; kullanıcı ileride
     * uygulama ekleyeceği için büyüme yönü (aşağı) boş kalmalı. */
    const int content_h = (rows - 1) * HC_PITCH_Y + HC_ICON_D;
    const int needed    = content_h + 2 * HC_PAD_TOP;
    const int canvas_h  = (needed > HC_VIEW_H) ? needed : HC_VIEW_H;
    const int y_base    = HC_PAD_TOP;

    lv_obj_set_size(s_canvas, HC_SCR_W, canvas_h);
    lv_obj_set_pos(s_canvas, 0, 0);

    int idx = 0;
    for (int r = 0; r < rows; r++) {
        const int k = counts[r];
        /* Beklenen sayı tek mi çift mi (3/2/3/2…)? Sapma varsa yarım adım öteler. */
        const int half = ((k & 1) != (HC_ROW_CAP(r) & 1)) ? (HC_PITCH_X / 2) : 0;
        const int cy   = y_base + HC_ICON_D / 2 + r * HC_PITCH_Y;

        for (int i = 0; i < k; i++, idx++) {
            /* (i - (k-1)/2) yarım adım çözünürlükte: 2·i - (k-1) */
            const int x = ((2 * i - (k - 1)) * HC_PITCH_X) / 2 + half;
            s_icons[idx].cx = (int16_t)(HC_SCR_W / 2 + x);
            s_icons[idx].cy = (int16_t)cy;
            s_icons[idx].cur_d = -1;   /* hc_relayout() mutlaka uygulasın */
        }
    }
}

static void hc_build(void)
{
    if (!s_canvas) return;

    const int32_t keep_sy = s_scroll ? lv_obj_get_scroll_y(s_scroll) : 0;

    hc_pill_hide();
    lv_obj_clean(s_canvas);
    s_icon_count = 0;

    /* ─── Sistem ikonları (always-on, Apple Watch home grid sırasıyla) ───── */
    hc_add_glyph_icon(lv_color_make(0x70, 0x70, 0x70), lv_color_make(0x40, 0x40, 0x40),
                      LV_SYMBOL_SETTINGS, "Settings", APP_ID_SETTINGS, false);

    hc_add_glyph_icon(lv_color_make(0x4F, 0xE0, 0x9D), lv_color_make(0x1C, 0x74, 0xE8),
                      LV_SYMBOL_IMAGE, "Gallery", APP_ID_GALLERY, false);

    hc_add_weather_icon(APP_ID_WEATHER);

    /* Alarm — turuncu/kırmızı çan (Apple Watch Alarms ikonuna referansla) */
    hc_add_glyph_icon(lv_color_make(0xFF, 0x9F, 0x0A), lv_color_make(0xCC, 0x4A, 0x12),
                      LV_SYMBOL_BELL, "Alarms", APP_ID_ALARM, false);

    /* Store — App Store tarzı mavi + LV_SYMBOL_DOWNLOAD (v1.2.4) */
    hc_add_glyph_icon(lv_color_make(0x4F, 0xC3, 0xF7), lv_color_make(0x02, 0x57, 0xE1),
                      LV_SYMBOL_DOWNLOAD, "Store", APP_ID_STORE, false);

    /* ─── Downloadable installed app'ler ────────────────────────────────── */
    const size_t count = store_catalog_count();
    for (size_t i = 0; i < count; i++) {
        const store_app_entry_t* e = store_catalog_at(i);
        if (!e || e->is_system) continue;
        if (!store_is_installed(e->id)) continue;

        /* Gradyan: ana renk + %70 koyu varyantı (basit shade) */
        uint8_t r2 = (uint8_t)(e->cr * 0.7f);
        uint8_t g2 = (uint8_t)(e->cg * 0.7f);
        uint8_t b2 = (uint8_t)(e->cb * 0.7f);
        hc_add_glyph_icon(lv_color_make(e->cr, e->cg, e->cb),
                          lv_color_make(r2, g2, b2),
                          e->icon_symbol, e->name, e->id, /*removable=*/true);
    }

#if LAUNCHER_DEMO_ICONS > 0
    {
        static const char* demo_sym[] = { LV_SYMBOL_AUDIO, LV_SYMBOL_CALL, LV_SYMBOL_CHARGE,
                                          LV_SYMBOL_KEYBOARD, LV_SYMBOL_SD_CARD, LV_SYMBOL_USB };
        static const char* demo_name[] = { "Demo 1", "Demo 2", "Demo 3", "Demo 4", "Demo 5", "Demo 6" };
        static const uint8_t demo_rgb[][3] = { {0xAF,0x52,0xDE}, {0x30,0xD1,0x58}, {0xFF,0x2D,0x55},
                                               {0x5A,0xC8,0xFA}, {0xFF,0xD6,0x0A}, {0x8E,0x8E,0x93} };
        for (int i = 0; i < LAUNCHER_DEMO_ICONS && i < 6; i++) {
            lv_color_t c = lv_color_make(demo_rgb[i][0], demo_rgb[i][1], demo_rgb[i][2]);
            lv_color_t d = lv_color_make(demo_rgb[i][0] * 7 / 10, demo_rgb[i][1] * 7 / 10, demo_rgb[i][2] * 7 / 10);
            hc_add_glyph_icon(c, d, demo_sym[i], demo_name[i], APP_ID_COUNT, false);
        }
    }
#endif

    hc_place_icons();

    /* Kaydırma konumunu koru (edit mode / uninstall sonrası zıplamasın) */
    if (s_scroll) {
        lv_obj_update_layout(s_scroll);
        int32_t max_sy = lv_obj_get_scroll_bottom(s_scroll) + lv_obj_get_scroll_y(s_scroll);
        int32_t sy = keep_sy;
        if (sy > max_sy) sy = max_sy;
        if (sy < 0)      sy = 0;
        lv_obj_scroll_to_y(s_scroll, sy, LV_ANIM_OFF);
    }
    hc_relayout();

    /* Yeni ikonlara da gesture bubble ver — screen'e ulaşan geri jesti için. */
    if (s_canvas) app_manager_enable_gestures(lv_obj_get_screen(s_canvas));
}

// ─── App lifecycle ─────────────────────────────────────────────────────────────

static void on_start(lv_obj_t* screen)
{
    ESP_LOGI(TAG, "Home screen starting (honeycomb)");
    ui_service_lock_swipe(false);

    /* Session 23b: eski 0x040418 hafif mavi tonluydu ve altındaki "glow"
     * diskiyle birlikte ikonların arkasında koyu mavi bir alan gibi
     * okunuyordu (kullanıcı fotoğrafı 2026-08-31). AMOLED'de saf siyah =
     * piksel kapalı → ikonlar cihazın kendi zemininde duruyor gibi. */
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* AMOLED'in üst-sağ köşesi yuvarlak (r≈120 px). y=26 → sağ inset
     * büyük ofsetler (icons -50, -82) ile yuvarlak köşe maskesi
     * dışında kalıyor.
     *
     * Saat: 4 ayrı digit + sabit ":" label. Her digit kendi
     * 10 px sabit container'ında center-text → glyph genişlik farkı
     * artık konumu etkilemiyor. */
    auto mk_digit = [&](lv_obj_t** out, int x, int w) {
        lv_obj_t* l = lv_label_create(screen);
        lv_obj_remove_style_all(l);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_set_width(l, w);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, x, 26);
        lv_label_set_text(l, "0");
        *out = l;
    };
#ifdef BOARD_VARIANT_LCD128
    /* 240 yuvarlak: 46 px'lik saat grubu üst-merkezde (x=97..143, y=26 —
     * dairenin o yükseklikteki görünür şeridi x=45..195). */
    mk_digit(&s_t_h0,  97, 10);
    mk_digit(&s_t_h1, 107, 10);
#else
    mk_digit(&s_t_h0, HC_SCR_W / 2 - 23, 10);
    mk_digit(&s_t_h1, HC_SCR_W / 2 - 13, 10);
#endif
    /* ":" sabit label */
    lv_obj_t* colon = lv_label_create(screen);
    lv_obj_remove_style_all(colon);
    lv_obj_set_style_text_color(colon, lv_color_white(), 0);
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_14, 0);
    lv_obj_set_width(colon, 6);
    lv_obj_set_style_text_align(colon, LV_TEXT_ALIGN_CENTER, 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_pos(colon, 117, 26);
    lv_label_set_text(colon, ":");
    mk_digit(&s_t_m0, 123, 10);
    mk_digit(&s_t_m1, 133, 10);
#else
    lv_obj_set_pos(colon, HC_SCR_W / 2 - 3, 26);
    lv_label_set_text(colon, ":");
    mk_digit(&s_t_m0, HC_SCR_W / 2 + 3,  10);
    mk_digit(&s_t_m1, HC_SCR_W / 2 + 13, 10);
#endif

    s_lbl_wifi = lv_label_create(screen);
    lv_obj_set_style_text_color(s_lbl_wifi, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_set_style_text_font(s_lbl_wifi, &lv_font_montserrat_14, 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_align(s_lbl_wifi, LV_ALIGN_TOP_MID, -52, 26);
#else
    lv_obj_align(s_lbl_wifi, LV_ALIGN_TOP_RIGHT, -82, 26);
#endif
    lv_label_set_text(s_lbl_wifi, LV_SYMBOL_WIFI);
    /* Başlangıç durumu — clock timer 1 sn sonra zaten teyit eder */
    if (!wifi_service_is_connected()) {
        lv_obj_add_flag(s_lbl_wifi, LV_OBJ_FLAG_HIDDEN);
    }

    s_lbl_bat = lv_label_create(screen);
    lv_obj_set_style_text_color(s_lbl_bat, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_set_style_text_font(s_lbl_bat, &lv_font_montserrat_14, 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_align(s_lbl_bat, LV_ALIGN_TOP_MID, 52, 26);
#else
    lv_obj_align(s_lbl_bat, LV_ALIGN_TOP_RIGHT, -50, 26);
#endif
    lv_label_set_text(s_lbl_bat, LV_SYMBOL_BATTERY_FULL);

    /* Anlık batarya durumunu uygula — power_service ilk EVENT_BATTERY_LEVEL'i
     * launcher henüz current değilken yayınladıysa kaçırmayalım. */
    apply_battery_visual();

    /* ─── Petek: dikey kaydırılan görüş penceresi + sabit tuval ─────────────
     * Kaydırma yönü YALNIZ dikey; yatay swipe global geri jesti olarak kalır.
     * Tuval sabit boyutlu olduğu için ikonlar küçülüp büyürken kaydırma
     * sınırları oynamaz (aksi hâlde balık gözü kaydırmayı titretirdi). */
    s_scroll = lv_obj_create(screen);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, HC_SCR_W, HC_VIEW_H);
    lv_obj_set_pos(s_scroll, 0, HC_TOP);
    lv_obj_set_style_pad_all(s_scroll, 0, 0);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_scroll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_scroll, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_scroll, hc_scroll_cb, LV_EVENT_SCROLL, NULL);

    s_canvas = lv_obj_create(s_scroll);
    lv_obj_remove_style_all(s_canvas);
    lv_obj_set_size(s_canvas, HC_SCR_W, HC_VIEW_H);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_clear_flag(s_canvas, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                                 LV_OBJ_FLAG_SCROLLABLE));

    /* İsim balonu — screen'in child'ı (scroller kırpmasın), başta gizli */
    s_pill = lv_obj_create(screen);
    lv_obj_remove_style_all(s_pill);
    lv_obj_set_style_bg_color(s_pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_pill, 12, 0);
    lv_obj_set_style_pad_ver(s_pill, 5, 0);
    lv_obj_set_size(s_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(s_pill, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                               LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);

    s_pill_lbl = lv_label_create(s_pill);
    lv_obj_set_style_text_color(s_pill_lbl, lv_color_make(0x0A, 0x0A, 0x14), 0);
    lv_obj_set_style_text_font(s_pill_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_pill_lbl, "");
    lv_obj_center(s_pill_lbl);

    hc_build();

    app_manager_enable_gestures(screen);
    lv_obj_add_event_cb(screen, gesture_cb, LV_EVENT_GESTURE, NULL);

    s_clk_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);
}

static void on_resume(void)
{
    if (s_clk_timer) lv_timer_resume(s_clk_timer);

    /* v1.2.6 (2026-05-22): Store'dan install/uninstall sonrası launcher'a
     * dönüldüğünde ikon listesi güncel olsun. Maliyet ~birkaç ms. */
    hc_build();
}

static void on_pause(void)
{
    if (s_clk_timer) lv_timer_pause(s_clk_timer);
    hc_pill_hide();
}

static void on_stop(void)
{
    if (s_clk_timer) { lv_timer_delete(s_clk_timer); s_clk_timer = NULL; }
    s_t_h0 = s_t_h1 = s_t_m0 = s_t_m1 = s_lbl_bat = s_lbl_wifi = NULL;
    /* Screen ile beraber silindiler — pointer'ları NULL'la */
    s_scroll = s_canvas = s_pill = s_pill_lbl = NULL;
    s_icon_count = 0;
    /* v1.2.6: edit mode state — screen kapanırsa edit mode da reset */
    s_edit_mode = false;
    s_uninstall_modal = NULL;
    s_uninstall_target = (app_id_t)0;
    s_press_start_us = 0;
}

static bool on_event(const os_event_t* event)
{
    if (event->type == EVENT_BATTERY_LEVEL ||
        event->type == EVENT_CHARGING_STATE) {
        // Event içeriğini kullanmıyoruz — taze durumu doğrudan PMIC'ten oku.
        apply_battery_visual();
        return true;
    }
    return false;
}

// ─── Interface export ─────────────────────────────────────────────────────────

static const app_interface_t s_iface = {
    .on_start  = on_start,
    .on_resume = on_resume,
    .on_pause  = on_pause,
    .on_stop   = on_stop,
    .on_event  = on_event,
    .name      = "Home",
};

const app_interface_t* launcher_app_get_interface(void) { return &s_iface; }
