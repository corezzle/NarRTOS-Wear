/**
 * @file alarm_app.cpp
 * @brief Alarm uygulaması — Apple Watch tarzı liste + edit + ringing ekranları.
 *
 * Üç görünüm tek "screen" üzerinde container-swap ile çalışır:
 *   VIEW_LIST    → Mevcut alarmlar listesi + "+" yeni alarm
 *   VIEW_EDIT    → Saat/dakika sürüklemeli digit-box'ları + gün toggle + Save
 *   VIEW_RINGING → Tam-ekran çalan alarm (büyük çan ikonu) + Snooze/Stop
 *
 * Edit ekranı — Apple Watch tarzı dial + iki sabit kutu:
 *   Önceki iki paradigma (lv_roller, ardından "drag-to-change" 3-etiketli
 *   statik kutu) 80-satırlık parça-display-buffer ile kullanıcı kayması
 *   problemini çözemedi. 6 iterasyon boyunca theme-NULL + buffer-align +
 *   fixed-width + hysteresis + curr-only-update + hide-prev-next + rate-limit
 *   denendi; hiçbiri kaymayı tamamen bitirmedi.
 *
 *   ÇÖZÜM (radikal paradigma değişimi): scroll/roller mantığı tamamen
 *   atılır. Dijitler ASLA scroll etmez. Yerine:
 *     • Ortada iki SABİT kutu: HH ve MM, font 28, ortalı.
 *     • Etrafında çevresel "dial" — saat modunda 12 sayı (00,02,...,22),
 *       dakika modunda 12 sayı (00,05,...,55) çember üzerinde.
 *     • Aktif kutu (yeşil border) hangisini düzenlediğini gösterir —
 *       diğerine tıklanınca aktif kutu değişir.
 *     • Kullanıcı dial üzerinde herhangi bir noktaya dokunduğunda parmağın
 *       merkezi referansla açısı hesaplanır (atan2), açıdan değer çıkarılır
 *       (hour: 0-23, minute: 0-59), seçili kutuya yazılır.
 *     • Sürükleme de aynı: parmak dial üzerinde gezinince anlık açı = anlık
 *       değer. Kayma fiziksel olarak imkansız çünkü dijit kutusu hiç scroll
 *       etmez, sadece text değişir + (90 px×60 px) küçük bölge invalidate.
 *     • Mevcut değerin açısal pozisyonunda turuncu nokta = position marker.
 *
 * Ringing tetikleme:
 *   alarm_service tick_cb() içinde dakika sınırı geçtiğinde
 *   EVENT_APP_SWITCH(APP_ID_ALARM) yayınlar; app_manager bu uygulamayı
 *   ön plana çıkarır. on_start, alarm_service_pop_pending() çağrısıyla
 *   pending var mı kontrol eder; varsa doğrudan VIEW_RINGING'e geçer,
 *   yoksa VIEW_LIST'i kurar.
 *
 * Yan etkiler:
 *   • Ringing girişinde display brightness 255'e çekilir; çıkışta önceki
 *     değere döner (kullanıcı uyandırma için maksimum görünürlük).
 *   • Snooze: 5 dakika sonra tekrar pending olur (alarm_service_snooze).
 */

#include "alarm_app.h"
#include "../../core/app_manager.h"
#include "../../core/os_config.h"
#include "../../core/event_bus.h"
#include "../../services/alarm_service.h"
#include "../../services/audio_service.h"
#include "../../services/ui_service.h"
#include "../../drivers/display/co5300_driver.h"
#include "../../../include/wear_motion.h"
#include <lvgl.h>
#include <esp_log.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

static const char* TAG = "AlarmApp";

#ifdef BOARD_VARIANT_LCD128
#define SAFE_X 18
#define SAFE_W 204
#else
#define SAFE_X 12
#define SAFE_W 386
#endif

typedef enum { VIEW_LIST, VIEW_EDIT, VIEW_RINGING } view_t;

/* ─── State ──────────────────────────────────────────────────────────────── */

static lv_obj_t* s_screen = NULL;

static lv_obj_t* s_view_list   = NULL;
static lv_obj_t* s_view_edit   = NULL;
static lv_obj_t* s_view_ring   = NULL;

/* Aktif ekran — BTN_TOP davranışını koşullamak için tutulur (sadece
 * VIEW_EDIT'te BTN_TOP HH/MM increment'ı; diğer ekranlarda app_manager
 * default'u (back) çalışır). show_list/edit/ringing girişinde set edilir. */
static view_t s_current_view = VIEW_LIST;

/* Edit görünümü state */
static int       s_edit_idx     = -1;   /* düzenlenen slot, -1=yeni */
static alarm_t   s_edit_buf     = {};   /* drag handler bunu canlı günceller */

/* Apple Watch tarzı dial state */
static int       s_edit_mode    = 0;    /* 0 = saat editing, 1 = dakika editing */
static lv_obj_t* s_box_hour     = NULL; /* HH kutusu (border yeşil = aktif) */
static lv_obj_t* s_box_minute   = NULL; /* MM kutusu */
static lv_obj_t* s_lbl_hour     = NULL; /* "HH" yazan label */
static lv_obj_t* s_lbl_minute   = NULL; /* "MM" yazan label */
/* Türk saati (24-hr) — analog saat tarzı 12-saat döngüsü dial'da çift modlu:
 *   AM (sabah) dial: 00 (top), 03 (right), 06 (bottom), 09 (left) — hour 0-11
 *   PM (akşam) dial: 12 (top), 15 (right), 18 (bottom), 21 (left) — hour 12-23
 * Hour 11→12 veya 23→00 sınırı geçildiğinde dial otomatik swap. HH kutusu
 * her zaman 24-saat değerini gösterir (örn. "22"). */
static lv_obj_t* s_dial_hour_am = NULL; /* 4 sayı: 00,03,06,09 (12-hr cycle) */
static lv_obj_t* s_dial_hour_pm = NULL; /* 4 sayı: 12,15,18,21 (12-hr cycle) */
static lv_obj_t* s_dial_minute  = NULL; /* 12 sayılı (00..55) minute dial container */
static lv_obj_t* s_pos_marker   = NULL; /* aktif değerin açısal pozisyon turuncu noktası */
static lv_obj_t* s_unit_label   = NULL; /* legacy — kullanılmıyor, NULL kalır */

static lv_obj_t* s_dow_chips[7] = {};   /* Pazar..Cumartesi */

/* Dial geometrisi (screen-space). Dial container 300×300, alanı:
 *   x: 55..355, y: 60..360. Center (205, 210).
 * Number labels radius 108, position marker radius 135.
 *
 * Yeniden tasarım v3 (Türk saati / analog clock layout):
 *   - Saat dial'ı 12-saat döngüsü (4 sayı, 90° aralık) — AM/PM iki ayrı dial
 *     AM:  00 top, 03 right, 06 bottom, 09 left
 *     PM:  12 top, 15 right, 18 bottom, 21 left
 *     Marker math 12-saat cycle (hour % 12) * 30° → marker analog saat ibresi
 *     gibi davranır.
 *   - Dial center y=200 → 210 (10 px aşağı): marker tepeye geldiğinde header
 *     ("Cancel/New Alarm/Save") ile çakışmasın diye yer açıldı.
 *   - DIAL_LABEL_R 115 → 108: yan label'lar (03/15 right, 09/21 left) HH/MM
 *     kutularına çok yapışmasın (kutu ±48 x_off, label r=108 → gap ~4 px).
 *   - DIAL_MARKER_R 148 → 135: tepe marker y_top=66, header end y=57 → 9 px
 *     gap (eski 4 px overlap'tan kurtuldu).
 *   - HH/MM kutuları y=178 (mid 210), colon y=194. Dakika dial 12 sayılı
 *     (00,05,...,55) — değişmedi. */
#ifdef BOARD_VARIANT_LCD128
#define DIAL_CX 120
#else
#define DIAL_CX        205
#endif
#ifdef BOARD_VARIANT_LCD128
#define DIAL_CY 112   /* Session 40: dial 164→140, merkez yukarı — altta gün chip'lerine yer */
#else
#define DIAL_CY        210
#endif
/* Geometri kısıtları (yan label "03"/"09" / "15"/"21" ile HH/MM kutuları
 * arasında yeterli gap olması gerekiyor):
 *   Label "03" merkez x = 205 + DIAL_LABEL_R, label genişliği ~28 px (font 22).
 *   MM kutusu sağ kenarı = 205 + x_off + box_w/2.
 *   Gap = DIAL_LABEL_R - 14 - (x_off + box_w/2).
 * R=115, box w=78 x_off=44 → gap = 115-14-(44+39) = 18 px. Net görünür gap.
 *
 * Eski R=108 + box w=86 x_off=48 kombinasyonunda gap = 108-14-(48+43) = 3 px;
 * label boxes ile çakışıyordu ve AM/PM swap sırasında stale framebuffer
 * artığı (LVGL partial redraw region overlap) "bozuk tasarım" görüntüsü
 * yaratıyordu (kullanıcı 12→00 geçişinde resimle bildirdi). */
#ifdef BOARD_VARIANT_LCD128
#define DIAL_LABEL_R 50   /* Session 42: etiketler kutulardan ve marker'dan ayrıldı */
#else
#define DIAL_LABEL_R   115
#endif
#ifdef BOARD_VARIANT_LCD128
#define DIAL_MARKER_R 70   /* Session 42: dial halkasının (r=70) tam üstünde */
#else
#define DIAL_MARKER_R  140
#endif    /* tepe marker y_top=70, header end y=57 → 13 px gap */
#ifdef BOARD_VARIANT_LCD128
/* Session 40: bu ikisi AMOLED değeriydi — 240'ta dial yarıçapı 70 iken
 * INNER_DEAD 60 yalnız 10 px'lik bir halkayı dokunulabilir bırakıyordu
 * (dial'a dokununca çoğu zaman hiçbir şey olmuyordu). */
