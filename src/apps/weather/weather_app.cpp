/**
 * @file weather_app.cpp
 * @brief Apple Watch tarzı detaylı hava durumu ekranı + çoklu şehir menüsü.
 *
 * Düzen (410×502):
 *   y=  26..  60  : Üst bar — sol liste ikon (clickable), ortada saat, sağda hava ikonu
 *   y=  86.. 118  : Şehir satırı (GPS + isim, ortalanmış flex)
 *   y= 130.. 220  : Büyük sıcaklık (Nunito ExtraBold 70) + ° (ortalanmış flex)
 *   y= 232.. 264  : Açıklama (Montserrat 22)
 *   y= 268.. 286  : "Y:16°  D:7°"  (Montserrat 16)
 *   y= 320.. 430  : 3 halka widget (UV / Rüzgâr / Nem) — başlıksız
 *
 * Sol üstteki turuncu liste ikonuna dokunma (LV_EVENT_CLICKED) → şehir liste
 * overlay'i açılır. Overlay primary olmayan şehirleri Apple Watch tarzı yuvarlak
 * mavi kartlar olarak listeler. Karta dokununca o şehir primary olur, overlay
 * kapanır, ana ekran yeni şehrin verisiyle dolar.
 *
 * Veri kaynağı: weather_service (snapshot + EVENT_WEATHER_UPDATE).
 * Sola kaydırma → app_manager_back() (overlay açıkken: overlay kapanır).
 */

#include "weather_app.h"
#include "../../core/app_manager.h"
#include "../../core/event_bus.h"
#include "../../core/os_config.h"
#include "../../services/weather_service.h"
#include "../../assets/weather_icon.h"
#include "wear_motion.h"
#include <lvgl.h>
#include <esp_log.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char* TAG = "WeatherApp";

/* Nunito ExtraBold 70 KALDIRILDI — fontun doğal yuvarlak/oval rakam tasarımı
 * extra-bold + 70 px'te eğik görünüm yaratıyordu. Montserrat 48 (geometric,
 * upright) ile değiştirildi. Bkz. build_dynamic_text_labels yorumu. */

// ─── Widget handles ───────────────────────────────────────────────────────────

static lv_obj_t*   s_screen        = NULL;   // ana ekran (overlay parent'ı)
static lv_obj_t*   s_listicon      = NULL;   // sol üst turuncu liste ikonu (clickable)
static lv_obj_t*   s_lbl_time      = NULL;
static lv_obj_t*   s_icon_box      = NULL;   // sağ üst hava ikonu
static lv_obj_t*   s_lbl_city      = NULL;
static lv_obj_t*   s_lbl_temp      = NULL;   // büyük sıcaklık (rakam)
static lv_obj_t*   s_lbl_temp_deg  = NULL;   // küçük "°" üst-sağ
static lv_obj_t*   s_lbl_desc      = NULL;
static lv_obj_t*   s_lbl_hilo      = NULL;

// Halkalar
static lv_obj_t*   s_arc_uv        = NULL;
static lv_obj_t*   s_lbl_uv        = NULL;

static lv_obj_t*   s_wind_arrow    = NULL;
static lv_obj_t*   s_lbl_wind      = NULL;

static lv_obj_t*   s_arc_hum       = NULL;
static lv_obj_t*   s_lbl_hum       = NULL;

static lv_timer_t* s_clk_timer     = NULL;

// Overlay (şehir liste menüsü)
static lv_obj_t*   s_overlay       = NULL;   // tam-ekran kapsayıcı (NULL = kapalı)

/* Per-card label pointers — overlay açıkken periyodik tazeleme için.
 * Fetch task arka planda şehirleri sırayla çekerken (~1-2 sn/şehir), kartlar
 * "--°" göstermesin diye 1 Hz tick ile re-read yapıyoruz; data geldikçe
 * kartlar canlı dolduruyor. */
typedef struct {
    int       idx;
    lv_obj_t* card;         // parent card — full invalidate için
    lv_obj_t* lbl_temp;     // büyük sıcaklık rakamı
    lv_obj_t* lbl_time;     // küçük updated_at saati
    char      last_temp[8]; // son set edilen text — değişmediyse invalidate gereksiz
    char      last_time[20];
} overlay_card_t;
static overlay_card_t s_overlay_cards[WEATHER_CITY_COUNT];
static int            s_overlay_card_count = 0;
static lv_timer_t*    s_overlay_timer      = NULL;

// ─── Yardımcı: rounded float → int (yakın) ─────────────────────────────────────

static int rnd_int(float f)
{
    if (f >= 0) return (int)(f + 0.5f);
    return -(int)(-f + 0.5f);
}

// ─── UI yenileme (snapshot'tan) ────────────────────────────────────────────────

/* Önceki çağrıdaki primary şehir adı — değişiklik tespiti için. Şehir
 * değişmediyse (sadece temp tick güncellemesi) full-screen invalidate
 * gereksiz. Değiştiyse flex layout'taki auto-resize label'lar (s_lbl_city,
 * s_lbl_temp, s_lbl_temp_deg) eski text pikselini bırakıyor → residue. */
static char s_last_city_text[32] = "";

static void listicon_click_cb(lv_event_t* e);   /* Session 41: şehir adı tıklaması için ileri bildirim */

