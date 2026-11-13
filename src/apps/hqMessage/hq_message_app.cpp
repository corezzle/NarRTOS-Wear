/**
 * @file hq_message_app.cpp
 * @brief Genel Müdürlük mesaj ekranı (v1.2.9).
 *
 * Layout (round AMOLED merkezli, LCD128'de de oranlı çalışır):
 *   - Üst-orta: yuvarlak görsel placeholder alanı (ileride upload edilen
 *     resim buraya çizilecek).
 *   - Alt: mesaj kartı — "Genel Müdürlükten Mesaj" metni. Türkçe karakterler
 *     için notification_banner ile aynı notif_font_* set'i kullanılır.
 */

#include "hq_message_app.h"
#include "../../core/app_manager.h"
#include "../../assets/hq_avatar.h"
#include "wear_design.h"
#include <lvgl.h>
#include <esp_log.h>

static const char* TAG = "HQMessage";

/* Türkçe glyph'li fontlar (bkz. src/fonts/notif_font_*.c — range'inde
 * İı/Ğğ/Şş + Latin-1 supplement var; montserrat stock'ta ü/ö yok). */
LV_FONT_DECLARE(notif_font_20);
LV_FONT_DECLARE(notif_font_16);

static void hq_on_start(lv_obj_t* screen)
{
    lv_obj_set_style_bg_color(screen, WD_BG_BASE, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t hor = lv_disp_get_hor_res(NULL);
    const int32_t ver = lv_disp_get_ver_res(NULL);
    /* LCD128 (240 px) gibi küçük panellerde font/oran küçültülür. */
    const bool small = (hor <= 280);

    /* ── Yuvarlak avatar (Genel Müdür fotoğrafı) ─────────────────────────
     * Dairesel alpha maskeli ARGB8888, iki boyut: 88 px (LCD128) /
     * 196 px (AMOLED, 1.8"). Teknopark logosunun yerini aldı (2026-08-19). */
#ifdef BOARD_VARIANT_LCD128
    const lv_image_dsc_t* logo_dsc = &hq_avatar_88;
#else
    const lv_image_dsc_t* logo_dsc = &hq_avatar_196;
#endif
    const int32_t circle_d = logo_dsc->header.w;
    lv_obj_t* circle = lv_image_create(screen);
    lv_image_set_src(circle, logo_dsc);
    lv_obj_align(circle, LV_ALIGN_CENTER, 0, -(ver * (small ? 24 : 16)) / 100);

    /* ── Mesaj alanı: başlık + gövde, arka plansız, dikey scroll ─────────
     * Kullanıcı isteği (2026-08-19): gri kart kaldırıldı — metin doğrudan
     * siyah zeminde. Uzun mesajlar alan içinde yukarı kaydırılarak okunur
     * (dikey swipe nav'da no-op olduğundan çakışma yok). */
    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, WD_PAD_XS, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);

    /* Başlık: boyut büyütmeden belirginlik — accent mavi + hafif harf
     * aralığı (soluk gri gövdeyle karışıyordu, kullanıcı isteği). */
    lv_obj_t* header = lv_label_create(body);
    lv_obj_set_style_text_font(header, &notif_font_16, 0);
    lv_obj_set_style_text_color(header, WD_ACCENT_BLUE, 0);
    lv_obj_set_style_text_letter_space(header, 1, 0);
    lv_obj_set_style_text_align(header, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(header, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(header, lv_pct(100));
    lv_label_set_text(header, "Genel Müdürlükten Mesaj");

    lv_obj_t* msg = lv_label_create(body);
    lv_obj_set_style_text_font(msg, small ? &notif_font_16 : &notif_font_20, 0);
    lv_obj_set_style_text_color(msg, WD_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, lv_pct(100));
    /* Örnek içerik: Teknopark İstanbul Genel Müdürü Prof. Dr. Abdurrahman
     * Akyol'un resmi sitedeki (teknoparkistanbul.com.tr, Trexo haberi)
     * açıklamasından kısa alıntı. Gerçek mesajlar ileride dinamik gelecek. */
    lv_label_set_text(msg,
        "\"Her şirketimizin başarısı bizim omuzlarımıza ülkemiz adına "
        "daha fazla sorumluluk yüklüyor.\"");

    /* Alan çemberin altından ekranın altına uzanır (%76 genişlik, %5 alt
     * marj). NOT: yuvarlak panelde alt satırlar kirişin daraldığı bölgeye
     * denk gelip kenarlarda kırpılabiliyor (2026-08-19 cihaz testi) —
     * alternatif: genişlik %64 + alt sınır %88 (kiriş içinde kalır).
     * Kullanıcı iki hali karşılaştırıp seçecek. */
    lv_obj_update_layout(circle);
    const int32_t gap      = small ? WD_PAD_S : WD_PAD_M;
    const int32_t body_top = lv_obj_get_y(circle) + circle_d + gap;
    const int32_t body_h   = ver - body_top - (ver * 5) / 100;
    lv_obj_set_size(body, (hor * 76) / 100, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, body_top);

    app_manager_enable_gestures(screen);
    ESP_LOGI(TAG, "HQ message screen started");
}

static void hq_on_resume(void) {}
static void hq_on_pause(void)  {}
static void hq_on_stop(void)   {}
static bool hq_on_event(const os_event_t* e) { (void)e; return false; }

static const app_interface_t s_hq_iface = {
    .on_start  = hq_on_start,
    .on_resume = hq_on_resume,
    .on_pause  = hq_on_pause,
    .on_stop   = hq_on_stop,
    .on_event  = hq_on_event,
    .name      = "HQ Message",
};

const app_interface_t* hq_message_app_get_interface(void) { return &s_hq_iface; }