#define DIAL_INNER_DEAD 28    /* kutuların (±21, 40 px) dışı */
#define DIAL_OUTER_MAX  80
#else
#define DIAL_INNER_DEAD 60    /* tap merkeze < 60 px ise yoksay (boxes alanı) */
#define DIAL_OUTER_MAX 148    /* tap > 148 px yoksay (marker R=140'ın 8 px dışı) */
#endif

/* ── Statik stiller (LVGL 9.2 default-tema baypasları) ─────────────────────
 * `lv_obj_remove_style_all` + ardışık `lv_obj_set_style_bg_color(...,STATE)`
 * lokal override'ları LVGL 9.2'nin LV_PALETTE_BLUE tabanlı dark-tema'sının
 * LV_STATE_PRESSED/FOCUSED renklerini ezmiyor — kullanıcı dijit kutusuna
 * dokununca arkaplanı mavi gradient'le yanıp sönüyor. Çözüm: container'lara
 * lokal stil ekleme yerine static `lv_style_t` örneğini `lv_obj_add_style()`
 * ile çakıyoruz. Static style, theme apply callback'i ile eklenen stillerden
 * SONRA add edildiği için stil zincirinde theme'den daha yüksek önceliğe
 * sahip olur ve PRESSED/FOCUSED state geçişlerinde de geçerli kalır.
 *
 * Üç stil:
 *   s_dark_box_st     → 130×110 dijit kutusu (#12121A)
 *   s_dark_chip_st    → 36×36 pasif gün chip'i (#2C2C36)
 *   s_orange_chip_st  → 36×36 aktif gün chip'i (#FF9F0A)
 */
static lv_style_t s_dark_box_st;
static lv_style_t s_dark_chip_st;
static lv_style_t s_orange_chip_st;
static bool       s_styles_inited = false;