static void update_ui_from_snapshot(void)
{
    weather_data_t d;
    bool ok = weather_service_get(&d);

    char buf[48];
    bool city_changed = false;

    // Şehir
    if (s_lbl_city) {
        const char* new_city = (ok && d.city[0]) ? d.city : "--";
        if (strcmp(new_city, s_last_city_text) != 0) {
            city_changed = true;
            strncpy(s_last_city_text, new_city, sizeof(s_last_city_text) - 1);
            s_last_city_text[sizeof(s_last_city_text) - 1] = '\0';
        }
        lv_label_set_text(s_lbl_city, new_city);
    }

    // Sıcaklık
    if (s_lbl_temp) {
        if (ok) {
#ifdef BOARD_VARIANT_LCD128
            snprintf(buf, sizeof(buf), "%d°", rnd_int(d.temperature));   /* Session 41: tek etiket */
#else
            snprintf(buf, sizeof(buf), "%d", rnd_int(d.temperature));
#endif
            lv_label_set_text(s_lbl_temp, buf);
        } else {
#ifdef BOARD_VARIANT_LCD128
            lv_label_set_text(s_lbl_temp, "--°");
#else
            lv_label_set_text(s_lbl_temp, "--");
#endif
        }
        /* DİNAMİK CENTERING TAMAMEN KALDIRILDI.
         *
         * Kullanıcı raporu (2026-05-05): "ilk hava durumu uygulamasına girince
         * her şey düzgün gösteriyor. Listeden şehir seçince derece, rakamlar
         * kaymış görünüyor. Geri gelip tekrar weather'a girince yine düzgün."
         *
         * Sebep: update_ui şehir değişimi sırasında lv_text_get_size çağırıp
         * label pozisyonunu yeniden hesaplıyordu. Aynı font için bile farklı
         * çağrılarda farklı text_w dönebiliyordu (LVGL font cache state) →
         * label birkaç px kayıyordu → "kaymış" görüntü. Fresh app entry'de
         * bu mantık çalışmıyordu (cached city aynı text → recompute yok),
         * ama city değişiminde recompute tetiklenince pozisyon değişiyordu.
         *
         * Çözüm: pozisyonlar on_start'ta sabit (s_lbl_temp x=38, deg x=238).
         * Hiç bir update'te değişmez. Text RIGHT-aligned bbox sağ kenarına
         * yapışıyor; "10" da "14" da "100" da bbox sağında render → ° hep aynı
         * yerde sıkışık. 2-haneli sıcaklıklar için ekran ortasında, 1/3-haneli
         * için ~5-15 px sapma var ama HİÇBİR zaman shift olmuyor. */
    }

    // Açıklama
    if (s_lbl_desc) {
        if (ok) lv_label_set_text(s_lbl_desc, weather_desc_from_wmo(d.wmo_code, d.is_night));
        else    lv_label_set_text(s_lbl_desc, "Loading...");
    }

    // High / Low
    if (s_lbl_hilo) {
        if (ok) {
            snprintf(buf, sizeof(buf), "H:%d°  L:%d°",
                     rnd_int(d.temp_max), rnd_int(d.temp_min));
            lv_label_set_text(s_lbl_hilo, buf);
        } else {
            lv_label_set_text(s_lbl_hilo, "H:--°  L:--°");
        }
    }

    // Sağ üst ikon
    if (s_icon_box) {
        weather_condition_t cond = ok ? d.condition : WEATHER_UNKNOWN;
        weather_icon_render(s_icon_box, cond);
    }

    // UV (0..11)
    if (s_lbl_uv) {   /* Session 41: LCD128'de halka yok, yalnız değer */
        int uv = ok ? rnd_int(d.uv_index) : 0;
        if (uv < 0) uv = 0;
        if (uv > 11) uv = 11;
        if (s_arc_uv) lv_arc_set_value(s_arc_uv, uv);
        snprintf(buf, sizeof(buf), "%d", uv);
        lv_label_set_text(s_lbl_uv, buf);
    }

    // Rüzgâr — ok rüzgârın geldiği yöne döner.
    if (s_lbl_wind) {
        int dir = ok ? d.wind_dir : 0;
        if (dir < 0) dir = 0;
        if (dir >= 360) dir %= 360;
        if (s_wind_arrow)
            lv_obj_set_style_transform_rotation(s_wind_arrow,
                                                ((dir + 180) % 360) * 10, 0);
        int kmh = ok ? rnd_int(d.wind_speed) : 0;
        snprintf(buf, sizeof(buf), "%d", kmh);
        lv_label_set_text(s_lbl_wind, buf);
    }

    // Nem (0..100)
    if (s_lbl_hum) {
        int h = ok ? d.humidity : 0;
        if (h < 0) h = 0;
        if (h > 100) h = 100;
        if (s_arc_hum) lv_arc_set_value(s_arc_hum, h);
        snprintf(buf, sizeof(buf), "%d", h);
        lv_label_set_text(s_lbl_hum, buf);
    }

    /* NOT: Screen invalidate (immediate + deferred 50ms) kaldırıldı.
     *
     * Tüm dynamic label'ların kendi opaque bg'si var (lbl_bg) → her
     * lv_label_set_text otomatik label-bbox invalidate ediyor, LVGL
     * render edince label kendi bg'siyle bbox'ı temizleyip yeni text'i
     * çiziyor. Residue layer-bazında zaten çözülüyor.
     *
     * Önceden burada deferred 50 ms full-screen invalidate vardı; partial-
     * flush ile chunk'lar sırayla render olunca kullanıcı bu redraw
     * flash'ını "yazılar sola gidip geliyor" olarak algılıyordu. Sadece
     * label-level invalidate ile screen flash'ı yok, geçiş kararlı. */
    (void)city_changed;  /* Şehir değişiklik takibi gelecekte gerekirse diye s_last_city_text korundu. */
}

// ─── Saat tick (1 sn) ──────────────────────────────────────────────────────────

static void clock_timer_cb(lv_timer_t* /*t*/)
{
    if (!s_lbl_time) return;
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    lv_label_set_text(s_lbl_time, buf);
}

// ─── Halka widget yardımcısı ──────────────────────────────────────────────────

static lv_obj_t* make_ring(lv_obj_t* parent, int x, int y, int size,
                           int range_min, int range_max,
                           lv_color_t fg, lv_color_t bg,
                           lv_obj_t** out_value_label,
                           const char* unit_bot)
{
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, size, size);
    lv_obj_set_pos(c, x, y);
    lv_obj_clear_flag(c, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                          LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t* arc = lv_arc_create(c);
    lv_obj_set_size(arc, size, size);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, range_min, range_max);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(arc, bg, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, fg, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

    /* Value label — sabit bbox + LONG_CLIP + opaque bg. Önceden auto-sized
     * + lv_obj_align idi: değer "100" → "66" (kısa) gibi geçişlerde label
     * shrink olunca eski genişlikteki bbox'ın kenar pikselleri kalıyordu →
     * "66" yanında eski "00" izleri (kullanıcı raporu 2026-05-05 humidity
     * residue). 70×32 sabit bbox ve siyah-mavi (lbl_bg) opaque arka plan
     * her redraw'da bbox'ı temizliyor; merkez konumu sabit (TOP_MID
     * mantığında). */
    lv_obj_t* v = lv_label_create(c);
    lv_obj_remove_style_all(v);
    lv_label_set_long_mode(v, LV_LABEL_LONG_CLIP);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(v, 44, 18);
#else
    lv_obj_set_size(v, 70, 32);
#endif
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(v, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(v, &lv_font_montserrat_28, 0);
#endif
    lv_obj_set_style_bg_color(v, lv_color_make(0x03, 0x0A, 0x1F), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(v, 0, 0);
    /* Even-aligned position: ring container içinde TOP_MID mantığı. */
#ifdef BOARD_VARIANT_LCD128
    int v_x = (size - 44) / 2;
    int v_y = (size / 2) - 9;
#else
    int v_x = (size - 70) / 2;
    int v_y = (size / 2) - 22;
#endif
    if (v_x & 1) v_x--;        /* even */
    if (v_y & 1) v_y--;
    lv_obj_set_pos(v, v_x, v_y);
    lv_label_set_text(v, "0");
    if (out_value_label) *out_value_label = v;

    if (unit_bot) {
        lv_obj_t* b = lv_label_create(c);
        lv_obj_set_style_text_color(b, lv_color_make(0xAA, 0xAA, 0xAA), 0);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_12, 0);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, (size / 2) + 10);
        lv_label_set_text(b, unit_bot);
    }

    return arc;
}

// ─── Rüzgâr pusulası (özel halka) ──────────────────────────────────────────────