static void init_alarm_styles_once(void)
{
    if (s_styles_inited) return;
    s_styles_inited = true;

    /* Tüm stillerde ortak property: gradient'i sıfırla, border/outline/shadow
     * yok — theme'in pressed-overlay'i bu üçünden birini kullanır. */
    lv_style_init(&s_dark_box_st);
    lv_style_set_bg_color    (&s_dark_box_st, lv_color_make(0x12, 0x12, 0x1A));
    lv_style_set_bg_grad_color(&s_dark_box_st, lv_color_make(0x12, 0x12, 0x1A));
    lv_style_set_bg_grad_dir (&s_dark_box_st, LV_GRAD_DIR_NONE);
    lv_style_set_bg_opa      (&s_dark_box_st, LV_OPA_COVER);
    lv_style_set_border_width(&s_dark_box_st, 0);
    lv_style_set_outline_width(&s_dark_box_st, 0);
    lv_style_set_shadow_width(&s_dark_box_st, 0);
    lv_style_set_radius      (&s_dark_box_st, 22);
    lv_style_set_pad_all     (&s_dark_box_st, 0);
    /* Tema veya base-class PRESSED-feedback transform sızıntısını kes:
     * scale=256 (1×) ve translate=0 → hiçbir state'te kutu yer değiştirmez. */
    lv_style_set_transform_scale_x(&s_dark_box_st, 256);
    lv_style_set_transform_scale_y(&s_dark_box_st, 256);
    lv_style_set_translate_x      (&s_dark_box_st, 0);
    lv_style_set_translate_y      (&s_dark_box_st, 0);

    lv_style_init(&s_dark_chip_st);
    lv_style_set_bg_color    (&s_dark_chip_st, lv_color_make(0x2C, 0x2C, 0x36));
    lv_style_set_bg_grad_color(&s_dark_chip_st, lv_color_make(0x2C, 0x2C, 0x36));
    lv_style_set_bg_grad_dir (&s_dark_chip_st, LV_GRAD_DIR_NONE);
    lv_style_set_bg_opa      (&s_dark_chip_st, LV_OPA_COVER);
    lv_style_set_border_width(&s_dark_chip_st, 0);
    lv_style_set_outline_width(&s_dark_chip_st, 0);
    lv_style_set_shadow_width(&s_dark_chip_st, 0);
    lv_style_set_radius      (&s_dark_chip_st, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all     (&s_dark_chip_st, 0);
    /* Theme PRESSED-feedback transform sızıntısını kes (s_dark_box_st ile
     * aynı pattern): aksi halde chip'e dokununca theme bazen
     * transform_scale_y'i ~250'ye düşürüyor → içindeki "S/M/T/W/T/F/S"
     * harfi bir kaç pixel kayıyor ("gün'ün harfinde kayma" raporu). */
    lv_style_set_transform_scale_x(&s_dark_chip_st, 256);
    lv_style_set_transform_scale_y(&s_dark_chip_st, 256);
    lv_style_set_translate_x      (&s_dark_chip_st, 0);
    lv_style_set_translate_y      (&s_dark_chip_st, 0);

    lv_style_init(&s_orange_chip_st);
    lv_style_set_bg_color    (&s_orange_chip_st, lv_color_make(0xFF, 0x9F, 0x0A));
    lv_style_set_bg_grad_color(&s_orange_chip_st, lv_color_make(0xFF, 0x9F, 0x0A));
    lv_style_set_bg_grad_dir (&s_orange_chip_st, LV_GRAD_DIR_NONE);
    lv_style_set_bg_opa      (&s_orange_chip_st, LV_OPA_COVER);
    lv_style_set_border_width(&s_orange_chip_st, 0);
    lv_style_set_outline_width(&s_orange_chip_st, 0);
    lv_style_set_shadow_width(&s_orange_chip_st, 0);
    lv_style_set_radius      (&s_orange_chip_st, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all     (&s_orange_chip_st, 0);
    lv_style_set_transform_scale_x(&s_orange_chip_st, 256);
    lv_style_set_transform_scale_y(&s_orange_chip_st, 256);
    lv_style_set_translate_x      (&s_orange_chip_st, 0);
    lv_style_set_translate_y      (&s_orange_chip_st, 0);
}

/* Aynı static stili DEFAULT, PRESSED, FOCUSED, FOCUS_KEY için ayrı ayrı ekler.
 * Belt-and-suspenders: `lv_obj_remove_style_all` tema PRESSED-state stilini
 * tam temizlemese bile bizim PRESSED-selektörlü add'imiz ondan sonra
 * eklendiğinden stil zincirinde önceliği kapar. */
static void add_style_all_states(lv_obj_t* obj, lv_style_t* st)
{
    lv_obj_add_style(obj, st, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_style(obj, st, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_style(obj, st, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_style(obj, st, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
}

static void remove_style_all_states(lv_obj_t* obj, lv_style_t* st)
{
    lv_obj_remove_style(obj, st, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(obj, st, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_remove_style(obj, st, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_remove_style(obj, st, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
}

/* Ringing görünümü state */
static int          s_ring_idx       = -1;
static lv_obj_t*    s_ring_bell      = NULL;
static lv_obj_t*    s_ring_time_lbl  = NULL;
static lv_obj_t*    s_ring_label_lbl = NULL;
static lv_timer_t*  s_ring_anim_timer = NULL;
static lv_timer_t*  s_ring_clock_timer = NULL;
static uint8_t      s_brightness_saved = 255;

/* Forward decls */
static void show_list(void);
static void show_edit(int idx);
static void show_ringing(int idx);
static void cleanup_ringing(void);

/* ─── Yardımcılar ────────────────────────────────────────────────────────── */

static void destroy_view(lv_obj_t** view_ptr)
{
    if (view_ptr && *view_ptr) {
        lv_obj_delete(*view_ptr);
        *view_ptr = NULL;
    }
}

static void destroy_all_views(void)
{
    cleanup_ringing();
    destroy_view(&s_view_list);
    destroy_view(&s_view_edit);
    destroy_view(&s_view_ring);
}

/* "Mon Wed Fri" gibi kısa özet metni üretir. dow=0 → "Once"; 0x7F → "Daily" */
static void format_dow_summary(uint8_t dow, char* out, size_t n)
{
    if (dow == 0)            { snprintf(out, n, "Once");      return; }
    if (dow == 0x7F)         { snprintf(out, n, "Every day"); return; }
    if (dow == ALARM_DOW_WEEKDAYS) { snprintf(out, n, "Weekdays"); return; }
    if (dow == ALARM_DOW_WEEKENDS) { snprintf(out, n, "Weekend"); return; }
    static const char* names[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    out[0] = '\0';
    for (int i = 0; i < 7; i++) {
        if (!((dow >> i) & 1u)) continue;
        if (out[0]) strncat(out, " ", n - strlen(out) - 1);
        strncat(out, names[i], n - strlen(out) - 1);
    }
}

/* Bir alarm satırı için "ON" göstergesi rengi (enabled / pasif) */
static lv_color_t toggle_color(bool en) {
    return en ? lv_color_make(0x4F, 0xE0, 0x9D) : lv_color_make(0x44, 0x44, 0x44);
}

/* ─── List view ──────────────────────────────────────────────────────────── */

static void list_toggle_cb(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t* sw = (lv_obj_t*)lv_event_get_target(e);
    bool en = lv_obj_has_state(sw, LV_STATE_CHECKED);
    alarm_service_set_enabled((uint8_t)idx, en);
}

static void list_row_click_cb(lv_event_t* e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    show_edit(idx);
}

static void list_add_click_cb(lv_event_t* e)
{
    (void)e;
    int idx = alarm_service_find_free_slot();
    if (idx < 0) {
        ESP_LOGW(TAG, "Tüm slotlar dolu");
        return;
    }
    show_edit(idx);   /* yeni boş alarm için edit */
}

static void list_view_gesture_cb(lv_event_t* e)
{
    (void)e;
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        app_manager_back();
    }
}

/* "Back" etiketi click handler'ı — daha önce yalnızca pasif görsel ipucuydu
 * (kullanıcı tıkladığında cevap vermiyordu). Artık gerçek tıklanabilir buton. */
static void back_hint_clicked_cb(lv_event_t* e)
{
    (void)e;
    app_manager_back();
}

static void show_list(void)
{
    destroy_all_views();
    s_current_view = VIEW_LIST;

    s_view_list = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_view_list);
    lv_obj_set_size(s_view_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_view_list, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_view_list, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_view_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_view_list, LV_OBJ_FLAG_SCROLLABLE);

    /* Üst bar: Geri ipucu + başlık. Yuvarlak köşe maskesinden kaçınmak için
     * y=32 ve x_inset 50 kullanıyoruz.
     * "Back" artık tıklanabilir — kullanıcı dokununca app_manager_back().
     * Hit area genişletildi (etrafa pad) ki etiket küçük olsa bile parmakla
     * isabet ettirmek kolay olsun. */
    lv_obj_t* back = lv_label_create(s_view_list);
    lv_obj_set_style_text_color(back, lv_color_make(0x4A, 0xC0, 0xFF), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(back, &lv_font_montserrat_16, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 56, 22);   /* Session 40: (50,18) köşesi bezel altındaydı */
    lv_label_set_text(back, LV_SYMBOL_LEFT);   /* dar ekranda sadece ok */
#else
    lv_obj_set_style_text_font(back, &lv_font_montserrat_20, 0);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 50, 32);
    lv_label_set_text(back, LV_SYMBOL_LEFT " Back");
#endif
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    /* v1.4.64: 20 → 28 (Apple HIG min 44×44 touch target) */
    lv_obj_set_ext_click_area(back, 28);
    lv_obj_add_event_cb(back, back_hint_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* title = lv_label_create(s_view_list);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);
#else
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
#endif
    lv_label_set_text(title, "Alarms");

    /* Kaydırılabilir içerik alanı */
    lv_obj_t* content = lv_obj_create(s_view_list);
    lv_obj_remove_style_all(content);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_pos(content, SAFE_X, 44);
    lv_obj_set_size(content, SAFE_W, 136);
#else
    lv_obj_set_pos(content, SAFE_X, 72);
    lv_obj_set_size(content, SAFE_W, 398);
#endif
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                                   LV_FLEX_ALIGN_CENTER,
                                   LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, 8, 0);
    lv_obj_set_style_pad_top(content, 8, 0);
    lv_obj_set_style_pad_bottom(content, 16, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);

    bool any = false;
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        alarm_t a;
        if (!alarm_service_get((uint8_t)i, &a)) continue;
        any = true;

        lv_obj_t* row = lv_obj_create(content);
        lv_obj_remove_style_all(row);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_size(row, SAFE_W - 4, 56);
#else
        lv_obj_set_size(row, SAFE_W - 4, 78);
#endif
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_set_style_bg_color(row, lv_color_make(0x16, 0x16, 0x1F), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(row, 14, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, list_row_click_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)i);

        /* Saat (büyük) */
        lv_obj_t* tlbl = lv_label_create(row);
        lv_obj_set_style_text_color(tlbl,
            a.enabled ? lv_color_white() : lv_color_make(0x55, 0x55, 0x60), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_22, 0);
#else
        lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_40, 0);
#endif
        lv_obj_align(tlbl, LV_ALIGN_LEFT_MID, 0, -8);
        lv_label_set_text_fmt(tlbl, "%02u:%02u", a.hour, a.minute);

        /* Gün/etiket özeti (alt) */
        char dows[40]; format_dow_summary(a.dow_mask, dows, sizeof(dows));
        char sub[80];
        if (a.label[0]) snprintf(sub, sizeof(sub), "%s · %s", dows, a.label);
        else            snprintf(sub, sizeof(sub), "%s", dows);

        lv_obj_t* sublbl = lv_label_create(row);
        lv_obj_set_style_text_color(sublbl, lv_color_make(0x88, 0x88, 0x99), 0);
        lv_obj_set_style_text_font(sublbl, &lv_font_montserrat_14, 0);
        lv_obj_align(sublbl, LV_ALIGN_BOTTOM_LEFT, 0, -2);
        lv_label_set_text(sublbl, sub);

        /* Toggle (sağ) */
        lv_obj_t* sw = lv_switch_create(row);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_size(sw, 44, 24);
#else
        lv_obj_set_size(sw, 56, 30);
#endif
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(sw, lv_color_make(0x33, 0x33, 0x33), 0);
        lv_obj_set_style_bg_color(sw, toggle_color(true), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (a.enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, list_toggle_cb, LV_EVENT_VALUE_CHANGED,
                            (void*)(intptr_t)i);
    }

    if (!any) {
        lv_obj_t* empty = lv_label_create(content);
        lv_obj_set_style_text_color(empty, lv_color_make(0x55, 0x55, 0x66), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_label_set_text(empty,
            "No alarms yet.\nTap " LV_SYMBOL_PLUS " below to add one.");
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* Alt: "+" ekle butonu (eğer slot varsa)
     *
     * lv_button_create kullanmıyoruz — LVGL theme'i kontrol etmediğimiz
     * transform/scale/anim/shadow/border styles uyguluyor → tıklanınca
     * "ikiye bölünüyor" görsel artifact (eski + yeni bbox aynı anda
     * görünüyor). Theme'i pinning ile zaplamaya çalıştık, başka property
     * sızıyordu. Çözüm: lv_obj_create + tüm stilleri kendimiz set →
     * theme hooks tamamen baypas. Davranış aynı (clickable, click event),
     * sadece geometrisi sabit. */
    if (alarm_service_find_free_slot() >= 0) {
        /* "+" butonu — minimum stil, hiç animasyon yok, AMOLED corner mask'tan
         * uzakta (-50 offset). Press feedback YOK — kullanıcı raporu: opacity
         * anim de "ikiye bölünüyor" gibi görünüyordu. Bu noktada en güvenli
         * test: tamamen statik buton, hiçbir state değişikliği yok.
         * Eğer hâlâ split görünüyorsa sorun LVGL render path veya AMOLED
         * panelinde (driver veya hardware), uygulama kodunda değil. */
        lv_obj_t* add = lv_obj_create(s_view_list);
        lv_obj_remove_style_all(add);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_size(add, 46, 46);
#else
        lv_obj_set_size(add, 64, 64);
#endif
        /* Bottom edge'den 50 px yukarı: AMOLED rounded corner mask'tan kesin
         * uzak (corner radius tipik ~40-50 px). Önceki -18 corner'a yakındı. */
#ifdef BOARD_VARIANT_LCD128
        lv_obj_align(add, LV_ALIGN_BOTTOM_MID, 0, -12);
#else
        lv_obj_align(add, LV_ALIGN_BOTTOM_MID, 0, -50);
#endif
        lv_obj_set_style_radius(add, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(add, lv_color_make(0xFF, 0x9F, 0x0A), 0);
        lv_obj_set_style_bg_opa(add, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(add, 0, 0);
        lv_obj_set_style_outline_width(add, 0, 0);
        lv_obj_set_style_shadow_width(add, 0, 0);
        lv_obj_set_style_pad_all(add, 0, 0);
        lv_obj_clear_flag(add, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(add, list_add_click_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t* plus = lv_label_create(add);
        lv_obj_set_style_text_color(plus, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_style_text_font(plus, &lv_font_montserrat_20, 0);
#else
        lv_obj_set_style_text_font(plus, &lv_font_montserrat_28, 0);
#endif
        lv_obj_center(plus);
        lv_label_set_text(plus, LV_SYMBOL_PLUS);
        lv_obj_add_flag(plus, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    /* Sağ swipe → geri */
    app_manager_enable_gestures(s_view_list);
    lv_obj_add_event_cb(s_view_list, list_view_gesture_cb, LV_EVENT_GESTURE, NULL);
}

/* ─── Edit view ──────────────────────────────────────────────────────────── */

static void edit_dow_chip_click_cb(lv_event_t* e)
{
    int day = (int)(intptr_t)lv_event_get_user_data(e);
    s_edit_buf.dow_mask ^= (uint8_t)(1u << day);

    lv_obj_t* chip = (lv_obj_t*)lv_event_get_target(e);
    bool on = (s_edit_buf.dow_mask >> day) & 1u;

    /* Static stilleri swap et — 4 state için tek tek (belt-and-suspenders).
     * Per-state lokal bg-override'lar theme'in pressed/focused mavi'sini
     * kesmediği için artık `lv_style_t` ile çakıyoruz. */
    if (on) {
        remove_style_all_states(chip, &s_dark_chip_st);
        add_style_all_states   (chip, &s_orange_chip_st);
    } else {
        remove_style_all_states(chip, &s_orange_chip_st);
        add_style_all_states   (chip, &s_dark_chip_st);
    }
    lv_obj_invalidate(chip);

    lv_obj_t* lbl = lv_obj_get_child(chip, 0);
    if (lbl) {
        lv_obj_set_style_text_color(lbl,
            on ? lv_color_white() : lv_color_make(0x99, 0x99, 0xAA), 0);
    }
}

static void edit_save_cb(lv_event_t* e)
{
    (void)e;
    /* drag handler s_edit_buf.hour/minute alanlarını canlı günceller; burada
     * sadece flag set + persist. */
    s_edit_buf.enabled = true;
    alarm_service_set((uint8_t)s_edit_idx, &s_edit_buf);
    show_list();
}

/* ─── Dial helper'ları ────────────────────────────────────────────────────
 * HH/MM kutuları SABİT — sadece text değişir, container hareket etmez.
 * Dial 12 sayı içerir (saat: 00,02,...,22; dakika: 00,05,...,55) açısal
 * olarak çember üstünde. Kullanıcı dial'a dokununca pozisyonun açısı (atan2)
 * → değer dönüşümü ile aktif kutuya yazılır. Position marker (turuncu
 * nokta) mevcut değerin açısal yerinde durur. */

/* HH ve MM kutu içeriklerini canlı senkronize et. */
static void update_box_labels(void)
{
    if (s_lbl_hour)   lv_label_set_text_fmt(s_lbl_hour,   "%02d", s_edit_buf.hour);
    if (s_lbl_minute) lv_label_set_text_fmt(s_lbl_minute, "%02d", s_edit_buf.minute);
}

/* Aktif kutu yeşil, pasif gri border. */
static void update_box_borders(void)
{
    if (!s_box_hour || !s_box_minute) return;
    lv_color_t green = lv_color_make(0x4F, 0xE0, 0x9D);
    lv_color_t gray  = lv_color_make(0xCC, 0xCC, 0xD0);
    lv_obj_set_style_border_color(s_box_hour,
        s_edit_mode == 0 ? green : gray, 0);
    lv_obj_set_style_border_color(s_box_minute,
        s_edit_mode == 1 ? green : gray, 0);
}

/* Position marker (turuncu nokta) — mevcut değerin açısal pozisyonuna kayar.
 * Açı: 0=tepe (yukarı), saat yönünde artar.
 *   Saat modu: 12-saat cycle → angle = (hour % 12) * 30°
 *              hour=0 veya 12 → tepe (00°), hour=22 → 300° (upper-left)
 *   Dakika modu: 60-min cycle → angle = minute * 6°
 *              minute=0 → tepe, minute=15 → 90°(right), 30→bottom, 45→left
 * Marker s_view_edit child'ı, screen-space mutlak pozisyon. */
static void update_pos_marker(void)
{
    if (!s_pos_marker) return;
    float ang_deg;
    if (s_edit_mode == 0) {
        /* 12-saat cycle: hour % 12 → 0..11 dial pozisyonu (her saat = 30°).
         * 24-saat HH değeri kutuda gösterilir; dial sadece cycle pozisyonunu
         * görselleştirir. PM hour 22 → 22%12=10 → 300°. */
        int v = (int)s_edit_buf.hour % 12;
        ang_deg = (float)v * 30.0f;
    } else {
        ang_deg = (float)s_edit_buf.minute * 6.0f;
    }
    float ang_rad = (ang_deg - 90.0f) * 3.14159265f / 180.0f;
    int x = DIAL_CX + (int)(DIAL_MARKER_R * cosf(ang_rad));
    int y = DIAL_CY + (int)(DIAL_MARKER_R * sinf(ang_rad));
    int w = lv_obj_get_width(s_pos_marker);
    int h = lv_obj_get_height(s_pos_marker);
#ifdef BOARD_VARIANT_LCD128
    /* Session 42: yerleşim henüz hesaplanmamışken 18'e düşüyordu → 10 px nokta
     * 4 px sol-üste kayıyordu (halkanın dışında değil, içinde duruyordu). */
    if (w <= 0) w = 10;
    if (h <= 0) h = 10;
#else
    if (w <= 0) w = 18;
    if (h <= 0) h = 18;
#endif

    /* STALE FB FIX — marker "çift nokta" artifact (kullanıcı raporu 2026-05-04):
     * Önceki "old position invalidate + 4 px margin" yaklaşımı yetersiz —
     * kullanıcı hâlâ görüyor. Artık MARKER ETRAFINDA TÜM DIAL HALKASI
     * invalidate ediliyor (300×300 dial bbox). Cost düşük (marker burst
     * hareket eder, sürekli değil). LVGL accumulated dirty area'yı tek
     * frame'de temizler → eski konum + dial perimeter + yeni konum hepsi
     * tek seferde redraw → ghost pixel kalmaz garanti. */
    lv_obj_t* parent = lv_obj_get_parent(s_pos_marker);
    if (parent) {
        /* Dial alanı ekran-x: 55..355, ekran-y: 60..360 (300×300 box).
         * Marker bu alanın çevresinde dolaşıyor; tüm bbox'ı invalidate
         * etmek hem eski hem yeni konumu garanti redraw eder. */
        lv_area_t dial_a;
        dial_a.x1 = DIAL_CX - 160;   /* 205 - 160 = 45 (sol kenar + biraz pay) */
        dial_a.y1 = DIAL_CY - 160;   /* 210 - 160 = 50 */
        dial_a.x2 = DIAL_CX + 160;   /* 365 */
        dial_a.y2 = DIAL_CY + 160;   /* 370 */
        lv_obj_invalidate_area(parent, &dial_a);
    }

    lv_obj_set_pos(s_pos_marker, x - w/2, y - h/2);
}

/* Hour dial AM/PM swap — hour değerine göre uygun dial'ı göster, diğerini gizle.
 * BTN_TOP increment 11→12 veya 23→00 sınırı geçtiğinde, ya da dial tap mevcut
 * cycle dışı bir hour seçtiğinde çağrılır. Sadece HH editing modunda etkili.
 *
 * NUKE-INVALIDATE: yalnızca HIDDEN flag toggle'ı LVGL'nin partial-redraw
 * scheduler'ını her zaman uyandırmıyor (özellikle aynı 300×300 bbox içinde
 * iki dial üst üste durduğunda) — sonuç: AM/PM swap sonrası stale
 * framebuffer artığı (kullanıcının "bombozuk tasarım" raporu, 2026-04-30).
 * Bunu önlemek için her dial'ı + yan widget'ları (kutular, colon, marker)
 * explicit invalidate ediyoruz; bir sonraki render frame'inde TÜM bbox
 * yeniden çizilir, stale piksel kalmaz. */
static void update_hour_dial_visibility(void)
{
    if (s_edit_mode != 0) return;   /* dakika modunda hour dial'ları zaten gizli */
    bool is_pm = (s_edit_buf.hour >= 12);
    if (s_dial_hour_am) {
        if (is_pm) lv_obj_add_flag  (s_dial_hour_am, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_clear_flag(s_dial_hour_am, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_dial_hour_pm) {
        if (is_pm) lv_obj_clear_flag(s_dial_hour_pm, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag  (s_dial_hour_pm, LV_OBJ_FLAG_HIDDEN);
    }
    /* Tüm s_view_edit'i invalidate et — LVGL partial-redraw scheduler iki
     * dial'ın aynı bbox'ında "biri görünüyordu, şimdi diğeri görünüyor"
     * geçişini her zaman doğru tespit etmiyor. Full view invalidate
     * ile bir sonraki frame'de TÜM widget'lar (dial/box/colon/marker/chips)
     * yeniden çizilir — stale piksel kalmaz. Maliyet: 1 frame full redraw
     * (~4ms 410×502 SPI'da), kullanıcının "bombozuk tasarım" raporu çözülür. */
    if (s_view_edit) lv_obj_invalidate(s_view_edit);
}

/* HH ↔ MM modu değiştir; uygun dial'ı göster, diğerini gizle.
 * NOT: mode label ("24 SA./DAKIKA") tasarımdan kaldırıldı — dial üst sayısı
 * "00" ile çakışıyordu ve aktif kutunun yeşil border'ı zaten hangi modun
 * düzenlendiğini açıkça gösteriyor. s_unit_label = NULL kalır.
 *
 * HH modunda iki dial var (AM ve PM); update_hour_dial_visibility() mevcut
 * 24-saat değerine göre hangisini göstereceğine karar verir. MM modunda
 * her iki hour dial'ı da gizlenir, minute dial görünür. */
static void switch_mode(int mode)
{
    if (mode == s_edit_mode) return;
    s_edit_mode = mode;
    if (mode == 0) {
        /* HH editing → AM veya PM dial (hour değerine göre), minute gizli.
         * update_hour_dial_visibility() içinde s_view_edit invalidate var. */
        if (s_dial_minute) lv_obj_add_flag(s_dial_minute, LV_OBJ_FLAG_HIDDEN);
        update_hour_dial_visibility();
    } else {
        /* MM editing → her iki hour dial gizli, minute görünür.
         * Full view invalidate gerekli: hour↔minute swap eski hour label'ları
         * (00/12,03/15,06/18,09/21) → yeni minute label'larıyla (00/05/10/.../55)
         * üst üste binebilir; LVGL partial-redraw'ı stale pikselleri her
         * zaman temizlemiyor. */
        if (s_dial_hour_am) lv_obj_add_flag  (s_dial_hour_am, LV_OBJ_FLAG_HIDDEN);
        if (s_dial_hour_pm) lv_obj_add_flag  (s_dial_hour_pm, LV_OBJ_FLAG_HIDDEN);
        if (s_dial_minute)  lv_obj_clear_flag(s_dial_minute,  LV_OBJ_FLAG_HIDDEN);
        if (s_view_edit)    lv_obj_invalidate(s_view_edit);
    }
    update_box_borders();
    update_pos_marker();
}

static void box_hour_click_cb  (lv_event_t* e) { (void)e; switch_mode(0); }
static void box_minute_click_cb(lv_event_t* e) { (void)e; switch_mode(1); }

/* Dial dokunma handler'ı (PRESSED + PRESSING).
 *   1) Dokunma noktasının dial-merkezine olan açısını hesapla (atan2).
 *   2) Açıyı 0..max'e map et (saat: 24, dakika: 60).
 *   3) Aktif kutuya yaz, marker'ı oraya taşı.
 * Kayma imkansız: kutu HER ZAMAN aynı pozisyonda, sadece text değişir
 * (90×60 px tek bir invalidate region). */
static void dial_input_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;

    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);

    int dx = p.x - DIAL_CX;
    int dy = p.y - DIAL_CY;
    int dist_sq = dx * dx + dy * dy;
    /* Çok merkeze yakın (boxes alanı) ya da çok dışarı: yoksay. */
    if (dist_sq < DIAL_INNER_DEAD * DIAL_INNER_DEAD) return;
    if (dist_sq > DIAL_OUTER_MAX  * DIAL_OUTER_MAX) return;

    /* atan2(x, -y): tepede 0, saat yönünde artar (pozitif x = sağ, ekran-y
     * aşağı doğru pozitif olduğundan -y = "yukarı pozitif"). */
    float ang_rad = atan2f((float)dx, -(float)dy);
    if (ang_rad < 0.0f) ang_rad += 2.0f * 3.14159265f;
    float ang_deg = ang_rad * 180.0f / 3.14159265f;

    /* HH modu: 12-saat cycle (0..11), açıdan değer çıkar.
     * Mevcut hour AM cycle'daysa (<12) 0..11 doğrudan; PM cycle'daysa (>=12)
     * 12 offset ekle → 12..23. Tap mevcut cycle'ı korur (AM→AM, PM→PM); cycle
     * değişimi yalnızca BTN_TOP ile sınır geçişi sırasında olur (11→12, 23→0).
     * MM modu: 60-min cycle, 0..59. */
    int max = (s_edit_mode == 0) ? 12 : 60;
    int val = (int)((ang_deg * (float)max / 360.0f) + 0.5f);
    if (val >= max) val -= max;

    if (s_edit_mode == 0) {
        bool is_pm = (s_edit_buf.hour >= 12);
        int new_hour = val + (is_pm ? 12 : 0);
        if (new_hour != s_edit_buf.hour) {
            s_edit_buf.hour = (uint8_t)new_hour;
            update_box_labels();
            update_pos_marker();
        }
    } else {
        if (val != s_edit_buf.minute) {
            s_edit_buf.minute = (uint8_t)val;
            update_box_labels();
            update_pos_marker();
        }
    }
}

static void edit_cancel_cb(lv_event_t* e)
{
    (void)e;
    show_list();
}

static void edit_delete_cb(lv_event_t* e)
{
    (void)e;
    if (s_edit_idx >= 0 && alarm_service_slot_used((uint8_t)s_edit_idx)) {
        alarm_service_remove((uint8_t)s_edit_idx);
    }
    show_list();
}

static void edit_view_gesture_cb(lv_event_t* e)
{
    (void)e;
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        show_list();
    }
}

static void show_edit(int idx)
{
    init_alarm_styles_once();
    destroy_all_views();
    s_current_view = VIEW_EDIT;
    s_edit_idx  = idx;
    s_edit_mode = 0;   /* her giriş HH editing ile başlar */

    /* Mevcut alarmı yükle ya da varsayılanları kur */
    if (idx >= 0 && alarm_service_get((uint8_t)idx, &s_edit_buf)) {
        /* mevcut */
    } else {
        memset(&s_edit_buf, 0, sizeof(s_edit_buf));
        time_t now; time(&now);
        struct tm ti; localtime_r(&now, &ti);
        s_edit_buf.hour     = (uint8_t)((ti.tm_hour + 1) % 24);
        s_edit_buf.minute   = 0;
        s_edit_buf.dow_mask = 0;     /* tek seferlik */
        s_edit_buf.enabled  = true;
    }

    /* ── NUKE: temayı geçici olarak NULL yap ───────────────────────────────
     * İki iterasyon (per-state lokal override + 4-state static add) LVGL 9.2
     * default-tema'sının (LV_PALETTE_BLUE) PRESSED-state mavisini ve PRESSED
     * transform'unu (rakamların sürükleme sırasında "kayma" görüntüsü)
     * temizleyemedi. Resim kanıtlıyor: hiç dokunulmamış chip'ler bile mavi
     * — yani DEFAULT state'te de tema sızıyor. Tek garantili çözüm: edit
     * view'in TÜM widget'larını yaratırken display'in tema'sını NULL yap.
     * Theme NULL iken `lv_obj_create` hiç tema stili apply etmez. Çıkışta
     * eski tema'yı geri ver — başka uygulamalar etkilenmez. */
    lv_display_t* disp = lv_display_get_default();
    lv_theme_t*   saved_theme = disp ? lv_display_get_theme(disp) : NULL;
    if (disp) lv_display_set_theme(disp, NULL);

    s_view_edit = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_view_edit);
    lv_obj_set_size(s_view_edit, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_view_edit, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_view_edit, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_view_edit, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_view_edit, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Üst bar ──────────────────────────────────────────────────────────
     * AMOLED yuvarlak köşe maskesi ~r=120; y=30'da kullanılabilir başlangıç
     * x≈45 civarı. x_inset 50 px ve y=32 ile metinler tamamen görünür.
     * Cancel/Save için arka plan dolgusu yok; sadece etiket dokunulabilir. */
    lv_obj_t* cancel = lv_label_create(s_view_edit);
    lv_obj_set_style_text_color(cancel, lv_color_make(0x4A, 0xC0, 0xFF), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(cancel, &lv_font_montserrat_16, 0);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 56, 16);   /* Session 42: tepe marker'a yer (16..33) */
    lv_label_set_text(cancel, LV_SYMBOL_CLOSE);
#else
    lv_obj_set_style_text_font(cancel, &lv_font_montserrat_20, 0);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 50, 32);
    lv_label_set_text(cancel, "Cancel");
#endif
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    /* v1.4.64: 18 → 26 (orta seviye — title label'ı orta bölgede, çakışmamalı) */
    lv_obj_set_ext_click_area(cancel, 26);
    lv_obj_add_event_cb(cancel, edit_cancel_cb, LV_EVENT_CLICKED, NULL);
    wear_press_feedback_attach(cancel);

    lv_obj_t* save = lv_label_create(s_view_edit);
    lv_obj_set_style_text_color(save, lv_color_make(0xFF, 0x9F, 0x0A), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(save, &lv_font_montserrat_16, 0);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -56, 16);
    lv_label_set_text(save, LV_SYMBOL_OK);
#else
    lv_obj_set_style_text_font(save, &lv_font_montserrat_20, 0);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -50, 32);
    lv_label_set_text(save, "Save");
#endif
    lv_obj_add_flag(save, LV_OBJ_FLAG_CLICKABLE);
    /* v1.4.64: 18 → 26 (orta seviye — Cancel ile simetrik) */
    lv_obj_set_ext_click_area(save, 26);
    lv_obj_add_event_cb(save, edit_save_cb, LV_EVENT_CLICKED, NULL);
    wear_press_feedback_attach(save);

    lv_obj_t* title = lv_label_create(s_view_edit);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);
    lv_label_set_text(title,
#else
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 32);
    lv_label_set_text(title,
#endif
        alarm_service_slot_used((uint8_t)idx) ? "Edit Alarm" : "New Alarm");

    /* ── Apple Watch tarzı dial + iki sabit HH/MM kutusu ─────────────────
     * Roller/scroll mantığı tamamen atıldı — kayma fiziksel olarak imkansız:
     * dijit kutusu HER ZAMAN aynı pozisyonda, sadece text 90×60 px küçük
     * bölgede değişir. Değer girişi: çevredeki dial'a dokunma → açı (atan2)
     * → değer. Aktif kutu (yeşil border) hangisini düzenlediğini gösterir;
     * diğer kutuya tıklamak modu değiştirir.
     *
     * Layout:
     *
     *           "24 SA."   ← y=70  (saat modu)  /  "DAKIKA" (dakika modu)
     *
     *               00                          ← dial üst, y≈95
     *         22         02
     *       20             04                   ← dial labels radius 105
     *      18    ┌──┐:┌──┐    06
     *      16    │11│ │03│    08                ← HH/MM kutuları y=170-230
     *      14    └──┘ └──┘    10
     *       12            (turuncu nokta = aktif değerin pozisyonu)
     *
     *           Cancel  Save                    ← üst bar y=32
     *
     * Dial container 270×270 at LV_ALIGN_TOP_MID y=65 → screen center (205,200).
     * Number label radius 105 (içte), position marker radius 130 (dışta). */

    /* Dial container builder — analog saat tarzı yerleşim.
     *   max_val:  cycle uzunluğu (12 saat cycle veya 60 dakika cycle)
     *   label_cnt: dial üzerinde kaç sayı gösterilecek (saat:4, dakika:12)
     *   label_offset: gösterilen sayıya eklenen ofset (AM dial:0, PM dial:12)
     *
     * Saat AM dial: 4 sayı, 90° aralık → 00 (top), 03 (right), 06 (bottom),
     *               09 (left). Marker math (hour % 12) * 30° ile aynı açıya
     *               oturduğu için marker da analog saat ibresi gibi davranır.
     * Saat PM dial: 4 sayı, 90° aralık → 12 (top), 15, 18, 21. AM ile aynı
     *               açısal pozisyonlar; sadece etiket metni 12 ofsetlidir.
     * Dakika dial: 12 sayı, 30° aralık → 00, 05, ... 55.
     *
     * Tap mantığı: dial_input_cb angular coord → 0..(max_val-1) value. Saat
     * modunda mevcut cycle (AM/PM) korunur; tap PM cycle'da 5 numarasına
     * basarsa hour=17 olur (5+12), AM cycle'da hour=5. */
    auto build_dial = [](lv_obj_t* parent, int max_val, int label_cnt,
                         int label_offset) -> lv_obj_t* {
        lv_obj_t* dial = lv_obj_create(parent);
        lv_obj_remove_style_all(dial);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_size(dial, 140, 140);
        lv_obj_align(dial, LV_ALIGN_TOP_MID, 0, 42);   /* merkez y=112 = DIAL_CY */
        /* Session 42: görünür ince halka — sayılar boşlukta yüzüyordu, nereye
         * dokunulacağı belli değildi. Marker (R=70) tam bu halkanın üstünde. */
        lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dial, 1, 0);
        lv_obj_set_style_border_color(dial, lv_color_make(0x2E, 0x2E, 0x38), 0);
        lv_obj_set_style_border_opa(dial, LV_OPA_COVER, 0);
#else
        lv_obj_set_size(dial, 300, 300);
        lv_obj_align(dial, LV_ALIGN_TOP_MID, 0, 60);
#endif
        lv_obj_clear_flag(dial, LV_OBJ_FLAG_SCROLLABLE);
        /* Dial tüm yüzeyi clickable: dokunma → açı → değer. */
        lv_obj_add_flag(dial, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(dial, dial_input_cb, LV_EVENT_PRESSED,  NULL);
        lv_obj_add_event_cb(dial, dial_input_cb, LV_EVENT_PRESSING, NULL);

        const int step = max_val / label_cnt;   /* saat-cycle:3, dakika:5 */
        const float r  = (float)DIAL_LABEL_R;
        for (int i = 0; i < label_cnt; i++) {
            int v = i * step;            /* dial cycle pozisyonu (0..max_val-1) */
            int shown = v + label_offset;/* ekrandaki sayı (AM: v, PM: v+12) */
            lv_obj_t* lbl = lv_label_create(dial);
            lv_obj_set_style_text_color(lbl, lv_color_make(0xDD, 0xDD, 0xE3), 0);
#ifdef BOARD_VARIANT_LCD128
            lv_obj_set_style_text_font (lbl, &lv_font_montserrat_14, 0);
#else
            lv_obj_set_style_text_font (lbl, &lv_font_montserrat_22, 0);
#endif
            char buf[8]; snprintf(buf, sizeof(buf), "%02u", (unsigned)shown);
            lv_label_set_text(lbl, buf);
            float ang_rad = ((float)v * 360.0f / (float)max_val - 90.0f)
                            * 3.14159265f / 180.0f;
            int dx_pos = (int)(r * cosf(ang_rad));
            int dy_pos = (int)(r * sinf(ang_rad));
            lv_obj_align(lbl, LV_ALIGN_CENTER, dx_pos, dy_pos);
        }
        return dial;
    };

    /* Üç dial: AM saat (00/03/06/09), PM saat (12/15/18/21), dakika (00..55).
     * AM ve PM dial'ları aynı angular pozisyonları kullanır — sadece etiket
     * sayıları farklıdır. update_hour_dial_visibility() mevcut hour değerine
     * göre AM ya da PM dial'ı görünür yapar. Dakika dial başlangıçta gizli
     * (s_edit_mode=0 = HH editing).
     *
     * NOT: Dial align y_offset 50 → 60 (DIAL_CY=210 ile uyumlu, screen
     * y=210 = 60 + 150). */
    s_dial_hour_am = build_dial(s_view_edit, 12, 4, 0);
    s_dial_hour_pm = build_dial(s_view_edit, 12, 4, 12);
    s_dial_minute  = build_dial(s_view_edit, 60, 12, 0);
    lv_obj_add_flag(s_dial_minute, LV_OBJ_FLAG_HIDDEN);
    update_hour_dial_visibility();   /* hour değerine göre AM/PM birini göster */

    /* NOT: "24 SA./DAKIKA" mode label'ı kaldırıldı.
     * Önceki tasarımda bu label y=60'taydı ve dial üst sayısı "00" (y≈85
     * civarı) ile dikey olarak çakışıyordu. Aktif kutudaki yeşil border
     * zaten hangi modun (HH veya MM) düzenlendiğini net gösterdiği için
     * label redundant'tı. Stale dangling pointer'a karşı NULL'a sıfırla. */
    s_unit_label = NULL;

    /* HH/MM kutuları — SABİT pozisyon, asla scroll etmez. Aktif kutu yeşil
     * border, pasif gri. Kutuya tıklamak hangisini düzenleyeceğini değiştirir.
     * Her kutu 86×64 px, y=168..232 (dial merkezi y=200 etrafında simetrik).
     * Font 32 — dijit kutusu net okunur. */
    auto build_box = [](lv_obj_t* parent, int x_off,
                        lv_event_cb_t cb, lv_obj_t** out_lbl) -> lv_obj_t* {
        lv_obj_t* box = lv_obj_create(parent);
        lv_obj_remove_style_all(box);
        /* Box 78×60: yan dial label'larıyla (R=115) net gap için 86×64'ten
         * küçültüldü. "00" font 32 ≈ 36 px → padding 21 px her yan. */
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_size(box, 34, 28);   /* Session 42: 40→34, yan etiketlere değiyordu; iki nokta için 8 px boşluk */
#else
        lv_obj_set_size(box, 78, 60);
#endif
        /* y_offset 180: DIAL_CY=210, box height=60 → vertical center
         * 180+30=210 = DIAL_CY. Box y=180-240. */
#ifdef BOARD_VARIANT_LCD128
        lv_obj_align(box, LV_ALIGN_TOP_MID, (x_off * 21) / 44, 98);   /* ±21, merkez y=112 */
#else
        lv_obj_align(box, LV_ALIGN_TOP_MID, x_off, 180);
#endif
        lv_obj_set_style_bg_color(box, lv_color_make(0x16, 0x16, 0x1F), 0);
        lv_obj_set_style_bg_opa  (box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius  (box, 16, 0);
        lv_obj_set_style_border_width(box, 3, 0);
        /* border_color update_box_borders'da set edilir. */
        lv_obj_set_style_pad_all (box, 0, 0);
        /* Kayma kaynağı transform'ları kapat: PRESSED'te scale/translate sızıntısı yok. */
        lv_obj_set_style_transform_scale_x(box, 256, 0);
        lv_obj_set_style_transform_scale_y(box, 256, 0);
        lv_obj_set_style_translate_x      (box, 0,   0);
        lv_obj_set_style_translate_y      (box, 0,   0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag (box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(box, cb, LV_EVENT_CLICKED, NULL);

        *out_lbl = lv_label_create(box);
        lv_obj_set_style_text_color(*out_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_style_text_font (*out_lbl, &lv_font_montserrat_20, 0);
#else
        lv_obj_set_style_text_font (*out_lbl, &lv_font_montserrat_32, 0);
#endif
        lv_obj_set_style_text_align(*out_lbl, LV_TEXT_ALIGN_CENTER, 0);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_width           (*out_lbl, 34);
#else
        lv_obj_set_width           (*out_lbl, 78);
#endif
        lv_obj_align              (*out_lbl, LV_ALIGN_CENTER, 0, 0);
        return box;
    };

    /* x_off ±44: HH center x=161, MM center x=249. MM rect right=288.
     * Yan label "03" merkez x=320 (R=115), label genişliği ~28 → label rect
     * left=306. Gap MM-right→label-left = 18 px. Net görünür ayrım. */
    s_box_hour   = build_box(s_view_edit, -44, box_hour_click_cb,   &s_lbl_hour);
    s_box_minute = build_box(s_view_edit,  44, box_minute_click_cb, &s_lbl_minute);

    /* `:` ayracı — kutuların ortasında. Box top y=180, mid y=210, font 32
     * cap height ~24 → y=196 baseline'i kutu mid'e yakın. */
    lv_obj_t* colon = lv_label_create(s_view_edit);
    lv_obj_set_style_text_color(colon, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_20, 0);
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 101);
#else
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_32, 0);
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 196);
#endif
    lv_label_set_text(colon, ":");

    /* Position marker — turuncu nokta, dial perimeter'inde aktif değerin
     * açısal pozisyonunda durur. 14→18 büyütüldü, dial ile orantılı. */
    s_pos_marker = lv_obj_create(s_view_edit);
    lv_obj_remove_style_all(s_pos_marker);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_pos_marker, 10, 10);   /* Session 42: 12 → 10, "15" etiketine biniyordu */
#else
    lv_obj_set_size(s_pos_marker, 18, 18);
#endif
    lv_obj_set_style_bg_color(s_pos_marker, lv_color_make(0xFF, 0x9F, 0x0A), 0);
    lv_obj_set_style_bg_opa  (s_pos_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_radius  (s_pos_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_pos_marker, 0, 0);
    lv_obj_clear_flag(s_pos_marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_pos_marker, LV_OBJ_FLAG_CLICKABLE);

    /* Initial sync: text + border + marker pozisyonu. */
    update_box_labels();
    update_box_borders();
    update_pos_marker();

    /* ── Gün chip'leri (S M T W T F S) ─────────────────────────────────────
     * Dial alt kenarı y=360 (60 + 300). Chip y=365 → 5 px gap (kompakt).
     * 7 chip × 36 px + 6 gap × 7 px = 252+42 = 294. x0 = 58.
     * Bu kompakt yerleşim hint label (y=405) ile çakışmayı önler ve
     * Edit modundaki Delete butonu (y_off -2 → top 408) için yer açar. */
    static const char* day_letters[7] = {"S","M","T","W","T","F","S"};
#ifdef BOARD_VARIANT_LCD128
    /* Session 40: 24 px/4 gap (192 px) uçlardaki iki chip'i bezelin altına
     * sokuyordu (y=208'de görünür kiriş x≈60..180). 20 px/2 gap = 152 px,
     * y=188..208 → alt kiriş 38..202, chip'ler 44..196 → içeride. */
    int chip_w  = 20;
    int chip_g  = 2;
#else
    int chip_w  = 36;
    int chip_g  = 7;
#endif
    int total_w = 7 * chip_w + 6 * chip_g;
#ifdef BOARD_VARIANT_LCD128
    int chip_y  = 188;
    int chip_x0 = (240 - total_w) / 2;
#else
    int chip_y  = 365;
    int chip_x0 = (410 - total_w) / 2;
#endif

    for (int d = 0; d < 7; d++) {
        lv_obj_t* chip = lv_obj_create(s_view_edit);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, chip_w, chip_w);
        lv_obj_set_pos(chip, chip_x0 + d * (chip_w + chip_g), chip_y);
        bool on = (s_edit_buf.dow_mask >> d) & 1u;
        /* Static style add — theme'in pressed mavi'sini de keser. Toggle
         * sırasında edit_dow_chip_click_cb iki stil arasında swap yapar.
         * Tüm interaktif state'lere ayrı ayrı eklenir (belt-and-suspenders). */
        add_style_all_states(chip, on ? &s_orange_chip_st : &s_dark_chip_st);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chip, edit_dow_chip_click_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)d);

        lv_obj_t* lbl = lv_label_create(chip);
        lv_obj_set_style_text_color(lbl,
            on ? lv_color_white() : lv_color_make(0x99, 0x99, 0xAA), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
#else
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
#endif
        lv_obj_center(lbl);
        lv_label_set_text(lbl, day_letters[d]);

        s_dow_chips[d] = chip;
        wear_press_feedback_attach(chip);   /* Apple Watch tap dim feedback */
    }

    /* Hint "(no day = one-shot)" — yalnızca New Alarm modunda görünür.
     * Chips alt kenarı y=401 (365+36). Hint y=405 → 4 px gap.
     * Edit modunda Delete butonu y=408'den başladığı için hint gizli; orada
     * kullanıcı zaten dow_mask'ı bildiği için bu açıklama gerekmiyor. */
    if (!alarm_service_slot_used((uint8_t)idx)) {
        lv_obj_t* hint = lv_label_create(s_view_edit);
        lv_obj_set_style_text_color(hint, lv_color_make(0x66, 0x66, 0x77), 0);
#ifdef BOARD_VARIANT_LCD128
        /* Session 40: y=405 ekran DIŞIYDI. Chip'lerin altındaki tek satır
         * (y=212; kiriş y=224'te x=60..180 → kısa metin). */
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 212);
        lv_label_set_text(hint, "no day = once");
#else
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 405);
        lv_label_set_text(hint, "(no day = one-shot)");
#endif
    }

    /* Alt: Delete butonu (sadece var olan slot için)
     * y_offset -2 → top y=408 (chips end y=401 + 7 px gap).
     * Buton 50 px → bottom y=458 (screen 466 max → 8 px güvenlik). */
    if (alarm_service_slot_used((uint8_t)idx)) {
        lv_obj_t* del = lv_button_create(s_view_edit);
#ifdef BOARD_VARIANT_LCD128
        /* Session 40: 200×50 buton y=188..238 → yuvarlak ekranın alt
         * kirişinin dışında VE gün chip'lerinin üstündeydi. Chip'lerin
         * altında küçük pill (kiriş y=228'de x=83..157 → 72 px sığar). */
        lv_obj_set_size(del, 72, 18);
        lv_obj_align(del, LV_ALIGN_TOP_MID, 0, 211);
        lv_obj_set_ext_click_area(del, 10);
#else
        lv_obj_set_size(del, 200, 50);
        lv_obj_align(del, LV_ALIGN_BOTTOM_MID, 0, -2);
#endif
        lv_obj_set_style_radius(del, 25, 0);
        lv_obj_set_style_bg_color(del, lv_color_make(0x33, 0x12, 0x12), 0);
        lv_obj_set_style_bg_color(del, lv_color_make(0x55, 0x18, 0x18), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(del, 0, 0);
        lv_obj_set_style_border_width(del, 1, 0);
        lv_obj_set_style_border_color(del, lv_color_make(0xFF, 0x44, 0x44), 0);
        lv_obj_add_event_cb(del, edit_delete_cb, LV_EVENT_CLICKED, NULL);
        wear_press_feedback_attach(del);

        lv_obj_t* dlbl = lv_label_create(del);
        lv_obj_set_style_text_color(dlbl, lv_color_make(0xFF, 0x55, 0x55), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_style_text_font(dlbl, &lv_font_montserrat_12, 0);
#else
        lv_obj_set_style_text_font(dlbl, &lv_font_montserrat_20, 0);
#endif
        lv_obj_center(dlbl);
        lv_label_set_text(dlbl, LV_SYMBOL_TRASH " Delete");
    }

    app_manager_enable_gestures(s_view_edit);
    lv_obj_add_event_cb(s_view_edit, edit_view_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* Tema'yı geri ver — sonraki uygulamalar normal stilini alsın. Mevcut
     * widget'lar zaten yaratılırken tema NULL'du, etkilenmez. */
    if (disp && saved_theme) lv_display_set_theme(disp, saved_theme);
}

/* ─── Ringing view ───────────────────────────────────────────────────────── */

static void ring_anim_cb(lv_timer_t* t)
{
    (void)t;
    if (!s_ring_bell) return;
    /* Çan ikonunu hafifçe büyüt-küçült (250→290→250) */
    static bool grow = true;
    static int  scale = 256;
    if (grow) { scale += 8; if (scale >= 296) grow = false; }
    else      { scale -= 8; if (scale <= 248) grow = true;  }
    lv_obj_set_style_transform_scale(s_ring_bell, scale, 0);
}

static void ring_clock_cb(lv_timer_t* t)
{
    (void)t;
    if (!s_ring_time_lbl) return;
    time_t now; time(&now);
    struct tm ti; localtime_r(&now, &ti);
    lv_label_set_text_fmt(s_ring_time_lbl, "%02d:%02d", ti.tm_hour, ti.tm_min);
}

static void cleanup_ringing(void)
{
    if (s_ring_anim_timer)  { lv_timer_delete(s_ring_anim_timer);  s_ring_anim_timer  = NULL; }
    if (s_ring_clock_timer) { lv_timer_delete(s_ring_clock_timer); s_ring_clock_timer = NULL; }
    s_ring_bell = s_ring_time_lbl = s_ring_label_lbl = NULL;
    /* Defensive: ringing view yıkılırken ses HER ZAMAN durur. Stop/Snooze
     * butonları zaten audio_service_stop çağırır; bu hat back-button,
     * uygulama değişimi (on_pause→on_stop), force-cleanup gibi yan
     * yollardan da sesin sürmemesini garanti eder. İdempotent. */
    audio_service_stop();
}

static void ring_stop_cb(lv_event_t* e)
{
    (void)e;
    /* Sesi anında kes — kullanıcı uyandı, alarm tüketildi. */
    audio_service_stop();
    /* Brightness'ı geri ver (kullanıcı ayarladığı seviyeye dön) */
    co5300_set_brightness(s_brightness_saved);
    s_ring_idx = -1;
    show_list();
}

static void ring_snooze_cb(lv_event_t* e)
{
    (void)e;
    /* Sesi kes — 5 dk sonra alarm yeniden tetikleneceğinde audio da
     * yeniden başlar. */
    audio_service_stop();
    if (s_ring_idx >= 0) {
        alarm_service_snooze((uint8_t)s_ring_idx, 5);   /* 5 dk sonra tekrar */
        ESP_LOGI(TAG, "Snoozed alarm[%d] for 5 minutes", s_ring_idx);
    }
    co5300_set_brightness(s_brightness_saved);
    s_ring_idx = -1;
    /* Snooze sonrası launcher'a düşmek istemiyoruz; arka plana gitsin diye
     * back tetikle (önceki uygulamaya dön). Eğer yoktu watchface'e iner. */
    app_manager_back();
}

static void show_ringing(int idx)
{
    destroy_all_views();
    s_current_view = VIEW_RINGING;
    s_ring_idx = idx;

    /* Brightness'ı maksa çek — kullanıcıyı uyandırma için */
    /* (Geri verirken s_brightness_saved kullanılacak; settings_app brightness
     * NVS persist etmiyorsa bile bizim kapatma akışımız 255'ten önceki canlı
     * değere döndürmeyi garantilemiyor — bu yüzden 255 öncesi değeri biz de
     * okuyamıyoruz. Pratik çözüm: 255'e set et, çıkışta varsayılan 200'e dön
     * — kullanıcı zaten settings'ten yeniden ayarlayabilir.) */
    s_brightness_saved = 200;
    co5300_set_brightness(255);

    s_view_ring = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_view_ring);
    lv_obj_set_size(s_view_ring, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_view_ring, lv_color_make(0x10, 0x06, 0x00), 0);
    lv_obj_set_style_bg_grad_color(s_view_ring, lv_color_make(0x4A, 0x1F, 0x00), 0);
    lv_obj_set_style_bg_grad_dir(s_view_ring, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_view_ring, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_view_ring, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_view_ring, LV_OBJ_FLAG_SCROLLABLE);

    /* Çalan saat (üst) */
    s_ring_time_lbl = lv_label_create(s_view_ring);
    lv_obj_set_style_text_color(s_ring_time_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    /* Session 40: 240 yuvarlak — 48 punto saat, 150×60 çift buton (birbirinin
     * üstüne biniyordu) AMOLED içindi. Saat 32, butonlar alt alta 112×30. */
    lv_obj_set_style_text_font(s_ring_time_lbl, &lv_font_montserrat_32, 0);
    lv_obj_align(s_ring_time_lbl, LV_ALIGN_TOP_MID, 0, 26);
#else
    lv_obj_set_style_text_font(s_ring_time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_align(s_ring_time_lbl, LV_ALIGN_TOP_MID, 0, 60);
#endif
    ring_clock_cb(NULL);

    /* Etiket (varsa) */
    s_ring_label_lbl = lv_label_create(s_view_ring);
    lv_obj_set_style_text_color(s_ring_label_lbl, lv_color_make(0xFF, 0xCC, 0x88), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_ring_label_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_ring_label_lbl, LV_ALIGN_TOP_MID, 0, 66);
#else
    lv_obj_set_style_text_font(s_ring_label_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_ring_label_lbl, LV_ALIGN_TOP_MID, 0, 120);
#endif
    alarm_t a;
    if (idx >= 0 && alarm_service_get((uint8_t)idx, &a) && a.label[0]) {
        lv_label_set_text(s_ring_label_lbl, a.label);
    } else {
        lv_label_set_text(s_ring_label_lbl, "Alarm");
    }

    /* Çan ikonu (büyük, animasyonlu) */
    s_ring_bell = lv_label_create(s_view_ring);
    lv_obj_set_style_text_color(s_ring_bell, lv_color_make(0xFF, 0x9F, 0x0A), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_ring_bell, &lv_font_montserrat_44, 0);
    lv_obj_align(s_ring_bell, LV_ALIGN_TOP_MID, 0, 88);
#else
    lv_obj_set_style_text_font(s_ring_bell, &lv_font_montserrat_48, 0);
    lv_obj_align(s_ring_bell, LV_ALIGN_CENTER, 0, -20);
#endif
    lv_obj_set_style_transform_pivot_x(s_ring_bell, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_ring_bell, LV_PCT(50), 0);
    lv_label_set_text(s_ring_bell, LV_SYMBOL_BELL);

    /* Snooze butonu (sol-alt) */
    lv_obj_t* snz = lv_button_create(s_view_ring);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(snz, 112, 30);
    lv_obj_align(snz, LV_ALIGN_TOP_MID, 0, 138);
#else
    lv_obj_set_size(snz, 150, 60);
    lv_obj_align(snz, LV_ALIGN_BOTTOM_LEFT, 35, -50);
#endif
    lv_obj_set_style_radius(snz, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(snz, lv_color_make(0x33, 0x33, 0x44), 0);
    lv_obj_set_style_bg_color(snz, lv_color_make(0x55, 0x55, 0x66), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(snz, 0, 0);
    lv_obj_add_event_cb(snz, ring_snooze_cb, LV_EVENT_CLICKED, NULL);
    wear_press_feedback_attach(snz);
    lv_obj_t* slbl = lv_label_create(snz);
    lv_obj_set_style_text_color(slbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(slbl, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(slbl, &lv_font_montserrat_20, 0);
#endif
    lv_obj_center(slbl);
    lv_label_set_text(slbl, "Snooze");

    /* Stop butonu (sağ-alt, parlak turuncu) */
    lv_obj_t* stp = lv_button_create(s_view_ring);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(stp, 112, 30);
    lv_obj_align(stp, LV_ALIGN_TOP_MID, 0, 174);   /* alt kenar 204: kiriş x=42..198 */
#else
    lv_obj_set_size(stp, 150, 60);
    lv_obj_align(stp, LV_ALIGN_BOTTOM_RIGHT, -35, -50);
#endif
    lv_obj_set_style_radius(stp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(stp, lv_color_make(0xFF, 0x9F, 0x0A), 0);
    lv_obj_set_style_bg_color(stp, lv_color_make(0xCC, 0x7F, 0x08), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(stp, 0, 0);
    lv_obj_add_event_cb(stp, ring_stop_cb, LV_EVENT_CLICKED, NULL);
    wear_press_feedback_attach(stp);
    lv_obj_t* tlbl = lv_label_create(stp);
    lv_obj_set_style_text_color(tlbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_20, 0);
#endif
    lv_obj_center(tlbl);
    lv_label_set_text(tlbl, "Stop");

    /* Animasyon ve saat timer'ları */
    s_ring_anim_timer  = lv_timer_create(ring_anim_cb,  60,   NULL);
    s_ring_clock_timer = lv_timer_create(ring_clock_cb, 1000, NULL);

    /* ── Ses başlat ─────────────────────────────────────────────────────────
     * Alarm pattern'i (A5→B5→D6 arpeggio + 700 ms duraklama) sonsuz loop'ta
     * çalar. Stop/Snooze veya cleanup_ringing (back, on_stop) sesi keser.
     * audio_service başlatılmamışsa (boot'ta init başarısızsa) no-op olur,
     * görsel UI normal çalışmaya devam eder. */
    audio_service_play_alarm();
}

/* ─── Lifecycle ──────────────────────────────────────────────────────────── */

static void on_start(lv_obj_t* screen)
{
    ESP_LOGI(TAG, "Alarm app starting");
    s_screen = screen;
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    /* on_start tetiklenmesinin sebebi pending bir alarm mı? */
    int p = alarm_service_pop_pending();
    if (p >= 0) {
        show_ringing(p);
    } else {
        show_list();
    }
}

static void on_resume(void)
{
    /* Geri dönüldüğünde bir pending varsa hemen ringing'e geç */
    int p = alarm_service_pop_pending();
    if (p >= 0) {
        show_ringing(p);
        return;
    }
    /* RINGING ekranı zaten açıkken pause olduysa timer'ları yeniden çalıştır */
    if (s_ring_anim_timer)  lv_timer_resume(s_ring_anim_timer);
    if (s_ring_clock_timer) lv_timer_resume(s_ring_clock_timer);
}

static void on_pause(void)
{
    /* RINGING ekranı arka plana düşerse animasyon ve saat timer'larını
     * pause et — invisible objects üzerinde 60 ms tick boşa CPU/SPI yükü
     * yaratır. on_resume'da yeniden start edilir, on_stop'ta cleanup yapılır. */
    if (s_ring_anim_timer)  lv_timer_pause(s_ring_anim_timer);
    if (s_ring_clock_timer) lv_timer_pause(s_ring_clock_timer);
}

static void on_stop(void)
{
    cleanup_ringing();
    s_view_list = s_view_edit = s_view_ring = NULL;
    s_screen = NULL;
}

static bool on_event(const os_event_t* event)
{
    /* APP_SWITCH dispatch'ten önce app_manager tarafından tüketildi
     * (alarm_service yayınladığında). Yine de tedbiren: pending varsa
     * ringing moduna geç. */
    int p = alarm_service_pop_pending();
    if (p >= 0) {
        show_ringing(p);
        return true;
    }

    /* ─── BTN_TOP increment (yalnızca New/Edit Alarm ekranında) ────────────
     * Edit ekranında üst butona kısa basınca aktif HH veya MM 1 birim artar.
     * Wrap-around: HH 23→00, MM 59→00. Diğer ekranlarda (list, ringing) ve
     * uzun basışta `false` döndürülür → app_manager default davranışı (back
     * veya watchface'e dön) çalışır. Böylece BTN_TOP'un global anlamı (geri)
     * yalnızca bu ekran için ezilir; kullanıcı diğer her yerde aynı butona
     * basınca alıştığı geri davranışını alır. */
    if (event && event->type == EVENT_BTN_TOP && event->data == 0 /* short press */) {
        if (s_current_view == VIEW_EDIT) {
            if (s_edit_mode == 0) {
                /* HH increment: 11→12 (AM→PM cycle) veya 23→0 (PM→AM cycle)
                 * sınırı geçildiğinde dial swap olmalı. Önce eski cycle'ı
                 * kaydet, hour artır, yeni cycle ile karşılaştır → değiştiyse
                 * update_hour_dial_visibility() AM↔PM dial'ı swap eder. */
                bool was_pm = (s_edit_buf.hour >= 12);
                s_edit_buf.hour = (uint8_t)((s_edit_buf.hour + 1) % 24);
                bool is_pm = (s_edit_buf.hour >= 12);
                if (was_pm != is_pm) update_hour_dial_visibility();
            } else {
                s_edit_buf.minute = (uint8_t)((s_edit_buf.minute + 1) % 60);
            }
            update_box_labels();
            update_pos_marker();
            return true;   /* tüketildi → app_manager back yapmasın */
        }
    }
    return false;
}

static const app_interface_t s_iface = {
    .on_start  = on_start,
    .on_resume = on_resume,
    .on_pause  = on_pause,
    .on_stop   = on_stop,
    .on_event  = on_event,
    .name      = "Alarm",
};

const app_interface_t* alarm_app_get_interface(void) { return &s_iface; }