static void make_wind_compass(lv_obj_t* parent, int x, int y, int size)
{
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, size, size);
    lv_obj_set_pos(c, x, y);
    lv_obj_clear_flag(c, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                          LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t* ring = lv_arc_create(c);
    lv_obj_set_size(ring, size, size);
    lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 0);
    lv_arc_set_rotation(ring, 0);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_value(ring, 0);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_remove_style(ring, NULL, LV_PART_INDICATOR);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(ring, lv_color_make(0x44, 0x4C, 0x60), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 4, LV_PART_MAIN);

    int cx = size / 2;
    int cy = size / 2;

    auto add_dir = [&](const char* s, int dx, int dy) {
        lv_obj_t* l = lv_label_create(c);
        lv_obj_set_style_text_color(l, lv_color_make(0xDD, 0xDD, 0xDD), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
        lv_label_set_text(l, s);
        lv_obj_set_pos(l, cx + dx - 4, cy + dy - 6);
    };
#ifndef BOARD_VARIANT_LCD128
    /* 54px halkada yön harfleri değer/ok/birimle çakışıyor — LCD'de yok. */
    int r = size / 2 - 10;
    add_dir("N", 0,  -r);
    add_dir("E", r,   0);
    add_dir("S", 0,   r);
    add_dir("W", -r,  0);
#else
    (void)add_dir;
#endif

    s_wind_arrow = lv_label_create(c);
    lv_obj_set_style_text_color(s_wind_arrow, lv_color_make(0x4F, 0xC3, 0xF7), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_wind_arrow, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_wind_arrow, LV_SYMBOL_UP);
    lv_obj_set_pos(s_wind_arrow, cx - 4, cy - 20);
#else
    lv_obj_set_style_text_font(s_wind_arrow, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_wind_arrow, LV_SYMBOL_UP);
    lv_obj_set_pos(s_wind_arrow, cx - 5, cy - 30);
#endif
    lv_obj_set_style_transform_pivot_x(s_wind_arrow, 5, 0);
    lv_obj_set_style_transform_pivot_y(s_wind_arrow, 8, 0);

    /* Wind value label — sabit bbox + opaque bg, value rakamları arasında
     * residue olmasın (make_ring'deki ile aynı mantık). Compass içinde
     * dolayısıyla biraz daha dar (60 px). */
    s_lbl_wind = lv_label_create(c);
    lv_obj_remove_style_all(s_lbl_wind);
    lv_label_set_long_mode(s_lbl_wind, LV_LABEL_LONG_CLIP);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_lbl_wind, 40, 16);
#else
    lv_obj_set_size(s_lbl_wind, 60, 28);
#endif
    lv_obj_set_style_text_align(s_lbl_wind, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_lbl_wind, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_lbl_wind, &lv_font_montserrat_12, 0);
#else
    lv_obj_set_style_text_font(s_lbl_wind, &lv_font_montserrat_24, 0);
#endif
    lv_obj_set_style_bg_color(s_lbl_wind, lv_color_make(0x03, 0x0A, 0x1F), 0);
    lv_obj_set_style_bg_opa(s_lbl_wind, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_lbl_wind, 0, 0);
#ifdef BOARD_VARIANT_LCD128
    int w_x = (size - 40) / 2;
    int w_y = (size / 2) - 3;
#else
    int w_x = (size - 60) / 2;
    int w_y = (size / 2) - 8;
#endif
    if (w_x & 1) w_x--;
    if (w_y & 1) w_y--;
    lv_obj_set_pos(s_lbl_wind, w_x, w_y);
    lv_label_set_text(s_lbl_wind, "0");

    lv_obj_t* unit = lv_label_create(c);
    lv_obj_set_style_text_color(unit, lv_color_make(0xAA, 0xAA, 0xAA), 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_12, 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, (size / 2) + 12);
#else
    lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, (size / 2) + 22);
#endif
    lv_label_set_text(unit, "km/h");
}

/* Dynamic-text label'ları sıfırdan oluştur — fresh label state.
 *
 * Kullanıcı raporu (2026-05-05): "ilk weather app girişinde tüm rakamlar
 * düzgün, listeden şehir seçince derece kayıyor, geri gelip tekrar weather
 * açtığımda yine düzgün". Yani fresh label create = clean. Update in-place
 * = kayma. Bu fonksiyon tap sonrası label'ları yeniden oluşturup
 * fresh-entry görsel davranışını garantiliyor.
 *
 * Çağrılar: on_start (ilk init) ve city_card_click_cb (re-create). */
static lv_color_t s_lbl_bg_color;          /* on_start'ta initialized */
static void build_dynamic_text_labels(lv_obj_t* screen)
{
    lv_color_t lbl_bg = s_lbl_bg_color;

    /* Şehir adı */
    s_lbl_city = lv_label_create(screen);
    lv_obj_remove_style_all(s_lbl_city);
    lv_label_set_long_mode(s_lbl_city, LV_LABEL_LONG_CLIP);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_lbl_city, 200, 20);
    /* Session 41: liste ikonu yok — şehir adına dokununca şehir listesi açılır */
    lv_obj_add_flag(s_lbl_city, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_lbl_city, 12);
    lv_obj_add_event_cb(s_lbl_city, listicon_click_cb, LV_EVENT_CLICKED, NULL);
#else
    lv_obj_set_size(s_lbl_city, 360, 32);
#endif
    lv_obj_set_style_text_align(s_lbl_city, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_lbl_city, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_lbl_city, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(s_lbl_city, &lv_font_montserrat_22, 0);
#endif
    lv_obj_set_style_bg_color(s_lbl_city, lbl_bg, 0);
    lv_obj_set_style_bg_opa(s_lbl_city, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_lbl_city, 0, 0);
#ifdef BOARD_VARIANT_LCD128
    lv_label_set_text(s_lbl_city, "--");
    lv_obj_set_pos(s_lbl_city, 20, 46);   /* 46..66 */
#else
    lv_label_set_text(s_lbl_city, "--");
    lv_obj_set_pos(s_lbl_city, 24, 86);
#endif

    /* Sıcaklık — Montserrat 48 (Nunito ExtraBold 70 yerine).
     *
     * KÖK SEBEP TEŞHİSİ (kullanıcı videosu 2026-05-05 14:47):
     * Kullanıcının "kayık" şikayeti POZİSYON SHIFT'i değil, FONT'un eğik
     * görünmesi. Türkçe "kayık" hem "shifted" hem "italic" anlamına geliyor;
     * yanlış teşhis ettim baştan beri. Nunito-ExtraBold.ttf'in doğal tasarımı
     * rakamları yuvarlak/oval yapıyor, ExtraBold weight + 70 px büyüklükte
     * bu yuvarlaklık eğik gibi algılanıyor. Motion blur değil, font tasarım
     * özelliği. Montserrat geometric/dik/upright — eğik görünmüyor. */
    s_lbl_temp = lv_label_create(screen);
    lv_obj_remove_style_all(s_lbl_temp);
    lv_label_set_long_mode(s_lbl_temp, LV_LABEL_LONG_CLIP);
#ifdef BOARD_VARIANT_LCD128
    /* Session 41 (mockup): "21°" tek etiket, 48 punto, ORTALI — ayrı "°"
     * etiketi yok; ortalama sabit kutuda olduğu için kayma sorunu yok. */
    lv_obj_set_size(s_lbl_temp, 200, 56);
    lv_obj_set_style_text_align(s_lbl_temp, LV_TEXT_ALIGN_CENTER, 0);
#else
    lv_obj_set_size(s_lbl_temp, 140, 70);
    lv_obj_set_style_text_align(s_lbl_temp, LV_TEXT_ALIGN_RIGHT, 0);
#endif
    lv_obj_set_style_text_color(s_lbl_temp, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_lbl_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_bg_color(s_lbl_temp, lbl_bg, 0);
    lv_obj_set_style_bg_opa(s_lbl_temp, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_lbl_temp, 0, 0);
    /* Defansif: theme veya parent inheritance ile transform/skew sızması varsa
     * sıfırla. LV 9'da labelda transform_skew olamaz teorik olarak ama emin
     * olmak için. */
    lv_obj_set_style_transform_rotation(s_lbl_temp, 0, 0);
    lv_obj_set_style_transform_scale_x(s_lbl_temp, 256, 0);
    lv_obj_set_style_transform_scale_y(s_lbl_temp, 256, 0);
    lv_obj_set_style_transform_skew_x(s_lbl_temp, 0, 0);
#ifdef BOARD_VARIANT_LCD128
    lv_label_set_text(s_lbl_temp, "--°");
#else
    lv_label_set_text(s_lbl_temp, "--");
#endif
    /* Mont 48'de "14" ≈ 56 px, ° Mont 28 ≈ 18 px → combo ~74 px.
     * Center on 410: visual_left = 168. Text RIGHT-align in 140-px bbox →
     * bbox.right = 168+56 = 224 → bbox.x = 84 (even). */
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_pos(s_lbl_temp, 20, 66);   /* 66..122 */
#else
    lv_obj_set_pos(s_lbl_temp, 84, 130);
#endif

    /* Derece simgesi — Mont 28 (Mont 36 yerine; daha kompakt ve uyumlu) */
#ifndef BOARD_VARIANT_LCD128
    s_lbl_temp_deg = lv_label_create(screen);
    lv_obj_remove_style_all(s_lbl_temp_deg);
    lv_label_set_long_mode(s_lbl_temp_deg, LV_LABEL_LONG_CLIP);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_lbl_temp_deg, 24, 20);
#else
    lv_obj_set_size(s_lbl_temp_deg, 40, 36);
#endif
    lv_obj_set_style_text_align(s_lbl_temp_deg, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(s_lbl_temp_deg, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_lbl_temp_deg, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(s_lbl_temp_deg, &lv_font_montserrat_28, 0);
#endif
    lv_obj_set_style_bg_color(s_lbl_temp_deg, lbl_bg, 0);
    lv_obj_set_style_bg_opa(s_lbl_temp_deg, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_lbl_temp_deg, 0, 0);
    lv_label_set_text(s_lbl_temp_deg, "°");
    lv_obj_set_pos(s_lbl_temp_deg, 224, 134);
#endif /* !LCD128 — Session 41: 240'ta ° sıcaklık etiketinin içinde */

    /* Açıklama */
    s_lbl_desc = lv_label_create(screen);
    lv_obj_remove_style_all(s_lbl_desc);
    lv_label_set_long_mode(s_lbl_desc, LV_LABEL_LONG_CLIP);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_lbl_desc, 200, 20);
#else
    lv_obj_set_size(s_lbl_desc, 360, 34);
#endif
    lv_obj_set_style_text_color(s_lbl_desc, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_lbl_desc, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(s_lbl_desc, &lv_font_montserrat_22, 0);
#endif
    lv_obj_set_style_text_align(s_lbl_desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_lbl_desc, lbl_bg, 0);
    lv_obj_set_style_bg_opa(s_lbl_desc, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_lbl_desc, 0, 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_color(s_lbl_desc, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_set_pos(s_lbl_desc, 20, 124);   /* 124..144 */
#else
    lv_obj_set_pos(s_lbl_desc, 24, 230);
#endif
    lv_label_set_text(s_lbl_desc, "Loading...");

    /* H/L */
    s_lbl_hilo = lv_label_create(screen);
    lv_obj_remove_style_all(s_lbl_hilo);
    lv_label_set_long_mode(s_lbl_hilo, LV_LABEL_LONG_CLIP);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_lbl_hilo, 200, 16);
#else
    lv_obj_set_size(s_lbl_hilo, 360, 26);
#endif
    lv_obj_set_style_text_color(s_lbl_hilo, lv_color_make(0xCC, 0xCC, 0xCC), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_lbl_hilo, &lv_font_montserrat_12, 0);
#else
    lv_obj_set_style_text_font(s_lbl_hilo, &lv_font_montserrat_16, 0);
#endif
    lv_obj_set_style_text_align(s_lbl_hilo, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_lbl_hilo, lbl_bg, 0);
    lv_obj_set_style_bg_opa(s_lbl_hilo, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_lbl_hilo, 0, 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_color(s_lbl_hilo, lv_color_make(0x8E, 0x8E, 0x93), 0);
    lv_obj_set_pos(s_lbl_hilo, 20, 146);   /* 146..162 */
#else
    lv_obj_set_pos(s_lbl_hilo, 24, 266);
#endif
    lv_label_set_text(s_lbl_hilo, "H:--°  L:--°");
}

/* Dynamic label'ları sil (sıradaki rebuild için temizle). */
static void destroy_dynamic_text_labels(void)
{
    if (s_lbl_city)     { lv_obj_delete(s_lbl_city);     s_lbl_city = NULL; }
    if (s_lbl_temp)     { lv_obj_delete(s_lbl_temp);     s_lbl_temp = NULL; }
    if (s_lbl_temp_deg) { lv_obj_delete(s_lbl_temp_deg); s_lbl_temp_deg = NULL; }
    if (s_lbl_desc)     { lv_obj_delete(s_lbl_desc);     s_lbl_desc = NULL; }
    if (s_lbl_hilo)     { lv_obj_delete(s_lbl_hilo);     s_lbl_hilo = NULL; }
}

// ─── Şehir liste overlay'i (Apple Watch tarzı) ────────────────────────────────

static void overlay_close(void);

/* Card click handler: kullanıcı bir şehir kartına dokundu → primary yap.
 *
 * Video analizi (2026-05-05 14:47): kullanıcı listeden şehir seçince eski
 * derece pixelleri (örn. "10°") panel internal RAM'inde tam temizlenmiyor,
 * yeni "19°" yazılınca eski + yeni pixel overlap'i → görsel "italic/eğik"
 * ghost efekti. Bu kullanıcının "kayık" şikayeti = double-exposure.
 *
 * Çözüm: dynamic-text label'larını destroy + rebuild + screen invalidate +
 * lv_refr_now ile SENKRON full render zorla. Partial-flush race'i tamamen
 * bypass ediyor. lv_async_call kullanıyoruz çünkü click handler içinden
 * direkt lv_refr_now re-entry'ye yol açabilir; async sıraya alınca sonraki
 * tick'te güvenle çalışıyor. */
static void city_card_click_cb(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "City selected: idx=%d (%s)", idx,
             weather_service_get_city_name(idx));
    weather_service_set_primary_idx(idx);
    overlay_close();

    /* Dynamic-text label'larını yeniden oluştur — fresh state */
    if (s_screen) {
        destroy_dynamic_text_labels();
        build_dynamic_text_labels(s_screen);
        /* Screen geneline force invalidate — overlay-residue alanını da kapsasın */
        lv_obj_invalidate(s_screen);
    }
    update_ui_from_snapshot();

    /* Senkron tam-ekran render — overlay async-delete tamamlandıktan sonra
     * (bir sonraki LVGL tick) tüm dirty area'lar tek render pass'inde işleniyor.
     * Panel internal RAM'de eski pixel kalmadan yeni state ile aynı senkron
     * yazılıyor → ghost/double-exposure imkânsız. */
    lv_async_call([](void* /*arg*/) {
        lv_refr_now(NULL);
    }, NULL);
}

/* "+" butonu — gelecekte custom city ekleme. Şu an yer tutucu. */
static void overlay_add_click_cb(lv_event_t* /*e*/)
{
    ESP_LOGI(TAG, "Add city tapped (placeholder)");
    /* Gelecekte: il listesi diyaloğu. Şu an no-op. */
}

/* Bir kartın temp / time label'larını güncel snapshot ile yenile.
 *
 * Önemli: LVGL `lv_label_set_text` sadece label'ın yeni bbox'unu invalidate
 * ediyor. CO5300 AMOLED partial flush + label'ın altında kalan kart gradyanının
 * yeniden boyanması gereken alan tam temizlenmiyor → eski text pikselleri
 * residue olarak kalıyor ("kayma" / "bozulma" hissi). Çözüm: text değiştiğinde
 * tüm card'ı invalidate edip clean redraw yaptırıyoruz. has_data=false iken
 * "--:--" gösteriyoruz — eski "Yukleniyor..." (uzun) → "11:30" (kısa) geçişi
 * de aynı residue sorunu yaratıyordu, sabit-uzunluk placeholder ile bunu
 * tamamen önlüyoruz. */
static void update_card_labels(overlay_card_t* c)
{
    if (!c || !c->lbl_temp || !c->lbl_time) return;
    weather_data_t d;
    bool has_data = weather_service_get_city(c->idx, &d);

    char temp_buf[8];
    char time_buf[20];
    if (has_data) snprintf(temp_buf, sizeof(temp_buf), "%d", rnd_int(d.temperature));
    else          snprintf(temp_buf, sizeof(temp_buf), "--");

    if (has_data && d.updated_at_s > 0) {
        time_t ts = (time_t)d.updated_at_s;
        struct tm ti;
        localtime_r(&ts, &ti);
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    } else {
        /* Sabit uzunluk placeholder — "Yukleniyor..." kullanılırsa text shrink
         * olunca residue oluşuyordu. "--:--" ile bbox tam aynı boyutta kalıyor. */
        snprintf(time_buf, sizeof(time_buf), "--:--");
    }

    bool changed = (strcmp(temp_buf, c->last_temp) != 0) ||
                   (strcmp(time_buf, c->last_time) != 0);
    if (!changed) return;

    lv_label_set_text(c->lbl_temp, temp_buf);
    lv_label_set_text(c->lbl_time, time_buf);
    strncpy(c->last_temp, temp_buf, sizeof(c->last_temp) - 1);
    c->last_temp[sizeof(c->last_temp) - 1] = '\0';
    strncpy(c->last_time, time_buf, sizeof(c->last_time) - 1);
    c->last_time[sizeof(c->last_time) - 1] = '\0';

    /* Full card invalidate — gradient + tüm label'lar tek bir clean
     * burst ile yeniden boyansın. Maliyet: card 340×130 = 44 KB pikselin
     * RGB565 olarak yeniden çizimi (~3-4 ms LVGL renderer'da). 1 Hz timer
     * cadence'ında problem değil. */
    if (c->card) lv_obj_invalidate(c->card);
}

/* Tek kartı oluştur — Apple Watch hava durumu liste tarzı:
 *   - Yuvarlak köşeli mavi gradient kart (~340×120)
 *   - Sol üst: şehir adı (Montserrat 22)
 *   - Sol orta: büyük sıcaklık (Montserrat 46 + ° süs) yan yana
 *   - Sol alt: küçük güncellenme saati
 * Card label pointers s_overlay_cards[]'a yazılır → 1Hz timer reaktif yeniler. */
static lv_obj_t* build_city_card(lv_obj_t* parent, int idx)
{
    const char* name = weather_service_get_city_name(idx);

    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 340, 130);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Apple Watch tarzı mavi gradient + büyük corner radius. PRESSED-state bg
     * color flip TAMAMEN kaldırıldı: kart 340×130 büyük, tap anında bg/grad
     * değişimi partial-flush chunk'larını farklı renklerle flush ediyordu
     * (üst chunk eski renk, alt chunk yeni renk → "ikiye bölünme" hissi).
     * Tap teyidi: menü anında kapanıyor, yeni şehir ana ekranda görünüyor —
     * bu zaten yeterli görsel feedback. */
    lv_obj_set_style_bg_color(card, lv_color_make(0x4A, 0x7B, 0xC0), 0);
    lv_obj_set_style_bg_grad_color(card, lv_color_make(0x6F, 0x9C, 0xD8), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    /* Şehir adı — sol üst. Sabit genişlik + LONG_CLIP: text uzunluğu değişse de
     * label bbox değişmez → residue temizlik garantisi. */
    lv_obj_t* lbl_name = lv_label_create(card);
    lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(lbl_name, 280, 26);
    lv_obj_set_style_text_color(lbl_name, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_22, 0);
    lv_label_set_text(lbl_name, name);
    lv_obj_set_pos(lbl_name, 22, 12);

    /* Büyük sıcaklık — sabit 100×56 bbox. "--" → "21" geçişinde auto-resize
     * eski bbox'ı invalidate etmiyordu → "Yukleniyor..." üstüne yeni text
     * yazılırken eski pikseller residue olarak kalıyordu (kullanıcı
     * raporu 2026-05-05). LONG_CLIP ile bbox sabit, LVGL doğru invalidate. */
    lv_obj_t* lbl_temp = lv_label_create(card);
    lv_label_set_long_mode(lbl_temp, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(lbl_temp, 110, 56);
    lv_obj_set_style_text_color(lbl_temp, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_temp, &lv_font_montserrat_46, 0);
    lv_label_set_text(lbl_temp, "--");
    lv_obj_set_pos(lbl_temp, 22, 42);

    /* Derece simgesi — sıcaklığın hemen sağında, üstte. lbl_temp 100px sabit
     * → ° label da sabit yere konuyor (set_pos x=126). */
    lv_obj_t* lbl_deg = lv_label_create(card);
    lv_obj_set_style_text_color(lbl_deg, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_deg, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_deg, "°");
    lv_obj_set_pos(lbl_deg, 92, 46);

    /* Saat — küçük, sol alt köşe. Sabit 200×20 bbox; "Yukleniyor..." (>100 px)
     * ile "11:30" (~35 px) arasında geçişte residue olmasın diye. */
    lv_obj_t* lbl_time = lv_label_create(card);
    lv_label_set_long_mode(lbl_time, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(lbl_time, 200, 20);
    lv_obj_set_style_text_color(lbl_time, lv_color_make(0xE8, 0xE8, 0xE8), 0);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_time, "--:--");
    lv_obj_set_pos(lbl_time, 22, 96);

    /* Tap handler — index'i user_data ile passla */
    lv_obj_add_event_cb(card, city_card_click_cb, LV_EVENT_CLICKED,
                        (void*)(intptr_t)idx);
    /* NOT: wear_press_feedback_attach KALDIRILDI. Opacity animasyonu (COVER →
     * 60 → COVER, ~140 ms) full-screen card'da partial-flush chunk'larını
     * farklı opa değerlerinde flush ediyor → kart "ikiye bölünmüş" görünüyordu
     * (kullanıcı raporu 2026-05-05). Üstteki PRESSED-state bg color flip
     * (instant, animasyon yok) zaten yeterli görsel teyit veriyor. */

    /* Reaktif tazeleme için label + card pointer'ları kaydet */
    if (s_overlay_card_count < WEATHER_CITY_COUNT) {
        s_overlay_cards[s_overlay_card_count].idx      = idx;
        s_overlay_cards[s_overlay_card_count].card     = card;
        s_overlay_cards[s_overlay_card_count].lbl_temp = lbl_temp;
        s_overlay_cards[s_overlay_card_count].lbl_time = lbl_time;
        s_overlay_cards[s_overlay_card_count].last_temp[0] = '\0';
        s_overlay_cards[s_overlay_card_count].last_time[0] = '\0';
        s_overlay_card_count++;
    }
    /* Mevcut snapshot ile bir kez doldur — cached varsa hemen görünür */
    update_card_labels(&s_overlay_cards[s_overlay_card_count - 1]);

    return card;
}

/* 1 Hz LV timer — fetch task arka planda yeni şehri çektikçe kartlar canlı
 * güncellensin. Maliyet: 2 kart × 1 mutex take + memcpy + label update = <500 µs. */
static void overlay_refresh_timer_cb(lv_timer_t* /*t*/)
{
    if (!s_overlay) return;
    for (int i = 0; i < s_overlay_card_count; i++) {
        update_card_labels(&s_overlay_cards[i]);
    }
}

/* Overlay aç — tüm primary olmayan şehirler için kart üret + "+" butonu. */
static void overlay_open(void)
{
    if (s_overlay) return;
    if (!s_screen) return;

    /* Card pointer kaydını sıfırla — overlay her açılışında yeniden inşa edilir */
    s_overlay_card_count = 0;
    for (int i = 0; i < WEATHER_CITY_COUNT; i++) {
        s_overlay_cards[i].idx      = -1;
        s_overlay_cards[i].card     = NULL;
        s_overlay_cards[i].lbl_temp = NULL;
        s_overlay_cards[i].lbl_time = NULL;
        s_overlay_cards[i].last_temp[0] = '\0';
        s_overlay_cards[i].last_time[0] = '\0';
    }

    /* Fetch task'i uyandır — secondary city'ler henüz çekilmemiş olabilir
     * (yeni firmware ilk boot, NVS cache boş). Task uyanınca tüm şehirleri
     * sırayla çekecek; tamamlanan her şehir overlay_refresh_timer_cb ile
     * canlı yansıyacak. Task halihazırda fetch ediyorsa notify queue'ya
     * yığılır ve mevcut tur bitince hemen yeni tur başlar. */
    weather_service_refresh_now();

    int primary = weather_service_get_primary_idx();

    /* Tam ekran kapsayıcı, scrollable column flex */
    s_overlay = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_overlay);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_overlay, 240, 240);
#else
    lv_obj_set_size(s_overlay, 410, 502);
#endif
    lv_obj_set_pos(s_overlay, 0, 0);

    /* Solid bg — ana ekran ile aynı koyu mavi. Gradient kaldırıldı (label
     * residue düzeltmesi için). */
    lv_obj_set_style_bg_color(s_overlay, lv_color_make(0x03, 0x0A, 0x1F), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(s_overlay, LV_GRAD_DIR_NONE, 0);

    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_START,
                                     LV_FLEX_ALIGN_CENTER,
                                     LV_FLEX_ALIGN_CENTER);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_pad_top(s_overlay, 40, 0);
    lv_obj_set_style_pad_bottom(s_overlay, 30, 0);
    lv_obj_set_style_pad_row(s_overlay, 10, 0);
#else
    lv_obj_set_style_pad_top(s_overlay, 70, 0);     /* AMOLED üst eğri için */
    lv_obj_set_style_pad_bottom(s_overlay, 60, 0);
    lv_obj_set_style_pad_row(s_overlay, 18, 0);
#endif
    lv_obj_set_scrollbar_mode(s_overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_overlay, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_overlay, LV_SCROLL_SNAP_NONE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    /* Click events kartlardan/butondan gelmeli, overlay arka planından değil */
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    /* Sağ üstte küçük saat (ana ekrandakiyle uyumlu) */
    lv_obj_t* lbl_hdr_time = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(lbl_hdr_time, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_hdr_time, &lv_font_montserrat_20, 0);
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    char hdr[12];
    snprintf(hdr, sizeof(hdr), "%02d:%02d", ti.tm_hour, ti.tm_min);
    lv_label_set_text(lbl_hdr_time, hdr);
    /* Flex item — kendi genişliğince, üst pad ile (zaten pad_top=70 var) */

    /* Primary olmayan şehirleri sırayla ekle */
    for (int i = 0; i < WEATHER_CITY_COUNT; i++) {
        if (i == primary) continue;
        build_city_card(s_overlay, i);
    }

    /* "+" butonu — gri yuvarlak */
    lv_obj_t* add_btn = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(add_btn);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(add_btn, 40, 40);
#else
    lv_obj_set_size(add_btn, 56, 56);
#endif
    lv_obj_add_flag(add_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(add_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(add_btn, lv_color_make(0x55, 0x55, 0x60), 0);
    lv_obj_set_style_bg_opa(add_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(add_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(add_btn, lv_color_make(0x45, 0x45, 0x50), LV_STATE_PRESSED);

    lv_obj_t* plus = lv_label_create(add_btn);
    lv_obj_set_style_text_color(plus, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(plus, &lv_font_montserrat_22, 0);
#else
    lv_obj_set_style_text_font(plus, &lv_font_montserrat_36, 0);
#endif
    lv_label_set_text(plus, "+");
    lv_obj_center(plus);

    lv_obj_add_event_cb(add_btn, overlay_add_click_cb, LV_EVENT_CLICKED, NULL);
    /* + butonu için de press feedback animasyonu kaldırıldı; PRESSED-state
     * bg flip (0x55→0x45) zaten yeterli görsel teyit. */

    /* Animasyon YOK — opacity fade-in (180 ms) AMOLED partial flush'ta
     * 4-6 step "dalgalanma" hissi yaratıyordu (full-screen overlay'in tüm
     * çocuklarını her frame'de farklı opa ile re-blend etmek pahalı; 60 fps
     * yetişmiyor). Apple Watch settings menüleri de instant açılır. */
    lv_obj_set_style_opa(s_overlay, LV_OPA_COVER, 0);

    /* 1 Hz tazeleme — fetch task arka planda şehirleri çektikçe kartlar
     * "--°" yerine gerçek değerleri reaktif gösterir. */
    if (!s_overlay_timer) {
        s_overlay_timer = lv_timer_create(overlay_refresh_timer_cb, 1000, NULL);
    }

    ESP_LOGI(TAG, "City overlay opened (cards=%d, refresh triggered)",
             s_overlay_card_count);
}

static void overlay_close(void)
{
    if (!s_overlay) return;
    /* Önce timer'ı durdur — kart pointer'ları lv_obj_delete sonrası invalid olur,
     * timer hala çalışıyorsa stale pointer'lara yazma riski. */
    if (s_overlay_timer) {
        lv_timer_delete(s_overlay_timer);
        s_overlay_timer = NULL;
    }
    s_overlay_card_count = 0;
    for (int i = 0; i < WEATHER_CITY_COUNT; i++) {
        s_overlay_cards[i].card     = NULL;
        s_overlay_cards[i].lbl_temp = NULL;
        s_overlay_cards[i].lbl_time = NULL;
    }

    /* Animasyon YOK — instant hide + async delete. Opacity fade-out (140 ms)
     * full-screen overlay'de "yavaş dalgalı" hissi yaratıyordu (partial flush
     * + opa blend 60 fps yetişmiyordu). Apple Watch tarzı: tap → press
     * feedback (kart dim, ~140 ms wear_press_feedback) zaten görsel teyit
     * veriyor; üstüne fade gerekmiyor.
     *
     * delete_async: city_card_click_cb event dispatch zinciri içinden
     * çağrılıyor — sync delete LVGL'in bubble/cleanup iterasyonunda stale
     * pointer riski. async ile bir sonraki LVGL tick'inde güvenle silinir. */
    lv_obj_t* doomed = s_overlay;
    s_overlay = NULL;
    /* Kullanıcının ekranda görmesini engellemek için anında hide flag */
    lv_obj_add_flag(doomed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(doomed);
    ESP_LOGI(TAG, "City overlay closed");
}

/* Sol üst liste ikonu tıklanınca overlay'i aç */
static void listicon_click_cb(lv_event_t* /*e*/)
{
    if (s_overlay) overlay_close();
    else           overlay_open();
}

// ─── Olaylar ──────────────────────────────────────────────────────────────────

static void gesture_cb(lv_event_t* /*e*/)
{
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_LEFT) {
        /* Overlay açıksa ilk önce onu kapat — kullanıcı geri-jest'le ana ekrana
         * dönmek isteyebilir ama ilk swipe overlay'i kapatsın */
        if (s_overlay) overlay_close();
        else           app_manager_back();
    }
}

// ─── App lifecycle ─────────────────────────────────────────────────────────────

static void on_start(lv_obj_t* screen)
{
    ESP_LOGI(TAG, "Weather app starting");
    s_screen = screen;
    s_overlay = NULL;

    /* Arka plan: solid koyu mavi. Önceden gradient (top→bottom dark blue→black)
     * kullanılıyordu ama her label opaque-bg ile clear olduğunda label'ın bg'si
     * gradient'ten farklı bir y-noktasındaki renge denk geliyordu → label edge'i
     * gradient'ten ayırt ediliyordu. Solid renk ile label-bg ve screen-bg
     * tamamen aynı → label dikdörtgenleri kaybolup düz görünüyor. */
#ifdef BOARD_VARIANT_LCD128
    /* Session 41 (mockup): saf siyah zemin, ikonsuz tipografik düzen. */
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
#else
    lv_obj_set_style_bg_color(screen, lv_color_make(0x03, 0x0A, 0x1F), 0);
#endif
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    /* Gradient flag'ini explicit kapat (önceki gradient style'larından kalıntı
     * olmasın diye) */
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    // ─── Üst bar ──────────────────────────────────────────────────────────────
    /* Sol turuncu liste ikonu — TIKLANIR (overlay aç).
     * Hit-target 44×44 olacak şekilde küçük ikon biraz büyük bir kapsayıcıya
     * konuluyor; parmak ucu ile kolayca isabet ettirilebilsin. */
#ifndef BOARD_VARIANT_LCD128   /* Session 41: 240'ta liste ikonu yok (şehir adı tıklanır) */
    s_listicon = lv_obj_create(screen);
    lv_obj_remove_style_all(s_listicon);
    lv_obj_set_size(s_listicon, 56, 56);
    lv_obj_set_pos(s_listicon, 24, 22);
    lv_obj_add_flag(s_listicon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_listicon, LV_OBJ_FLAG_SCROLLABLE);
    /* v1.4.64: pos (24, 22) display sol-üst curve'üne yakın — ext_click_area 16
     * → effective ~88×88 isabet alanı (zaten 56×56 olan görsel'den +16 her yöne) */
    lv_obj_set_ext_click_area(s_listicon, 16);
    lv_obj_set_style_bg_opa(s_listicon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_listicon, LV_RADIUS_CIRCLE, 0);
    /* PRESSED state: hafif yarı saydam */
    lv_obj_set_style_bg_color(s_listicon, lv_color_make(0xFF, 0x9E, 0x3A),
                               LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_listicon, LV_OPA_30, LV_STATE_PRESSED);

    lv_obj_t* listicon_lbl = lv_label_create(s_listicon);
    lv_obj_set_style_text_color(listicon_lbl, lv_color_make(0xFF, 0x9E, 0x3A), 0);
    lv_obj_set_style_text_font(listicon_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(listicon_lbl, LV_SYMBOL_LIST);
    lv_obj_center(listicon_lbl);
    lv_obj_add_event_cb(s_listicon, listicon_click_cb, LV_EVENT_CLICKED, NULL);
#endif /* !LCD128 */

    // Saat (orta) — 22pt'de üst kenar AMOLED'in eğrisine giriyor ve yamuk
    // görünüyordu. 20pt + y=36 ile glyph üst kenarı düz alana sığıyor.
    s_lbl_time = lv_label_create(screen);
    lv_obj_set_style_text_color(s_lbl_time, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_time, lv_color_make(0x8E, 0x8E, 0x93), 0);
    lv_obj_set_style_text_align(s_lbl_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_time, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_lbl_time, 80);
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_MID, 0, 24);
#else
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_lbl_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_time, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_lbl_time, 120);
    lv_obj_align(s_lbl_time, LV_ALIGN_TOP_MID, 0, 36);
#endif
    lv_label_set_text(s_lbl_time, "--:--");

    // Sağ üst hava ikon kutusu (44×44)
#ifndef BOARD_VARIANT_LCD128   /* Session 41: 240'ta hava ikonu yok */
    s_icon_box = lv_obj_create(screen);
    lv_obj_remove_style_all(s_icon_box);
    lv_obj_set_size(s_icon_box, 44, 44);
    lv_obj_set_pos(s_icon_box, 410 - 44 - 30, 26);
    lv_obj_clear_flag(s_icon_box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                                   LV_OBJ_FLAG_SCROLLABLE));
#endif

    /* Dynamic-text label'lar yardımcı fonksiyondan oluşturuluyor — şehir
     * tıklamasında yeniden oluşturmak için aynı fonksiyon kullanılacak. */
#ifdef BOARD_VARIANT_LCD128
    s_lbl_bg_color = lv_color_black();
#else
    s_lbl_bg_color = lv_color_make(0x03, 0x0A, 0x1F);
#endif
    build_dynamic_text_labels(screen);

#ifdef BOARD_VARIANT_LCD128
    /* ─── Session 41 (mockup): halka/pusula yerine üç sütunlu istatistik satırı
     *   4      12      63        ← değer, 16 punto (UV turuncu, nem cyan)
     *   UV    km/h     %         ← birim, 12 punto gri
     * Değerler y=170..189, birimler 190..204; y=204'te kiriş x=57..183,
     * küme 80..160 → daire içinde. */
    {
        struct { lv_obj_t** out; const char* unit; lv_color_t col; int cx; } cols[3] = {
            /* Session 41b: kutular 40 px + 26 px adımla ÜST ÜSTE biniyordu; opak
             * siyah zemin sonraki kutuyu öncekinin üstüne çiziyor, "18"in 8'i
             * nem kutusunun altında kalıyordu. 32 px kutu, 34 px adım → 70..102,
             * 104..136, 138..170: çakışma yok, "100" (28 px) sığar. */
            { &s_lbl_uv,   "UV",   lv_color_make(0xFF, 0x9F, 0x0A),  86 },
            { &s_lbl_wind, "km/h", lv_color_white(),                 120 },
            { &s_lbl_hum,  "%",    lv_color_make(0x4F, 0xC3, 0xF7),  154 },
        };
        for (int i = 0; i < 3; i++) {
            lv_obj_t* v = lv_label_create(screen);
            lv_obj_remove_style_all(v);
            lv_label_set_long_mode(v, LV_LABEL_LONG_CLIP);
            lv_obj_set_size(v, 32, 20);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(v, cols[i].col, 0);
            lv_obj_set_style_bg_color(v, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
            lv_obj_set_pos(v, cols[i].cx - 16, 170);
            lv_label_set_text(v, "0");
            *cols[i].out = v;

            lv_obj_t* u = lv_label_create(screen);
            lv_obj_set_style_text_font(u, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(u, lv_color_make(0x8E, 0x8E, 0x93), 0);
            lv_label_set_text(u, cols[i].unit);
            lv_obj_update_layout(u);
            lv_obj_set_pos(u, cols[i].cx - lv_obj_get_width(u) / 2, 191);
        }
    }
#else
    // ─── 3 halka widget (üst başlıksız) ───────────────────────────────────────
    const int RY = 320;
    const int RS = 110;
    const int RX0 = 24;
    const int GAP = 16;

    s_arc_uv = make_ring(screen, RX0, RY, RS, 0, 11,
                         lv_color_make(0xFF, 0xB7, 0x00),
                         lv_color_make(0x33, 0x2A, 0x14),
                         &s_lbl_uv, "indeks");

    make_wind_compass(screen, RX0 + RS + GAP, RY, RS);

    s_arc_hum = make_ring(screen, RX0 + 2*(RS + GAP), RY, RS, 0, 100,
                          lv_color_make(0x4F, 0xC3, 0xF7),
                          lv_color_make(0x10, 0x28, 0x40),
                          &s_lbl_hum, "%");
#endif /* !LCD128 halkalar */

    update_ui_from_snapshot();

    s_clk_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
    clock_timer_cb(NULL);

    app_manager_enable_gestures(screen);
    lv_obj_add_event_cb(screen, gesture_cb, LV_EVENT_GESTURE, NULL);

    weather_service_refresh_now();
}

static void on_resume(void)
{
    update_ui_from_snapshot();
    if (s_clk_timer) lv_timer_resume(s_clk_timer);
}

static void on_pause(void)
{
    if (s_clk_timer) lv_timer_pause(s_clk_timer);
    /* App pauselandığında overlay'i temizle — geri dönülünce ana ekran taze
     * gözüksün. lv_obj_delete: overlay screen'in child'ı, screen ölmeden
     * elden çıkarılmalı. */
    if (s_overlay_timer) { lv_timer_delete(s_overlay_timer); s_overlay_timer = NULL; }
    s_overlay_card_count = 0;
    if (s_overlay) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }
}

static void on_stop(void)
{
    if (s_clk_timer) { lv_timer_delete(s_clk_timer); s_clk_timer = NULL; }
    if (s_overlay_timer) { lv_timer_delete(s_overlay_timer); s_overlay_timer = NULL; }
    s_overlay_card_count = 0;
    s_last_city_text[0] = '\0';     /* Sonraki app start'ta ilk update force redraw */
    s_screen = NULL;
    s_overlay = NULL;
    s_listicon = NULL;
    s_lbl_time = s_icon_box = s_lbl_city = s_lbl_temp = s_lbl_temp_deg = NULL;
    s_lbl_desc = s_lbl_hilo = NULL;
    s_arc_uv = s_lbl_uv = NULL;
    s_wind_arrow = s_lbl_wind = NULL;
    s_arc_hum = s_lbl_hum = NULL;
}

static bool on_event(const os_event_t* event)
{
    if (event->type == EVENT_WEATHER_UPDATE) {
        update_ui_from_snapshot();
        /* Overlay açıksa, fetch'i tamamlanan şehrin kartı anında dolsun —
         * 1 Hz timer'ı beklemeye gerek yok. event payload high byte = idx;
         * direkt o card'ı update etmek de mümkündü ama tüm card'ları
         * tazelemek (en fazla 2-3 kart) idempotent + cheap. */
        if (s_overlay) {
            for (int i = 0; i < s_overlay_card_count; i++) {
                update_card_labels(&s_overlay_cards[i]);
            }
        }
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
    .name      = "Weather",
};

const app_interface_t* weather_app_get_interface(void) { return &s_iface; }
