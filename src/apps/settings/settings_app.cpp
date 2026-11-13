/**
 * @file settings_app.cpp
 * @brief Ayarlar — Apple Watch tarzı iki seviyeli navigasyon
 *
 * Ana ekran (Settings):
 *   [General]  [WiFi]  [Display & Brightness]   — her biri chevron (>) ile
 *
 * Alt ekranlar ("< Başlık" ile ana ekrana dönülür):
 *   General            → About
 *   About              → Cihaz bilgileri
 *   WiFi               → WiFi toggle
 *   Display&Brightness → Brightness  ☀ ──●── ☀
 */

#include "settings_app.h"
#include "../../core/app_manager.h"
#include "../../core/os_config.h"
#include "../../drivers/display/co5300_driver.h"
#include "../../services/time_service.h"
#include "../../services/power_service.h"   /* Session 34: ortak parlaklık */
#include "../../services/wifi_service.h"
#include "../../services/ui_service.h"
#include "../../services/unistroke.h"
#include "../../services/ota_service.h"
#include "../../drivers/power/axp2101_driver.h"   /* v1.1.1: Güç Rayları sayfası */
#include "../../assets/sun_icons.h"
#include "../../assets/plane_icon.h"
#include "../../../include/wear_motion.h"
#include <lvgl.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <math.h>
#include <stdlib.h>
#include <esp_timer.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
/* v1.2.3 (2026-05-19): Storage view için heap + PSRAM + SPIFFS + NVS info */
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_spiffs.h>
#include <esp_ota_ops.h>
#include <esp_image_format.h>   /* v1.2.3: esp_image_verify ile gerçek firmware size */
#include <nvs.h>

static const char* TAG = "Settings";

// ─── Kalıcı state (ekranlar arası) ───────────────────────────────────────────
static bool    s_wifi_enabled = false;
static uint8_t s_brightness   = 255;

// Canlı widget referansları (on_stop'ta sıfırlanır)
static lv_obj_t* s_wifi_switch       = NULL;
static lv_obj_t* s_wifi_knob         = NULL;  // özel toggle'ın beyaz knob'u
static lv_obj_t* s_au_switch         = NULL;  // Otomatik Güncelleme toggle (outer)
static lv_obj_t* s_au_knob           = NULL;  // ... knob'u
static lv_obj_t* s_br_bar            = NULL;  // brightness pill (track)
static lv_obj_t* s_br_fill           = NULL;  // brightness pill (white fill)
static int32_t   s_br_bar_w          = 0;     // bar inner genişlik (px)
static lv_obj_t* s_wifi_section_lbl  = NULL;
static lv_obj_t* s_wifi_list         = NULL;
/* Bağlı ağın WiFi toggle'ın hemen altında gösterildiği özel satır.
 * Bağlı ağ ana listede tekrar görünmez; iOS Watch davranışıyla birebir. */
static lv_obj_t* s_connected_row     = NULL;

// Bağlantı overlay
static lv_obj_t*   s_conn_overlay    = NULL;
static lv_obj_t*   s_conn_status_lbl = NULL;
static lv_obj_t*   s_pw_textarea     = NULL;
static lv_obj_t*   s_pw_scribble     = NULL;   /* scribble canvas paneli */
static lv_obj_t*   s_pw_hint_lbl     = NULL;   /* canvas üstünde "Draw a letter" ipucu */
/* Şifre giriş modu (scribble vs. tam klavye) */
typedef enum { PW_INPUT_SCRIBBLE = 0, PW_INPUT_KEYBOARD = 1 } pw_input_mode_t;
static pw_input_mode_t s_pw_input_mode = PW_INPUT_SCRIBBLE;
static lv_obj_t*   s_pw_keyboard     = NULL;   /* lv_keyboard widget'ı (sadece KEYBOARD modunda) */
static lv_obj_t*   s_pw_del_btn      = NULL;   /* alt bar — scribble moduna özel */
static lv_obj_t*   s_pw_ok_btn       = NULL;   /* alt bar — scribble moduna özel */
static lv_obj_t*   s_pw_space_btn    = NULL;   /* alt bar — scribble moduna özel */
static lv_obj_t*   s_pw_mode_btn     = NULL;   /* alt bar mod toggle butonu */
static lv_obj_t*   s_pw_mode_btn_lbl = NULL;   /* "Keyboard" ↔ "Scribble" etiketi */
static lv_timer_t* s_close_timer     = NULL;

/* ── Özel şifre tamponu + maske zamanlayıcısı ──────────────────────────────
 * LVGL'in pwd_mode + pwd_show_time mekanizması lv_anim_path_step üzerinden
 * çalışıyor; "4 s aç, sonra gizle" olarak set edildiğinde kullanıcı yine
 * karakter çok kısa görünüyor diye şikayet ediyor. Problem: LVGL
 * `lv_textarea_add_char` içinde eski karakterleri ANINDA `pwd_char_hider`
 * ile maskeliyor ve anim-based reveal'i pwd_show_time süresince bekletiyor
 * — ama bazı cascade (state değişimi, invalidate, scroll, tema) anim'i
 * erken iptal edebiliyor. Kontrolü LVGL'e bırakmak yerine kendi basit
 * tamponumuzu ve lv_timer'ımızı kuruyoruz.
 *
 * Gerçek şifre tam uzunluğunda s_pw_real[]'da tutulur. Ekranda gösterilen
 * textarea metni manuel yazılır: reveal süresi boyunca son eklenen harf
 * açık, geri kalanı '*' maskeli. Süre dolunca hepsi '*' olur. */
#define PW_MAX_LEN       64
#define PW_REVEAL_MS     5000    /* son eklenen karakter bu kadar ms açık kalır */
static char        s_pw_real[PW_MAX_LEN + 1] = {0};
static uint8_t     s_pw_len                  = 0;
static lv_timer_t* s_pw_hide_timer           = NULL;

/* v1.4.55 (2026-05-19) — audit orta öncelik #3: secure_zero helper.
 * Önceki "s_pw_real[0] = 0;" sadece ilk byte'ı sıfırlıyordu → BSS'de
 * password plaintext kalıyordu (heap dump / RAM forensics → ifşa).
 * memset compiler dead-store elimination'a karşı korunmuyor; volatile
 * pointer loop ile garanti edilir. */
static inline void secure_zero(void* p, size_t n)
{
    volatile unsigned char* vp = (volatile unsigned char*)p;
    while (n--) *vp++ = 0;
}

/* Scribble stroke capture buffer — canvas-local koordinatlar.
 * Çoklu stroke (örn. "H" = 3 ayrı çizim) s_stroke_pts'te ardışık saklanır;
 * stroke sınırları s_strokes[] meta dizisinde tutulur. Classify zamanı
 * hepsini birleştirip $1'e veriyoruz (path-jump'lar $1'in "best angle"
 * aramasıyla bir miktar tolere edilir). */
#define SCRIBBLE_MAX_POINTS 1536
#define SCRIBBLE_MAX_STROKES 10
#define SCRIBBLE_INTERP_STEP 3     /* peşpeşe stroke noktaları arasındaki hedef px mesafesi
                                      — her PRESSING örneğinde, son captured nokta ile yeni
                                      nokta arasına bu mesafeye göre ara noktalar serpiştirilir.
                                      3 px + line_width=8 px = round_cap yarıçapı 4 px her iki
                                      ucu örtüyor → gözle görülür "bead" kalmıyor. */
#define SCRIBBLE_JITTER_PX   2     /* bu kadardan kısa hareketler jitter say, atla */
#define SCRIBBLE_LINE_W      8     /* çizgi kalınlığı (px) */

typedef struct { uint16_t start; uint16_t count; } stroke_meta_t;

static unistroke_point_t s_stroke_pts[SCRIBBLE_MAX_POINTS];
static uint16_t          s_stroke_n   = 0;          /* toplam nokta */
static stroke_meta_t     s_strokes[SCRIBBLE_MAX_STROKES];
static uint8_t           s_stroke_count = 0;        /* tamamlanan stroke sayısı */
static bool              s_stroke_live  = false;    /* parmak ekranda mı? */
static uint16_t          s_cur_stroke_start = 0;    /* aktif stroke'un başlangıç indeksi */
/* PRESSING event'inde full-invalidate'i 3 px throttle ile tetiklemek için
 * son invalidate anındaki parmak konumu. */
static int32_t           s_last_inv_x = 0;
static int32_t           s_last_inv_y = 0;

/* ── Touch-jitter grace-period commit ─────────────────────────────────────
 * Kapasitif ekran dokunma kontrolcüsü hızlı eğrisel hareket sırasında
 * (örn. 'a' harfinin yuvarlağı, 'o', 'c', 'g', 'b' kıvrımları) bazen 1-2
 * kare süreyle press=false raporlayabilir → LVGL anında LV_EVENT_RELEASED
 * fırlatır, bizim kod stroke'u finalize eder ve bir sonraki frame
 * LV_EVENT_PRESSED geldiğinde YENİ bir stroke başlatır. Kullanıcı ise hiç
 * parmağını kaldırmadı — sonuç: tek bir sürekli stroke olması gereken
 * çizim, ekranda 2-3-4 ayrı parçaya bölünmüş gibi görünüyor
 * (fotoğrafta 'a' yuvarlağı kesik kesik).
 *
 * Çözüm: RELEASED gelince stroke'u HEMEN commit etme. Kısa bir timer
 * (TOUCH_MERGE_MS) başlat. Bu pencere içinde yeni bir PRESSED gelirse
 * VE son nokta ile mekansal olarak yakınsa → aynı stroke'a devam et
 * (interpolate ile boşluğu doldur). Timer süresi dolarsa gerçekten
 * kaldırılmış kabul edilir, stroke meta'ya eklenir. */
#define TOUCH_MERGE_MS   150    /* touch-glitch için grace penceresi */
#define TOUCH_MERGE_PX   60     /* bu kadar yakın bir yeniden-PRESSED → merge */
static lv_timer_t*       s_commit_timer = NULL;

// Son scan sonuçları + bağlı ağ
static wifi_ap_t s_last_aps[WIFI_SCAN_MAX_APS];
static uint8_t   s_last_ap_count     = 0;
static char      s_connected_ssid[33] = {};
static char      s_pending_ssid[33]   = {};
static bool      s_pending_secured    = false;

// View container'ları
static lv_obj_t* s_view_main     = NULL;
static lv_obj_t* s_view_general  = NULL;
static lv_obj_t* s_view_about    = NULL;
static lv_obj_t* s_view_storage  = NULL;   // v1.2.3: Depolama bilgi sayfası
static lv_obj_t* s_view_network  = NULL;
static lv_obj_t* s_view_display  = NULL;
static lv_obj_t* s_view_airplane = NULL;
static lv_obj_t* s_view_swupdate = NULL;   // Software Updates
static lv_obj_t* s_view_auto_update = NULL; // General → Otomatik Güncelleme alt sayfası
static lv_obj_t* s_view_power    = NULL;   // v1.1.1: Güç Rayları (AXP rail toggle'ları + Soft Power Off)
static lv_obj_t* s_power_content = NULL;

/* v1.1.1 → v1.4.47: Güç Rayları sayfasındaki toggle widget'ları.
 * v1.4.47'de schematic re-mapping ile 5 → 9 rail genişledi:
 *   4 ALDO + 3 DCDC (2,3,4) + 2 BLDO. Her rail için outer+knob pointer'ı. */
static lv_obj_t* s_rail_aldo1_switch = NULL;   /* ALDO1 — audio A3V3 muhtemel */
static lv_obj_t* s_rail_aldo1_knob   = NULL;
static lv_obj_t* s_rail_aldo2_switch = NULL;   /* ALDO2 — display panel (DSI_PWR_EN) */
static lv_obj_t* s_rail_aldo2_knob   = NULL;
static lv_obj_t* s_rail_aldo3_switch = NULL;   /* ALDO3 — motor + sistem paralel */
static lv_obj_t* s_rail_aldo3_knob   = NULL;
static lv_obj_t* s_rail_aldo4_switch = NULL;   /* ALDO4 — VL3 1.8V (belirsiz) */
static lv_obj_t* s_rail_aldo4_knob   = NULL;
static lv_obj_t* s_rail_dcdc2_switch = NULL;   /* DCDC2 — 0.9V (belirsiz) */
static lv_obj_t* s_rail_dcdc2_knob   = NULL;
static lv_obj_t* s_rail_dcdc3_switch = NULL;   /* DCDC3 — 1.2V (belirsiz) — v1.4.47 */
static lv_obj_t* s_rail_dcdc3_knob   = NULL;
static lv_obj_t* s_rail_dcdc4_switch = NULL;   /* DCDC4 — 1.8V (display IO muhtemel) — v1.4.47 */
static lv_obj_t* s_rail_dcdc4_knob   = NULL;
static lv_obj_t* s_rail_bldo1_switch = NULL;   /* BLDO1 — 1.2V (belirsiz) — v1.4.47 */
static lv_obj_t* s_rail_bldo1_knob   = NULL;
static lv_obj_t* s_rail_bldo2_switch = NULL;   /* BLDO2 — 2.8V (belirsiz) — v1.4.47 */
static lv_obj_t* s_rail_bldo2_knob   = NULL;

/* v1.4.38+47: Inverse semantic — toggle ON = rail KAPATILDI, OFF = rail
 * AÇIK (default). Sayfa açılışında hepsi OFF görünür. */
static bool s_rail_aldo1_visual_on = false;
static bool s_rail_aldo2_visual_on = false;
static bool s_rail_aldo3_visual_on = false;
static bool s_rail_aldo4_visual_on = false;
static bool s_rail_dcdc2_visual_on = false;
static bool s_rail_dcdc3_visual_on = false;
static bool s_rail_dcdc4_visual_on = false;
static bool s_rail_bldo1_visual_on = false;
static bool s_rail_bldo2_visual_on = false;

/* v1.1.1: Display rail kapatılınca ekran kararıyor → 5sn auto-wake timer
 * + ekrana dokununca anında uyandırma. Timer ve invisible touch overlay'i
 * yönetir. Bir tane yeter çünkü iki display rail aynı anda kapatılabilse de
 * her ikisi tek wake mekanizmasıyla geri açılır. */
static lv_timer_t* s_rail_wake_timer   = NULL;
static lv_obj_t*   s_rail_wake_overlay = NULL;

// Scroll content roots
static lv_obj_t* s_main_content     = NULL;
static lv_obj_t* s_general_content  = NULL;
static lv_obj_t* s_about_content    = NULL;
static lv_obj_t* s_storage_content  = NULL;   // v1.2.3
static lv_obj_t* s_network_content  = NULL;
static lv_obj_t* s_display_content  = NULL;
static lv_obj_t* s_airplane_content = NULL;
static lv_obj_t* s_swupdate_content = NULL;

// Software Update view widget'ları (state'e göre içerik değişir)
static lv_obj_t* s_swu_icon        = NULL;   // büyük üst ikon
static lv_obj_t* s_swu_title       = NULL;   // başlık
static lv_obj_t* s_swu_subtitle    = NULL;   // alt başlık (sürüm vs)
static lv_obj_t* s_swu_notes       = NULL;   // release notes (multi-line)
static lv_obj_t* s_swu_progress    = NULL;   // bar (DOWNLOADING için)
static lv_obj_t* s_swu_progress_lbl= NULL;   // "%42" yazısı
static lv_obj_t* s_swu_button      = NULL;   // tek aksiyon butonu (Check/Install/Restart/Retry)
static lv_obj_t* s_swu_button_lbl  = NULL;
static lv_timer_t* s_swu_poll_timer = NULL;  // 1 Hz state polling
static ota_state_t s_swu_last_state  = OTA_STATE_IDLE;

// Airplane mode state ve toggle widget'ları
static bool      s_airplane_enabled      = false;
static bool      s_airplane_pref_wifi    = true;  // ON: airplane wifi'yi kapat
static bool      s_airplane_pref_bt      = true;  // ON: airplane BT'yi kapat
static bool      s_wifi_was_on_pre_apm   = false; // APM açılırken wifi'nin önceki hali
static lv_obj_t* s_apm_master_switch     = NULL;
static lv_obj_t* s_apm_master_knob       = NULL;
static lv_obj_t* s_apm_wifi_switch       = NULL;
static lv_obj_t* s_apm_wifi_knob         = NULL;
static lv_obj_t* s_apm_bt_switch         = NULL;
static lv_obj_t* s_apm_bt_knob           = NULL;

// ─── WiFi (arka plan task'ında bağlan) ────────────────────────────────────────
static void close_conn_overlay(void);
static void wifi_enable_task(void* arg)
{
    (void)arg;
    wifi_service_init();
    esp_err_t ret = wifi_service_connect(10000);
    s_wifi_enabled = (ret == ESP_OK);
    ESP_LOGI(TAG, "WiFi %s", s_wifi_enabled ? "ON" : "FAIL");
    if (s_wifi_enabled) time_service_sync_from_ntp(10000);
    vTaskDelete(NULL);
}
static void wifi_enable(void)
{
    /* Task spawn fail durumunda kullanıcı switch'i AÇIK görür ama hiçbir şey
     * yapmaz; en azından log + state'i geri çek ki UI tutarlı kalsın. */
    BaseType_t r = xTaskCreate(wifi_enable_task, "wifi_en", 4096, NULL, 5, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(wifi_en) failed");
        s_wifi_enabled = false;
    }
}
static void wifi_disable(void)
{
    wifi_service_disconnect();
    s_wifi_enabled = false;
    ESP_LOGI(TAG, "WiFi OFF");
}

#ifdef BOARD_VARIANT_LCD128
/* 240 yuvarlak: içerik sütunu 204px, kenarlardan 18px içeride. */
#define SETTINGS_SAFE_X 18
#define SETTINGS_SAFE_W 204
#else
#define SETTINGS_SAFE_X 12
#define SETTINGS_SAFE_W 386
#endif

static void ntp_sync_task(void* arg)
{
    (void)arg;
    time_service_sync_from_ntp(8000);
    vTaskDelete(NULL);
}

// ─── Navigasyon yardımcıları ──────────────────────────────────────────────────
/* Session 13: TEMBEL İNŞA — alt sayfalar ilk girişte kurulur (eskiden on_start
 * 10 sayfayı birden kuruyordu: ~300+ nesne → açılışta 241 ms takılma). build_*
 * fonksiyonları aşağıda tanımlı; on_start yalnız ana menüyü kurar, go_*_cb
 * gerekirse ensure_view() ile ilgili sayfayı kurar. on_stop TÜM view
 * pointer'larını NULL'lar — aksi halde ikinci açılışta ölü pointer kullanılır. */
static lv_obj_t* s_screen = NULL;
static void build_general(lv_obj_t* screen);
static void build_about(lv_obj_t* screen);
static void build_storage(lv_obj_t* screen);
static void build_software_update(lv_obj_t* screen);
static void build_auto_update(lv_obj_t* screen);
static void build_network(lv_obj_t* screen);
static void build_display(lv_obj_t* screen);
static void build_airplane(lv_obj_t* screen);
static void build_power(lv_obj_t* screen);
static void ensure_view(lv_obj_t** view, void (*build)(lv_obj_t*))
{
    if (*view || !s_screen) return;
    int64_t t0 = esp_timer_get_time();
    build(s_screen);
    if (*view) lv_obj_add_flag(*view, LV_OBJ_FLAG_HIDDEN);   /* slide_forward açar */
    ESP_LOGI(TAG, "lazy build %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
}

static void hide_all(void)
{
    if (s_view_main)        lv_obj_add_flag(s_view_main,        LV_OBJ_FLAG_HIDDEN);
    if (s_view_general)     lv_obj_add_flag(s_view_general,     LV_OBJ_FLAG_HIDDEN);
    if (s_view_about)       lv_obj_add_flag(s_view_about,       LV_OBJ_FLAG_HIDDEN);
    if (s_view_storage)     lv_obj_add_flag(s_view_storage,     LV_OBJ_FLAG_HIDDEN);   /* v1.2.3 */
    if (s_view_network)     lv_obj_add_flag(s_view_network,     LV_OBJ_FLAG_HIDDEN);
    if (s_view_display)     lv_obj_add_flag(s_view_display,     LV_OBJ_FLAG_HIDDEN);
    if (s_view_airplane)    lv_obj_add_flag(s_view_airplane,    LV_OBJ_FLAG_HIDDEN);
    if (s_view_swupdate)    lv_obj_add_flag(s_view_swupdate,    LV_OBJ_FLAG_HIDDEN);
    if (s_view_auto_update) lv_obj_add_flag(s_view_auto_update, LV_OBJ_FLAG_HIDDEN);
    if (s_view_power)       lv_obj_add_flag(s_view_power,       LV_OBJ_FLAG_HIDDEN);
}

/* ─── Apple Watch tarzı slide-in animasyonu ──────────────────────────────────
 * Settings sub-view'leri arasında geçişte yumuşak slide. LVGL view container'ı
 * ekrana göre konumlanır (lv_obj_align_to LV_PCT(100)/0/0/-LV_PCT(100)).
 *
 * Forward (örn. main → General): yeni view sağdan içeri kayar, eski view
 * ALTINDA kalır (üstüne yeni view biner). Anim bitince eski view hide edilir.
 *
 * Back (örn. General → main): mevcut view sağa doğru kayıp ekrandan çıkar;
 * altında zaten görünür olan main view ortaya çıkar. Anim bitince çıkan
 * view hide edilir.
 *
 * Süre 240 ms — app_manager'daki SCREEN_TRANSITION_MS (260 ms) ile uyumlu;
 * sub-view geçişi biraz daha hızlı çünkü scope daha küçük (kullanıcı
 * algısında "iki seviye derinlik = daha hızlı dönüş"). */
/* Settings sub-view slide. WEAR_ANIM_QUICK (180 ms) + apple_spring path:
 * spring physics curve sayesinde 180 ms snappy + natural physical hissi
 * verir; ease-out'tan farkı: hafif overshoot + snap-settle = "yaylı yer
 * değiştirme" hissi. Linear veya ease_out 180 ms uzun gelirdi; spring
 * curve perceptive süreyi ~120 ms gibi gösterir. */
/* v1.2.1 (2026-05-18): 110 ms → WEAR_ANIM_QUICK (180 ms).
 * 110 ms'de quintic eğri başlangıçta çok dik, snappy değil "tak" gibi
 * hissediliyordu — sub-page geçişi kullanıcı görmüyordu. 180 ms'de
 * eğrinin ilk %25'i (snappy) hâlâ <50 ms — algı olarak hızlı görünür,
 * ama anim'in glide-tail'i mevcut. wear_motion.h QUICK standardıyla hizalı. */
#define SETTINGS_SLIDE_MS WEAR_ANIM_QUICK   /* 180 ms */

static void anim_set_x_cb(void* obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t*)obj, v);
}

static void slide_done_hide_cb(lv_anim_t* a)
{
    lv_obj_t* obj = (lv_obj_t*)lv_anim_get_user_data(a);
    if (obj) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(obj, 0);   /* off-screen pos'u temizle, bir sonraki açılışta clean */
    }
}

/* PARALLAX SLIDE — Apple Watch tarzı: eski view sola çıkar, yeni view sağdan
 * girer, eş zamanlı. Eski single-view animasyonu (sadece yeni hareket eder)
 * split-screen efekti yaratıyordu (kullanıcı raporu 2026-05-03: video kanıtı).
 * Parallax'da her zaman tek bir view ortadadır, kullanıcı net bir geçiş hisseder. */

/* Forward: eski sola, yeni sağdan ortaya.
 *
 * v1.2.2 (2026-05-18): Race fix — hızlı geri-ileri navigasyonda paralel
 * animasyonlar `slide_done_hide_cb` user_data ile yanlış view'ı hide
 * ediyordu (UAF benzeri davranış: forward anim biter, hide cb yeni view'ı
 * vurur). Çözüm: yeni anim başlamadan önce target view'larda pending
 * anim'leri iptal et. */
static void slide_forward(lv_obj_t* old_view, lv_obj_t* new_view)
{
    if (!new_view) return;

    /* v1.2.2: pending anim'leri iptal et (race fix) */
    lv_anim_delete(new_view, anim_set_x_cb);
    if (old_view) lv_anim_delete(old_view, anim_set_x_cb);

    /* Yeni view setup: off-screen right (x=LV_HOR_RES), görünür, üstte. */
    lv_obj_set_x(new_view, LV_HOR_RES);
    lv_obj_clear_flag(new_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(new_view);

    /* Yeni view anim: x=LV_HOR_RES → 0 */
    lv_anim_t a_new;
    lv_anim_init(&a_new);
    lv_anim_set_var(&a_new, new_view);
    lv_anim_set_exec_cb(&a_new, anim_set_x_cb);
    lv_anim_set_values(&a_new, LV_HOR_RES, 0);
    lv_anim_set_duration(&a_new, SETTINGS_SLIDE_MS);
    lv_anim_set_path_cb(&a_new, wear_anim_path_ease_out_quint);
    lv_anim_start(&a_new);

    /* Eski view anim: x=0 → -LV_HOR_RES (sola kayıp ekrandan çıkar) */
    if (old_view) {
        lv_anim_t a_old;
        lv_anim_init(&a_old);
        lv_anim_set_var(&a_old, old_view);
        lv_anim_set_exec_cb(&a_old, anim_set_x_cb);
        lv_anim_set_values(&a_old, 0, -LV_HOR_RES);
        lv_anim_set_duration(&a_old, SETTINGS_SLIDE_MS);
        lv_anim_set_path_cb(&a_old, wear_anim_path_ease_out_quint);
        lv_anim_set_user_data(&a_old, old_view);
        lv_anim_set_completed_cb(&a_old, slide_done_hide_cb);
        lv_anim_start(&a_old);
    }
}

/* Back: eski sağa, target view soldan ortaya.
 * v1.2.2 race fix — slide_forward'daki gibi pending anim cancel. */
static void slide_back(lv_obj_t* old_view, lv_obj_t* target_view)
{
    if (!target_view) return;

    /* v1.2.2: pending anim'leri iptal et (race fix) */
    lv_anim_delete(target_view, anim_set_x_cb);
    if (old_view) lv_anim_delete(old_view, anim_set_x_cb);

    /* Target view setup: off-screen left (x=-LV_HOR_RES), görünür. */
    lv_obj_set_x(target_view, -LV_HOR_RES);
    lv_obj_clear_flag(target_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(target_view);

    /* Target view anim: x=-LV_HOR_RES → 0 */
    lv_anim_t a_t;
    lv_anim_init(&a_t);
    lv_anim_set_var(&a_t, target_view);
    lv_anim_set_exec_cb(&a_t, anim_set_x_cb);
    lv_anim_set_values(&a_t, -LV_HOR_RES, 0);
    lv_anim_set_duration(&a_t, SETTINGS_SLIDE_MS);
    lv_anim_set_path_cb(&a_t, wear_anim_path_ease_out_quint);
    lv_anim_start(&a_t);

    /* Eski view anim: x=0 → LV_HOR_RES (sağa kayıp ekrandan çıkar) */
    if (old_view) {
        lv_anim_t a_old;
        lv_anim_init(&a_old);
        lv_anim_set_var(&a_old, old_view);
        lv_anim_set_exec_cb(&a_old, anim_set_x_cb);
        lv_anim_set_values(&a_old, 0, LV_HOR_RES);
        lv_anim_set_duration(&a_old, SETTINGS_SLIDE_MS);
        lv_anim_set_path_cb(&a_old, wear_anim_path_ease_out_quint);
        lv_anim_set_user_data(&a_old, old_view);
        lv_anim_set_completed_cb(&a_old, slide_done_hide_cb);
        lv_anim_start(&a_old);
    }
}

/* Şu an görünür olan view'i tespit eder (anim için "old_view" parametresi). */
static lv_obj_t* current_visible_view(void)
{
    if (s_view_main        && !lv_obj_has_flag(s_view_main,        LV_OBJ_FLAG_HIDDEN)) return s_view_main;
    if (s_view_general     && !lv_obj_has_flag(s_view_general,     LV_OBJ_FLAG_HIDDEN)) return s_view_general;
    if (s_view_about       && !lv_obj_has_flag(s_view_about,       LV_OBJ_FLAG_HIDDEN)) return s_view_about;
    if (s_view_storage     && !lv_obj_has_flag(s_view_storage,     LV_OBJ_FLAG_HIDDEN)) return s_view_storage;   /* v1.2.3 */
    if (s_view_network     && !lv_obj_has_flag(s_view_network,     LV_OBJ_FLAG_HIDDEN)) return s_view_network;
    if (s_view_display     && !lv_obj_has_flag(s_view_display,     LV_OBJ_FLAG_HIDDEN)) return s_view_display;
    if (s_view_airplane    && !lv_obj_has_flag(s_view_airplane,    LV_OBJ_FLAG_HIDDEN)) return s_view_airplane;
    if (s_view_swupdate    && !lv_obj_has_flag(s_view_swupdate,    LV_OBJ_FLAG_HIDDEN)) return s_view_swupdate;
    if (s_view_auto_update && !lv_obj_has_flag(s_view_auto_update, LV_OBJ_FLAG_HIDDEN)) return s_view_auto_update;
    if (s_view_power       && !lv_obj_has_flag(s_view_power,       LV_OBJ_FLAG_HIDDEN)) return s_view_power;
    return NULL;
}

static void go_main_cb(lv_event_t* e)
{
    (void)e;
    close_conn_overlay();   /* WiFi ekranından ayrılırken overlay'i kapat */
    lv_obj_t* old = current_visible_view();
    if (old == s_view_main) return;   /* zaten main */
    slide_back(old, s_view_main);
}
static void go_general_cb(lv_event_t* e)
{
    (void)e;
    ensure_view(&s_view_general, build_general);
    lv_obj_t* old = current_visible_view();
    if (old == s_view_general) return;
    slide_forward(old, s_view_general);
}
static void go_about_cb(lv_event_t* e)
{
    (void)e;
    ensure_view(&s_view_about, build_about);
    lv_obj_t* old = current_visible_view();
    if (old == s_view_about) return;
    slide_forward(old, s_view_about);
}
/* v1.2.3 (2026-05-19): Storage alt sayfası — General → Storage navigation.
 * Slide forward animation, geri butonu go_general_cb ile. */
static void go_storage_cb(lv_event_t* e)
{
    (void)e;
    ensure_view(&s_view_storage, build_storage);
    lv_obj_t* old = current_visible_view();
    if (old == s_view_storage) return;
    slide_forward(old, s_view_storage);
}
/* Otomatik Güncellemeler alt sayfası — General nav row tıklaması açıyor.
 * Toggle state'i build sırasında NVS'ten okunup atanıyor; build sonrası
 * yeniden açılışlarda da güncel state yansıyor. */
static void go_auto_update_cb(lv_event_t* e)
{
    (void)e;
    ensure_view(&s_view_auto_update, build_auto_update);
    lv_obj_t* old = current_visible_view();
    if (old == s_view_auto_update) return;
    slide_forward(old, s_view_auto_update);
}
static void go_network_cb(lv_event_t* e)
{
    (void)e;
    ensure_view(&s_view_network, build_network);
    lv_obj_t* old = current_visible_view();
    if (old == s_view_network) return;
    slide_forward(old, s_view_network);
}
static void go_display_cb(lv_event_t* e)
{
    (void)e;
    ensure_view(&s_view_display, build_display);
    lv_obj_t* old = current_visible_view();
    if (old == s_view_display) return;
    slide_forward(old, s_view_display);
}
static void go_airplane_cb(lv_event_t* e)
{
    (void)e;
    ensure_view(&s_view_airplane, build_airplane);
    lv_obj_t* old = current_visible_view();
    if (old == s_view_airplane) return;
    slide_forward(old, s_view_airplane);
}

/* v1.1.1: Settings → Güç Rayları alt sayfası nav callback'i. Build sırasında
 * AXP register'ları okunup toggle state'leri set ediliyor — burada ek bir
 * "open hook" yok, sayfa açılırken UI dump-and-build pattern'i değil tam-
 * inşa pattern'ini takip ediyor (build_power on_start'ta çağrılır, view
 * persistent, sadece görünürlüğü değişir). Eğer kullanıcı sayfayı kapatıp
 * tekrar açarsa toggle'lar başlangıçtaki state'i gösterir; yeniden senkron
 * için spawn sırasında refresh fonksiyonu çağrılabilir, şimdilik gerekmiyor
 * çünkü rail değişikliği sadece bu sayfadan yapılıyor. */
static void go_power_cb(lv_event_t* e)
{
    (void)e;
    ensure_view(&s_view_power, build_power);
    lv_obj_t* old = current_visible_view();
    if (old == s_view_power) return;
    slide_forward(old, s_view_power);
}

/* Forward decl — swupdate view'ı build_software_update'te poll timer kuruyor;
 * geri tıklandığında timer'ı temizlemek gerekiyor. */
static void swupdate_start_polling(void);
static void swupdate_stop_polling(void);
static void swupdate_apply_state(const ota_status_t* st);
static void swupdate_set_button(const char* text);
static void go_swupdate_cb(lv_event_t* e)
{
    (void)e;
    ensure_view(&s_view_swupdate, build_software_update);
    lv_obj_t* old = current_visible_view();
    if (old == s_view_swupdate) return;
    /* View açılırken otomatik check tetikle — kullanıcının ayrıca buton'a
     * basmasına gerek kalmasın. */
    ota_service_check_async();
    /* UI'yi anında initial state'e çek, sonra timer poll'lar */
    ota_status_t st;
    ota_service_get_status(&st);
    swupdate_apply_state(&st);
    /* Polling timer'ı yeniden başlat — view ilk hidden olduğunda timer
     * kendi kendini siliyor (poll_cb içinde stop_polling). View tekrar
     * açıldığında start_polling çağırılmazsa state değişimleri UI'a
     * yansımaz; kullanıcı sadece view'a tekrar girince güncel state'i
     * görüyordu (kullanıcı raporu 2026-05-11 "indir ve yukle'ye basıyorum
     * ekran değişmiyor, geri gelip tekrar girince görüyorum"). */
    s_swu_last_state = st.state;
    swupdate_start_polling();
    slide_forward(old, s_view_swupdate);
}

static void reset_to_root(void)
{
    close_conn_overlay();
    hide_all();
    if (s_view_main) {
        lv_obj_set_x(s_view_main, 0);
        lv_obj_clear_flag(s_view_main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_view_main);
        /* Session 38: liste EN ÜSTTEN başlasın. Kaydırma konumu korunuyordu;
         * Ayarlar'dan çıkıp geri girince kullanıcı listenin ortasında
         * buluyordu kendini (Apple Watch da köke dönerken başa sarar). */
        if (s_main_content) lv_obj_scroll_to_y(s_main_content, 0, LV_ANIM_OFF);
    }
}

bool settings_app_handle_back(void)
{
    if (!s_view_main) return false;
    if (!lv_obj_has_flag(s_view_main, LV_OBJ_FLAG_HIDDEN)) return false;
    reset_to_root();
    return true;
}

// ─── WiFi scan callback (called from FreeRTOS task → lv_async_call) ──────────
typedef struct {
    wifi_ap_t aps[WIFI_SCAN_MAX_APS];
    uint8_t   count;
} wifi_scan_result_t;

/* forward declare so wifi_list_update can register it as a row callback */
static void ap_row_tap_cb(lv_event_t* e);

static void wifi_list_update(void* data)
{
    wifi_scan_result_t* res = (wifi_scan_result_t*)data;
    if (!s_wifi_list) { free(res); return; }

    /* Persist scan results for tap callbacks + post-connect refresh */
    s_last_ap_count = res->count;
    if (res->count > 0)
        memcpy(s_last_aps, res->aps, res->count * sizeof(wifi_ap_t));

    /* Boot'taki auto-connect veya Settings dışından gelen bağlantılar için:
     * scan sırasında esp_wifi_sta_get_ap_info() hangi AP'nin bağlı olduğunu
     * zaten tespit ediyor (aps[i].connected == true). Eğer s_connected_ssid
     * henüz dolu değilse, otomatik doldur. Bu sayede reboot sonrası Settings
     * Wi-Fi ekranı açılınca "Connected" satırı doğru görünür ve kullanıcıya
     * tekrar parola sorulmaz. */
    if (s_connected_ssid[0] == '\0') {
        for (uint8_t i = 0; i < res->count; i++) {
            if (res->aps[i].connected) {
                strlcpy(s_connected_ssid, res->aps[i].ssid,
                        sizeof(s_connected_ssid));
                break;
            }
        }
    }

    /* Invalidate list's OLD bounds before wiping children. Parents are all
     * transparent (panel/sbox/view), so LVGL's default invalidate tied to
     * the shrunken post-clean bounds would leave ghost row pixels. */
    lv_obj_invalidate(s_wifi_list);
    if (s_view_network) lv_obj_invalidate(s_view_network);

    lv_obj_clean(s_wifi_list);

    /* ── Bağlı ağ satırı (WiFi toggle altında ayrı gösterim) ──────────────
     * Bağlı SSID varsa s_connected_row'u doldurup göster; ana listede
     * tekrar görünmesin diye aşağıda iterasyon sırasında atlanacak.
     * NETWORKS etiketi ve liste, bağlı satır varsa aşağı kayar. */
    const bool have_connected = (s_connected_ssid[0] != '\0');
    const wifi_ap_t* conn_ap  = NULL;
    if (have_connected) {
        for (uint8_t i = 0; i < res->count; i++) {
            if (strcmp(res->aps[i].ssid, s_connected_ssid) == 0) {
                conn_ap = &res->aps[i];
                break;
            }
        }
    }

    if (s_connected_row) {
        lv_obj_clean(s_connected_row);
        if (have_connected) {
            /* Signal bars (conn_ap yoksa RSSI bilinmediği için 3 bar varsay) */
            int bars = 3;
            if (conn_ap) {
                if (conn_ap->rssi >= -60)      bars = 3;
                else if (conn_ap->rssi >= -72) bars = 2;
                else                            bars = 1;
            }
#ifdef BOARD_VARIANT_LCD128
            const int32_t BW = 4, BH_MAX = 16, BH_STEP = 4, BGAP = 3, BAR_X0 = 10;
#else
            const int32_t BW = 5, BH_MAX = 22, BH_STEP = 6, BGAP = 4, BAR_X0 = 14;
#endif
            for (int b = 0; b < 3; b++) {
                int32_t bh = BH_MAX - (2 - b) * BH_STEP;
                lv_obj_t* br = lv_obj_create(s_connected_row);
                lv_obj_remove_style_all(br);
                lv_obj_set_size(br, BW, bh);
#ifdef BOARD_VARIANT_LCD128
                lv_obj_set_pos(br, BAR_X0 + b * (BW + BGAP),
                               (48 - BH_MAX) / 2 + (BH_MAX - bh));
#else
                lv_obj_set_pos(br, BAR_X0 + b * (BW + BGAP),
                               (68 - BH_MAX) / 2 + (BH_MAX - bh));
#endif
                lv_color_t col = (b < bars) ? lv_color_white()
                                             : lv_color_make(0x44, 0x44, 0x48);
                lv_obj_set_style_bg_color(br, col, 0);
                lv_obj_set_style_bg_opa(br, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(br, 2, 0);
                lv_obj_set_style_border_width(br, 0, 0);
                lv_obj_clear_flag(br, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                                       LV_OBJ_FLAG_SCROLLABLE));
            }

            /* SSID etiketi — long_mode set'i width'ten ÖNCE olmalı, aksi
             * halde LVGL state machine'i mode'u uygulamadan label width'i
             * lock'lar → uzun SSID'lerde scroll-circular wraparound. */
            lv_obj_t* ssid_lbl = lv_label_create(s_connected_row);
            lv_obj_set_style_text_color(ssid_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
            lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_14, 0);
            lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(ssid_lbl, 92);
#else
            lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_20, 0);
            lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(ssid_lbl, 210);
#endif
            lv_label_set_text(ssid_lbl, s_connected_ssid);
#ifdef BOARD_VARIANT_LCD128
            lv_obj_align(ssid_lbl, LV_ALIGN_LEFT_MID, 30, 0);

            /* "Connected" etiketi — iOS yeşili */
#else
            lv_obj_align(ssid_lbl, LV_ALIGN_LEFT_MID, 50, 0);

            /* "Connected" etiketi — iOS yeşili */
#endif
            lv_obj_t* conn_lbl = lv_label_create(s_connected_row);
            lv_obj_set_style_text_color(conn_lbl,
                lv_color_make(0x30, 0xD1, 0x58), 0);
#ifdef BOARD_VARIANT_LCD128
            lv_obj_set_style_text_font(conn_lbl,
                &lv_font_montserrat_12, 0);
            lv_obj_align(conn_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
#else
            lv_obj_set_style_text_font(conn_lbl,
                &lv_font_montserrat_16, 0);
            lv_obj_align(conn_lbl, LV_ALIGN_RIGHT_MID, -14, 0);
#endif
            lv_label_set_text(conn_lbl, "Connected");

            lv_obj_clear_flag(s_connected_row, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_connected_row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* NETWORKS etiketi + liste y konumu: bağlı satır varsa aşağı kayar */
#ifdef BOARD_VARIANT_LCD128
    const int32_t NET_Y_BASE  = 72;
    const int32_t LIST_Y_BASE = 92;
#else
    const int32_t NET_Y_BASE  = 110;
    const int32_t LIST_Y_BASE = 132;
#endif
#ifdef BOARD_VARIANT_LCD128
    const int32_t SHIFT       = 58;  /* bağlı satır 48 + 10 boşluk */
#else
    const int32_t SHIFT       = 78;  /* bağlı satır 68 + 10 boşluk */
#endif
    if (s_wifi_section_lbl)
        lv_obj_set_y(s_wifi_section_lbl, have_connected ? NET_Y_BASE + SHIFT
                                                         : NET_Y_BASE);
    lv_obj_set_y(s_wifi_list, have_connected ? LIST_Y_BASE + SHIFT
                                              : LIST_Y_BASE);

    if (res->count == 0) {
        lv_obj_t* lbl = lv_label_create(s_wifi_list);
        lv_obj_set_style_text_color(lbl, lv_color_make(0x8E, 0x8E, 0x93), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(lbl, 16, 12);
#else
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_pos(lbl, 20, 16);
#endif
        lv_label_set_text(lbl, have_connected ? "No Other Networks"
                                               : "No Networks Found");
        free(res);
        return;
    }

#ifdef BOARD_VARIANT_LCD128
    const int32_t ROW_H   = 44;
#else
    const int32_t ROW_H   = 68;
#endif
    const int32_t ROW_GAP = 6;
    int32_t y = 0;

    for (uint8_t i = 0; i < res->count; i++) {
        wifi_ap_t* ap = &res->aps[i];

        /* Override connected flag with the authoritative global */
        ap->connected = (s_connected_ssid[0] != '\0') &&
                        (strcmp(ap->ssid, s_connected_ssid) == 0);

        /* Bağlı ağ ana listede tekrar görünmesin; üstte s_connected_row
         * olarak zaten ayrıca gösteriliyor (iOS Watch davranışı). */
        if (ap->connected) continue;

        /* Row card */
        lv_obj_t* row = lv_obj_create(s_wifi_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, SETTINGS_SAFE_W, ROW_H);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_color(row, lv_color_make(0x1C, 0x1C, 0x1E), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_make(0x2C, 0x2C, 0x2E),
                                   LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_make(0x2C, 0x2C, 0x2E), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
        /* index stored as user_data for ap_row_tap_cb */
        lv_obj_add_event_cb(row, ap_row_tap_cb, LV_EVENT_CLICKED,
                            (void*)(uintptr_t)i);

        /* Signal bars: 3 vertical rectangles, growing in height */
        int bars = 1;
        if (ap->rssi >= -60) bars = 3;
        else if (ap->rssi >= -72) bars = 2;

#ifdef BOARD_VARIANT_LCD128
        const int32_t BW = 4, BH_MAX = 16, BH_STEP = 4, BGAP = 3;
        const int32_t BAR_X0 = 10;
#else
        const int32_t BW = 5, BH_MAX = 22, BH_STEP = 6, BGAP = 4;
        const int32_t BAR_X0 = 14;
#endif
        for (int b = 0; b < 3; b++) {
            int32_t bh = BH_MAX - (2 - b) * BH_STEP;  /* 10, 16, 22 */
            lv_obj_t* br = lv_obj_create(row);
            lv_obj_remove_style_all(br);
            lv_obj_set_size(br, BW, bh);
            lv_obj_set_pos(br, BAR_X0 + b * (BW + BGAP),
                           (ROW_H - BH_MAX) / 2 + (BH_MAX - bh));
            lv_color_t col = (b < bars) ? lv_color_white()
                                        : lv_color_make(0x44, 0x44, 0x48);
            lv_obj_set_style_bg_color(br, col, 0);
            lv_obj_set_style_bg_opa(br, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(br, 2, 0);
            lv_obj_set_style_border_width(br, 0, 0);
            lv_obj_clear_flag(br, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                                   LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_add_flag(br, LV_OBJ_FLAG_EVENT_BUBBLE);
        }

        /* SSID label — long_mode set'i width'ten ÖNCE olmalı (LVGL 9.2). */
        lv_obj_t* ssid = lv_label_create(row);
        lv_obj_set_style_text_color(ssid, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_style_text_font(ssid, &lv_font_montserrat_14, 0);
#else
        lv_obj_set_style_text_font(ssid, &lv_font_montserrat_20, 0);
#endif
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_width(ssid, 120);
#else
        lv_obj_set_width(ssid, 280);
#endif
        lv_label_set_text(ssid, ap->ssid[0] ? ap->ssid : "(hidden)");
#ifdef BOARD_VARIANT_LCD128
        lv_obj_align(ssid, LV_ALIGN_LEFT_MID, 36, 0);
#else
        lv_obj_align(ssid, LV_ALIGN_LEFT_MID, 50, 0);
#endif
        lv_obj_clear_flag(ssid, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(ssid, LV_OBJ_FLAG_EVENT_BUBBLE);

        /* Checkmark if connected */
        if (ap->connected) {
            lv_obj_t* chk = lv_label_create(row);
            lv_obj_set_style_text_color(chk, lv_color_make(0x30, 0xD1, 0x58), 0);
            lv_obj_set_style_text_font(chk, &lv_font_montserrat_20, 0);
            lv_obj_align(chk, LV_ALIGN_RIGHT_MID, -14, 0);
            lv_label_set_text(chk, LV_SYMBOL_OK);
            lv_obj_clear_flag(chk, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(chk, LV_OBJ_FLAG_EVENT_BUBBLE);
        }

        /* Lock indicator for secured networks */
        if (ap->secured) {
            int32_t lock_x = ap->connected ? -42 : -14;
            lv_obj_t* lock = lv_label_create(row);
            lv_obj_set_style_text_color(lock, lv_color_make(0x8E, 0x8E, 0x93), 0);
            lv_obj_set_style_text_font(lock, &lv_font_montserrat_16, 0);
            lv_obj_align(lock, LV_ALIGN_RIGHT_MID, lock_x, 0);
            lv_label_set_text(lock, LV_SYMBOL_EYE_CLOSE);
            lv_obj_clear_flag(lock, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(lock, LV_OBJ_FLAG_EVENT_BUBBLE);
        }

        y += ROW_H + ROW_GAP;
    }

    /* Mirror updated connected flags back to s_last_aps */
    if (res->count > 0)
        memcpy(s_last_aps, res->aps, res->count * sizeof(wifi_ap_t));

    free(res);
}

static void wifi_scan_done_cb(const wifi_ap_t* aps, uint8_t count, void* ud)
{
    (void)ud;
    wifi_scan_result_t* res =
        (wifi_scan_result_t*)malloc(sizeof(wifi_scan_result_t));
    if (!res) return;
    res->count = count;
    if (count > 0)
        memcpy(res->aps, aps, count * sizeof(wifi_ap_t));
    lv_async_call(wifi_list_update, res);
}

// ─── Bağlantı overlay (şifre girişi + bağlanıyor ekranı) ─────────────────────

static void close_conn_overlay(void)
{
    ui_service_lock_swipe(false);
    if (s_close_timer) {
        lv_timer_del(s_close_timer);
        s_close_timer = NULL;
    }
    /* Şifre reveal zamanlayıcısı ve tampon */
    if (s_pw_hide_timer) {
        lv_timer_del(s_pw_hide_timer);
        s_pw_hide_timer = NULL;
    }
    if (s_commit_timer) {
        lv_timer_del(s_commit_timer);
        s_commit_timer = NULL;
    }
    s_pw_len = 0;
    secure_zero(s_pw_real, sizeof(s_pw_real));   /* audit #3: BSS plaintext leak fix */
    if (s_conn_overlay) {
        lv_obj_del(s_conn_overlay);
        s_conn_overlay = NULL;
    }
    s_conn_status_lbl = NULL;
    s_pw_textarea     = NULL;
    s_pw_scribble     = NULL;
    s_pw_hint_lbl     = NULL;
    s_pw_keyboard     = NULL;
    s_pw_del_btn      = NULL;
    s_pw_ok_btn       = NULL;
    s_pw_space_btn    = NULL;
    s_pw_mode_btn     = NULL;
    s_pw_mode_btn_lbl = NULL;
    s_pw_input_mode   = PW_INPUT_SCRIBBLE;
    s_stroke_n        = 0;
    s_stroke_count    = 0;
    s_cur_stroke_start = 0;
    s_stroke_live     = false;
}

static void conn_close_timer_cb(lv_timer_t* t)
{
    (void)t;
    s_close_timer = NULL;
    close_conn_overlay();
}

/* ── Bağlantı sonucu (lv_async_call ile LVGL task'ında çalışır) ────────── */
typedef struct { bool ok; char ssid[33]; } conn_result_t;

static void ap_connect_result_ui(void* data)
{
    conn_result_t* r = (conn_result_t*)data;

    if (r->ok) {
        strlcpy(s_connected_ssid, r->ssid, sizeof(s_connected_ssid));
    }

    /* Listeyi güncelle — connected flag'ini yeniden değerlendir */
    if (s_wifi_list && s_last_ap_count > 0) {
        wifi_scan_result_t* copy =
            (wifi_scan_result_t*)malloc(sizeof(wifi_scan_result_t));
        if (copy) {
            copy->count = s_last_ap_count;
            memcpy(copy->aps, s_last_aps, s_last_ap_count * sizeof(wifi_ap_t));
            wifi_list_update(copy);  /* frees copy */
        }
    }

    /* Overlay hâlâ açıksa sonucu göster ve kapat */
    if (s_conn_overlay && s_conn_status_lbl) {
        lv_label_set_text(s_conn_status_lbl,
                          r->ok ? "Connected!" : "Connection Failed");
        lv_color_t col = r->ok ? lv_color_make(0x30, 0xD1, 0x58)
                                : lv_color_make(0xFF, 0x3B, 0x30);
        lv_obj_set_style_text_color(s_conn_status_lbl, col, 0);
        s_close_timer = lv_timer_create(conn_close_timer_cb, 1800, NULL);
        lv_timer_set_repeat_count(s_close_timer, 1);
    }

    free(r);
}

static void on_ap_connect_result(bool connected, void* ud)
{
    (void)ud;
    if (connected) {
        BaseType_t r = xTaskCreate(ntp_sync_task, "ntp_sync", 4096, NULL, 4, NULL);
        if (r != pdPASS) ESP_LOGW(TAG, "xTaskCreate(ntp_sync) failed; time will sync next reboot");
    }
    conn_result_t* r = (conn_result_t*)malloc(sizeof(conn_result_t));
    if (!r) return;
    r->ok = connected;
    strlcpy(r->ssid, s_pending_ssid, sizeof(r->ssid));
    lv_async_call(ap_connect_result_ui, r);
}

/* ── Overlay oluşturma yardımcıları ────────────────────────────────────── */

/* Bağlanıyor ekranı — overlay oluşturur veya mevcut içeriği değiştirir */
/* Overlay parent olarak her zaman aktif screen kullanılır.
 * Bu sayede scrollable sbox'ın scroll-to-focus mekanizması devreye girmez. */
static lv_obj_t* make_overlay_base(void)
{
    lv_obj_t* ov = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(ov);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(ov, 240, 240);
#else
    lv_obj_set_size(ov, 410, 502);
#endif
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_clear_flag(ov, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                           LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    return ov;
}

static void show_connecting_overlay(const char* ssid)
{
    close_conn_overlay();

    lv_obj_t* ov = make_overlay_base();
    s_conn_overlay = ov;

    /* İptal butonu — absolute pos */
    lv_obj_t* cancel = lv_label_create(ov);
    lv_obj_set_style_text_color(cancel, lv_color_make(0x0A, 0x84, 0xFF), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(cancel, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cancel, 48, 16);
    lv_label_set_text(cancel, LV_SYMBOL_LEFT " Cancel");
#else
    lv_obj_set_style_text_font(cancel, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(cancel, 16, 26);
    lv_label_set_text(cancel, LV_SYMBOL_LEFT "  Cancel");
#endif
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    /* v1.4.64: 14 → 28 (Apple HIG min 44×44 touch hedefi) */
    lv_obj_set_ext_click_area(cancel, 28);
    lv_obj_add_event_cb(cancel, [](lv_event_t* e) {
        (void)e; close_conn_overlay();
    }, LV_EVENT_CLICKED, NULL);

    /* Durum metni — ortada */
    char buf[80];
    snprintf(buf, sizeof(buf), "Connecting to\n\"%s\"", ssid);
    s_conn_status_lbl = lv_label_create(ov);
    lv_obj_set_style_text_color(s_conn_status_lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_conn_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_conn_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_conn_status_lbl, 180, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_conn_status_lbl, 30, 100);
#else
    lv_obj_set_style_text_font(s_conn_status_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_conn_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_conn_status_lbl, 360, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_conn_status_lbl, 25, 220);
#endif
    lv_label_set_text(s_conn_status_lbl, buf);
}

/* ── Şifre tamponu yardımcıları (custom reveal/mask) ───────────────────── */

/* Textarea ekranını real buffer'dan kurar. show_last_idx = -1 ise HEPSI
 * '*'; >=0 ise o konumdaki gerçek karakter açık, diğerleri '*'. */
static void pw_render_display(int show_last_idx)
{
    if (!s_pw_textarea) return;
    char disp[PW_MAX_LEN + 1];
    for (uint8_t i = 0; i < s_pw_len; i++) {
        disp[i] = ((int)i == show_last_idx) ? s_pw_real[i] : '*';
    }
    disp[s_pw_len] = 0;
    lv_textarea_set_text(s_pw_textarea, disp);
}

/* Eski otomatik gizleme callback'i — şu an kullanılmıyor (son karakter
 * bir sonraki karakter gelene kadar açık kalıyor). İleride tekrar
 * auto-hide davranışına dönmek istersek hazır duruyor. */
__attribute__((unused))
static void pw_hide_timer_cb(lv_timer_t* t)
{
    (void)t;
    s_pw_hide_timer = NULL;
    pw_render_display(-1);
}

static void pw_cancel_hide_timer(void)
{
    if (s_pw_hide_timer) {
        lv_timer_del(s_pw_hide_timer);
        s_pw_hide_timer = NULL;
    }
}

/* Yeni karakteri append et, son karakteri bir sonraki karakter gelene
 * kadar AÇIK bırak (otomatik maskeleme yok). Kullanıcı "karakter çok hızlı
 * noktaya dönüşüyor" şikayeti için: artık hiç otomatik gizlenmiyor — son
 * girilen harf, yeni bir harf eklenene veya DEL'e basılana kadar okunaklı
 * kalır. Bu Apple Watch Scribble'daki davranışın en konservatif biçimi. */
static void pw_append_char(char c)
{
    if (s_pw_len >= PW_MAX_LEN) return;
    s_pw_real[s_pw_len++] = c;
    s_pw_real[s_pw_len]   = 0;

    /* Varsa eski timer'ı temizle — artık kullanılmıyor ama güvenli taraf. */
    pw_cancel_hide_timer();
    pw_render_display((int)s_pw_len - 1);
    /* Otomatik gizleme timer'ı kurulmuyor. Bir sonraki pw_append_char'da
     * render sadece yeni son karakteri gösterecek; öncekiler '*'. */
}

/* Son karakteri sil — ekranda da maskeli '*' kümesinden bir azalsın. */
static void pw_backspace(void)
{
    if (s_pw_len == 0) return;
    s_pw_len--;
    s_pw_real[s_pw_len] = 0;
    pw_cancel_hide_timer();
    pw_render_display(-1);
}

static void pw_reset_buffer(void)
{
    s_pw_len = 0;
    secure_zero(s_pw_real, sizeof(s_pw_real));   /* audit #3: BSS plaintext leak fix */
    pw_cancel_hide_timer();
}

/* ── Şifre overlay event'leri ───────────────────────────────────────────── */
static void pw_do_connect(void)
{
    if (!s_pw_textarea) return;
    char pw[PW_MAX_LEN + 1];
    strlcpy(pw, s_pw_real, sizeof(pw));
    show_connecting_overlay(s_pending_ssid);
    wifi_service_connect_to_ap(s_pending_ssid, pw, on_ap_connect_result, NULL);
    /* v1.4.55 audit #3: lokal stack buffer'ı da temizle (return sonrası
     * stack frame reuse → password residual'i sızabilir) */
    secure_zero(pw, sizeof(pw));
}

/* ── Klavye modunda tuş basıldığında çağrılır ───────────────────────────────
 * lv_keyboard varsayılan davranışında "abc/ABC/1#" tuşları kendi mod
 * değişimini yapar (default handler bunu intercept eder ve return olur).
 * Sıradan karakter ve özel sembol tuşları VALUE_CHANGED event'i atar; biz
 * burada metni okuyup pw_real buffer'a uyguluyoruz. lv_keyboard'a textarea
 * bağlamıyoruz çünkü s_pw_textarea maskelenmiş ('*') gösterim yapıyor —
 * gerçek karakteri kendi buffer'ımıza yazmamız gerek. */
static void pw_keyboard_event_cb(lv_event_t* e)
{
    lv_obj_t* kb = (lv_obj_t*)lv_event_get_target(e);
    uint32_t btn_id = lv_keyboard_get_selected_button(kb);
    if (btn_id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char* txt = lv_keyboard_get_button_text(kb, btn_id);
    if (!txt || !*txt) return;

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        pw_backspace();
        return;
    }
    if (strcmp(txt, LV_SYMBOL_NEW_LINE) == 0 ||
        strcmp(txt, LV_SYMBOL_OK)       == 0 ||
        strcmp(txt, "Join")             == 0) {
        pw_do_connect();
        return;
    }
    /* iPhone tarzı mod tuşları: ↑ (shift) → büyük/küçük toggle,
     *                          "123"     → sayı/sembol moduna geç,
     *                          "ABC"     → harf moduna (küçük) dön. */
    if (strcmp(txt, LV_SYMBOL_UP) == 0) {
        lv_keyboard_mode_t cur = lv_keyboard_get_mode(kb);
        lv_keyboard_set_mode(kb, cur == LV_KEYBOARD_MODE_TEXT_UPPER
                                     ? LV_KEYBOARD_MODE_TEXT_LOWER
                                     : LV_KEYBOARD_MODE_TEXT_UPPER);
        return;
    }
    if (strcmp(txt, "123") == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_SPECIAL);
        return;
    }
    if (strcmp(txt, "ABC") == 0) {
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        return;
    }
    /* Eski default handler'a kalabilen yön/klavye sembolleri — guard. */
    if (strcmp(txt, LV_SYMBOL_LEFT)  == 0 ||
        strcmp(txt, LV_SYMBOL_RIGHT) == 0 ||
        strcmp(txt, LV_SYMBOL_KEYBOARD) == 0) {
        return;
    }
    /* Sıradan karakter — UTF-8 farkındaysan da basit ASCII parolada her tuş
     * tek byte'lık. txt birden çok karakter içerebilir (ör. "  " space tuşu
     * "  " olabilir); döngüyle ekle. */
    for (const char* p = txt; *p; ++p) {
        pw_append_char(*p);
    }
}

/* Scribble ↔ Klavye modu arası geçiş. Hangi modunsak diğerinin UI'sini
 * yaratıp/yıkar ve alt baradaki mod-toggle butonunun etiketini günceller. */
static void pw_set_input_mode(pw_input_mode_t mode);

static void pw_mode_toggle_cb(lv_event_t* e)
{
    (void)e;
    pw_set_input_mode(s_pw_input_mode == PW_INPUT_SCRIBBLE
                          ? PW_INPUT_KEYBOARD
                          : PW_INPUT_SCRIBBLE);
}

static void pw_set_input_mode(pw_input_mode_t mode)
{
    if (mode == s_pw_input_mode && s_pw_keyboard == NULL && s_pw_scribble != NULL) {
        return;  // already in scribble, nothing to do
    }
    s_pw_input_mode = mode;

    if (mode == PW_INPUT_KEYBOARD) {
        // Scribble panel ve scribble-only butonları gizle.
        if (s_pw_scribble)  lv_obj_add_flag(s_pw_scribble,  LV_OBJ_FLAG_HIDDEN);
        if (s_pw_del_btn)   lv_obj_add_flag(s_pw_del_btn,   LV_OBJ_FLAG_HIDDEN);
        if (s_pw_ok_btn)    lv_obj_add_flag(s_pw_ok_btn,    LV_OBJ_FLAG_HIDDEN);
        if (s_pw_space_btn) lv_obj_add_flag(s_pw_space_btn, LV_OBJ_FLAG_HIDDEN);
        // KRİTİK: Tüm overlay'i invalidate → eski OK butonunun yeşil
        // piksel kalıntıları (y=416 civarı) tamamen temizlenir.
        if (s_conn_overlay) lv_obj_invalidate(s_conn_overlay);
#ifdef BOARD_VARIANT_LCD128
        /* Klavye alt kenarı 188'de bitiyor — kalem butonu alt-ortaya. */
        if (s_pw_mode_btn) lv_obj_set_pos(s_pw_mode_btn, 105, 200);
#endif

        // Klavyeyi yarat (yoksa)
        if (!s_pw_keyboard && s_conn_overlay) {
            // ─── iPhone tarzı 4 satırlık düzen ────────────────────────
            // Row1: 10 harf | Row2: 9 harf (doğal olarak biraz girintili
            // görünür) | Row3: ⇧ + 7 harf + ⌫ | Row4: 123 + space + Join.
            // CTRL bayrağında: bit0..2 = genişlik (1..7), CUSTOM_1 = 0x4000
            // → fonksiyon tuşu işareti (renk override için).
            #define KB_K  ((lv_buttonmatrix_ctrl_t)1)                                       /* harf, w=1 */
            #define KB_F1 ((lv_buttonmatrix_ctrl_t)(1 | LV_BUTTONMATRIX_CTRL_CUSTOM_1))     /* fonksiyon, w=1 */
            #define KB_F2 ((lv_buttonmatrix_ctrl_t)(2 | LV_BUTTONMATRIX_CTRL_CUSTOM_1))     /* fonksiyon, w=2 */
            #define KB_SP ((lv_buttonmatrix_ctrl_t)6)                                       /* space, w=6 */

            // Küçük harf modu — shift (↑) sol altta, 123 son satırda solda,
            // space ortada, Join sağda.
            static const char* const kb_lc_map[] = {
                "q","w","e","r","t","y","u","i","o","p", "\n",
                "a","s","d","f","g","h","j","k","l", "\n",
                LV_SYMBOL_UP,"z","x","c","v","b","n","m", LV_SYMBOL_BACKSPACE, "\n",
                "123"," ","Join",""
            };
            static const lv_buttonmatrix_ctrl_t kb_lc_ctrl[] = {
                KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,
                KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,
                KB_F2,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_F2,
                KB_F2,KB_SP,KB_F2
            };

            // Büyük harf modu (shift bir kez basılınca aktif).
            static const char* const kb_uc_map[] = {
                "Q","W","E","R","T","Y","U","I","O","P", "\n",
                "A","S","D","F","G","H","J","K","L", "\n",
                LV_SYMBOL_UP,"Z","X","C","V","B","N","M", LV_SYMBOL_BACKSPACE, "\n",
                "123"," ","Join",""
            };
            static const lv_buttonmatrix_ctrl_t kb_uc_ctrl[] = {
                KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,
                KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,
                KB_F2,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_F2,
                KB_F2,KB_SP,KB_F2
            };

            // Sayı/sembol modu — soldaki "ABC" harf moduna döner.
            static const char* const kb_num_map[] = {
                "1","2","3","4","5","6","7","8","9","0", "\n",
                "-","/",":",";","(",")","$","&","@","\"", "\n",
                ".",",","?","!","'","\"","_","+", LV_SYMBOL_BACKSPACE, "\n",
                "ABC"," ","Join",""
            };
            static const lv_buttonmatrix_ctrl_t kb_num_ctrl[] = {
                KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,
                KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,
                KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_K,KB_F2,
                KB_F2,KB_SP,KB_F2
            };
            #undef KB_K
            #undef KB_F1
            #undef KB_F2
            #undef KB_SP

            s_pw_keyboard = lv_keyboard_create(s_conn_overlay);
            // KRİTİK: lv_keyboard constructor'ı LV_ALIGN_BOTTOM_MID
            // ayarlıyor → set_pos görmezden geliniyordu. TOP_LEFT'e al.
            lv_obj_set_align(s_pw_keyboard, LV_ALIGN_TOP_LEFT);
            // 4 satırlı düzen için biraz daha yüksek alan: y=130..390
            // (260 px). Yuvarlak köşe maskesi (≥y~395) altta 5 px boşluk.
#ifdef BOARD_VARIANT_LCD128
            lv_obj_set_size(s_pw_keyboard, 208, 132);
            lv_obj_set_pos(s_pw_keyboard, 16, 56);
#else
            lv_obj_set_size(s_pw_keyboard, 410, 260);
            lv_obj_set_pos(s_pw_keyboard, 0, 130);
#endif

            // ─── iPhone-vari klavye stili (kahverengimsi sıcak ton) ─────
            // Referans: iOS klavye sıcak tema.
            // Klavye gövdesi: çerçevesiz, sıcak koyu kahve zemin.
            lv_obj_set_style_bg_color(s_pw_keyboard, lv_color_hex(0x4E443B), 0);
            lv_obj_set_style_bg_opa(s_pw_keyboard, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(s_pw_keyboard, 0, 0);
            lv_obj_set_style_radius(s_pw_keyboard, 0, 0);
            // 4 satıra sığsın diye paddingleri biraz kıstık.
            lv_obj_set_style_pad_row(s_pw_keyboard, 6, 0);
            lv_obj_set_style_pad_column(s_pw_keyboard, 4, 0);
            lv_obj_set_style_pad_top(s_pw_keyboard, 5, 0);
            lv_obj_set_style_pad_bottom(s_pw_keyboard, 5, 0);
            lv_obj_set_style_pad_left(s_pw_keyboard, 3, 0);
            lv_obj_set_style_pad_right(s_pw_keyboard, 3, 0);

            // Harf tuşu: yuvarlak köşeli, sıcak gri-kahve zemin, beyaz
            // bold metin, hafif alt-gölge (kabarık his).
            lv_obj_set_style_bg_color(s_pw_keyboard, lv_color_hex(0x8C8277), LV_PART_ITEMS);
            lv_obj_set_style_bg_opa(s_pw_keyboard, LV_OPA_COVER, LV_PART_ITEMS);
            lv_obj_set_style_text_color(s_pw_keyboard, lv_color_white(), LV_PART_ITEMS);
            lv_obj_set_style_radius(s_pw_keyboard, 8, LV_PART_ITEMS);
            lv_obj_set_style_border_width(s_pw_keyboard, 0, LV_PART_ITEMS);
            lv_obj_set_style_shadow_width(s_pw_keyboard, 3, LV_PART_ITEMS);
            lv_obj_set_style_shadow_offset_y(s_pw_keyboard, 1, LV_PART_ITEMS);
            lv_obj_set_style_shadow_color(s_pw_keyboard, lv_color_black(), LV_PART_ITEMS);
            lv_obj_set_style_shadow_opa(s_pw_keyboard, LV_OPA_40, LV_PART_ITEMS);
            // Basılı durum: iOS shift-active gibi krem-beyaz.
            lv_obj_set_style_bg_color(s_pw_keyboard, lv_color_hex(0xEAE3D6),
                                      LV_PART_ITEMS | LV_STATE_PRESSED);
            lv_obj_set_style_text_color(s_pw_keyboard, lv_color_hex(0x2B2520),
                                        LV_PART_ITEMS | LV_STATE_PRESSED);
            // 4 satır + dar tuşlar — fontu 16'ya çekiyoruz ki "Join"/"123"
            // gibi metinler de tuş içine sığsın (Montserrat 18 lv_conf'da
            // etkin değil; 16 zaten yüklü).
            lv_obj_set_style_text_font(s_pw_keyboard,
                                       &lv_font_montserrat_16,
                                       LV_PART_ITEMS);

            lv_keyboard_set_map(s_pw_keyboard,
                                LV_KEYBOARD_MODE_TEXT_LOWER,
                                kb_lc_map, kb_lc_ctrl);
            lv_keyboard_set_map(s_pw_keyboard,
                                LV_KEYBOARD_MODE_TEXT_UPPER,
                                kb_uc_map, kb_uc_ctrl);
            lv_keyboard_set_map(s_pw_keyboard,
                                LV_KEYBOARD_MODE_SPECIAL,
                                kb_num_map, kb_num_ctrl);
            lv_keyboard_set_mode(s_pw_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
            // KRİTİK: textarea'ya bağlama → varsayılan handler textarea'ya
            // yazma yapmaz, biz kendi pw_real'imize yazarız.
            lv_keyboard_set_textarea(s_pw_keyboard, NULL);
            // Default event cb'yi kaldır → "ABC"/"abc"/"1#" otomatik mod
            // değişimini biz pw_keyboard_event_cb içinde kendi etiketlerimize
            // (↑ / 123 / ABC) göre yapıyoruz; çakışmasın.
            lv_obj_remove_event_cb(s_pw_keyboard, lv_keyboard_def_event_cb);
            lv_obj_add_event_cb(s_pw_keyboard, pw_keyboard_event_cb,
                                LV_EVENT_VALUE_CHANGED, NULL);
            // Fonksiyon tuşları (1#/ABC, ⌫) için per-buton renk override.
            // CUSTOM_1 flag'li tuşlar koyu-gri (#3A3A3C) çizilir; iOS'taki
            // shift/backspace tonu. Çizim aşamasında fill_dsc'yi düzenliyoruz.
            lv_obj_add_flag(s_pw_keyboard, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
            lv_obj_add_event_cb(s_pw_keyboard, [](lv_event_t* e) {
                lv_obj_t* kb = (lv_obj_t*)lv_event_get_current_target(e);
                lv_draw_task_t* t = (lv_draw_task_t*)lv_event_get_param(e);
                /* lv_draw_task_t opaque — public accessor lazım. */
                lv_draw_dsc_base_t* base =
                    (lv_draw_dsc_base_t*)lv_draw_task_get_draw_dsc(t);
                if (!base || base->part != LV_PART_ITEMS) return;
                int32_t id = base->id1;
                if (!lv_buttonmatrix_has_button_ctrl(kb, id,
                        LV_BUTTONMATRIX_CTRL_CUSTOM_1)) return;
                lv_draw_fill_dsc_t* fd = lv_draw_task_get_fill_dsc(t);
                if (!fd) return;
                /* Pressed durumda da iOS'taki gibi belirgin açılma için
                 * basılı tuşu açık ton, normal tuşu koyu ton yap. */
                bool pressed = ((uint32_t)id == lv_buttonmatrix_get_selected_button(kb))
                               && lv_obj_has_state(kb, LV_STATE_PRESSED);
                /* Fonksiyon tuşu (1#/ABC, ⌫): normalde harften daha koyu
                 * kahve; basılınca harf rengiyle aynı sıcak tona açılır. */
                fd->color = pressed ? lv_color_hex(0x8C8277)
                                    : lv_color_hex(0x6E6557);
            }, LV_EVENT_DRAW_TASK_ADDED, NULL);
        } else if (s_pw_keyboard) {
            lv_obj_clear_flag(s_pw_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        // Mod toggle etiketi — invalidate buton container'ı, eski label
        // pikselleri arka planda kalıp "ghost text" oluşturmasın.
        if (s_pw_mode_btn_lbl)
#ifdef BOARD_VARIANT_LCD128
            lv_label_set_text(s_pw_mode_btn_lbl, LV_SYMBOL_EDIT);
#else
            lv_label_set_text(s_pw_mode_btn_lbl, "Scribble");
#endif
        if (s_pw_mode_btn) lv_obj_invalidate(s_pw_mode_btn);
    } else {
        // SCRIBBLE moduna dön
        if (s_pw_keyboard) {
            lv_obj_del(s_pw_keyboard);
            s_pw_keyboard = NULL;
        }
        if (s_pw_scribble)  lv_obj_clear_flag(s_pw_scribble,  LV_OBJ_FLAG_HIDDEN);
#ifdef BOARD_VARIANT_LCD128
        if (s_pw_mode_btn) lv_obj_set_pos(s_pw_mode_btn, 160, 190);
#endif
        if (s_pw_del_btn)   lv_obj_clear_flag(s_pw_del_btn,   LV_OBJ_FLAG_HIDDEN);
        if (s_pw_ok_btn)    lv_obj_clear_flag(s_pw_ok_btn,    LV_OBJ_FLAG_HIDDEN);
        if (s_pw_space_btn) lv_obj_clear_flag(s_pw_space_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_pw_mode_btn_lbl)
#ifdef BOARD_VARIANT_LCD128
            lv_label_set_text(s_pw_mode_btn_lbl, LV_SYMBOL_KEYBOARD);
#else
            lv_label_set_text(s_pw_mode_btn_lbl, "Keyboard");
#endif
        if (s_conn_overlay) lv_obj_invalidate(s_conn_overlay);
    }
}

/* ── Scribble (elyazısı) — Apple Watch tarzı çoklu stroke + geometrik tanıma ──
 * Akış:
 *   PRESS    → yeni stroke başlat (hint gizle), ilk noktayı ekle, invalidate
 *   PRESSING → her noktayı interpolate ederek buffer'a ekle; parmak
 *              son invalidate konumundan ≥3 px hareket ettiğinde full invalidate
 *   RELEASE  → stroke'u stroke_meta dizisine işle
 *   OK (↵)   → tüm stroke'ları unistroke_classify_multi ile sınıflandır,
 *              karakteri textarea'ya yaz, state'i sıfırla
 *   DEL (⌫)  → son stroke'u geri al (stroke yoksa textarea'dan son karakteri sil)
 *
 * Çizim modeli: lv_obj + LV_EVENT_DRAW_MAIN. Her frame stroke segmentleri
 * lv_draw_line ile çizilir — dotted grid YOK (önceki denemede 660 rect-draw
 * per-frame yavaşlığın sebebiydi). Apple Watch de zaten minimum arka plan
 * kullanıyor: temiz siyah zemin + parlak beyaz izler. lv_canvas'dan vazgeçildi
 * çünkü L8 (126 KB) iç RAM'de fragmantasyon nedeniyle ayrılamıyor, I1'de
 * LVGL'in palette/blend semantiği bizim kullanımımıza uymuyor (+
 * lv_canvas_finish_layer busy-wait LV_USE_OS=NONE'da takılabiliyor). */

/* DRAW_MAIN handler — overlay'deki lv_obj_t çizim panelinin içine stroke'ları
 * render eder. LVGL default bg/border her frame çiziminden sonra çağrılır
 * (LV_EVENT_DRAW_MAIN sonrası bir Main draw geldiği için LV_EVENT_DRAW_POST
 * da tercih edilebilirdi; ama DRAW_MAIN + EVENT_BUBBLE default çizimi
 * bozmuyor, sadece ekliyor). */
static void scribble_draw_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_DRAW_MAIN) return;
    if (!s_pw_scribble || s_stroke_n == 0) return;

    lv_layer_t* layer = lv_event_get_layer(e);
    if (!layer) return;

    lv_area_t ca;
    lv_obj_get_coords(s_pw_scribble, &ca);

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color       = lv_color_make(0xEA, 0xEA, 0xF0);
    ld.width       = SCRIBBLE_LINE_W;
    ld.round_start = 1;
    ld.round_end   = 1;
    ld.opa         = LV_OPA_COVER;

    /* Her stroke ayrı bir polyline — stroke sınırını atlama (H'nin dikey
     * çubukları arası uçmasını önler). Aktif (canlı) stroke da s_stroke_n
     * içinde olduğu için onu da çizmeliyiz. Aktif stroke'un başlangıcı
     * s_cur_stroke_start; sonu s_stroke_n. Meta'ya henüz eklenmemiş olsa
     * bile parmak hâlâ bastığı için burada bu "sanal stroke"'u da çiziyoruz. */
    auto draw_stroke = [&](uint16_t start, uint16_t count) {
        if (count == 0) return;
        uint16_t end = start + count;
        /* Tek noktalı stroke → round-cap ile küçük daire */
        if (count == 1) {
            ld.p1.x = s_stroke_pts[start].x + ca.x1;
            ld.p1.y = s_stroke_pts[start].y + ca.y1;
            ld.p2.x = ld.p1.x + 1;
            ld.p2.y = ld.p1.y;
            lv_draw_line(layer, &ld);
            return;
        }
        for (uint16_t i = start + 1; i < end; i++) {
            ld.p1.x = s_stroke_pts[i - 1].x + ca.x1;
            ld.p1.y = s_stroke_pts[i - 1].y + ca.y1;
            ld.p2.x = s_stroke_pts[i].x + ca.x1;
            ld.p2.y = s_stroke_pts[i].y + ca.y1;
            lv_draw_line(layer, &ld);
        }
    };

    /* Tamamlanmış stroke'lar */
    for (uint8_t si = 0; si < s_stroke_count; si++) {
        draw_stroke(s_strokes[si].start, s_strokes[si].count);
    }
    /* Henüz commit edilmemiş stroke — hem "live" (parmak hâlâ basıyor) hem
     * de "grace period" (RELEASED oldu ama timer henüz fire etmedi, yani
     * touch-glitch ihtimaline karşı bekliyoruz) durumları burada. Eğer bu
     * kontrolü s_stroke_live'a bağlasaydık, 150 ms'lik grace penceresinde
     * stroke görsel olarak kaybolur ve gözle kesik kesik algılanırdı. */
    if (s_stroke_n > s_cur_stroke_start) {
        draw_stroke(s_cur_stroke_start, s_stroke_n - s_cur_stroke_start);
    }
}

/* Grace-period commit: bekleyen stroke'u gerçekten kalıcı hale getirir.
 * Timer dolunca VEYA merge-edilemeyecek yeni bir PRESSED geldiğinde çağrılır. */
static void commit_pending_stroke(void)
{
    if (s_commit_timer) {
        lv_timer_del(s_commit_timer);
        s_commit_timer = NULL;
    }
    uint16_t stroke_len = s_stroke_n - s_cur_stroke_start;
    if (stroke_len < 3) {
        /* Çok kısa → sadece tap, at. */
        s_stroke_n = s_cur_stroke_start;
        return;
    }
    if (s_stroke_count < SCRIBBLE_MAX_STROKES) {
        s_strokes[s_stroke_count].start = s_cur_stroke_start;
        s_strokes[s_stroke_count].count = stroke_len;
        s_stroke_count++;
    }
    /* Bir sonraki stroke s_stroke_n'den başlasın ki draw_cb "pending"
     * bölgesi boş kalsın (aksi halde zaten meta'dan çizilen stroke yine
     * pending olarak ikinci kez çizilirdi). */
    s_cur_stroke_start = s_stroke_n;
}

static void commit_timer_cb(lv_timer_t* t)
{
    (void)t;
    s_commit_timer = NULL;
    /* lv_timer kendi kendine silinecek (repeat_count=1); biz sadece
     * pointer'ı temizliyoruz. */
    commit_pending_stroke();
    /* Görsel güncelleme gerekmez — pending bölgesi zaten çiziliyordu,
     * meta'ya geçmek sadece internal accounting. */
}

static void cancel_commit_timer(void)
{
    if (s_commit_timer) {
        lv_timer_del(s_commit_timer);
        s_commit_timer = NULL;
    }
}

static void scribble_reset_all(void)
{
    cancel_commit_timer();
    s_stroke_n = 0;
    s_stroke_count = 0;
    s_cur_stroke_start = 0;
    s_stroke_live = false;
    if (s_pw_scribble) lv_obj_invalidate(s_pw_scribble);
}

/* NOT: Eski "settle/fade" zamanlayıcıları tamamen kaldırıldı — artık
 * canvas kendi kendine silinmez, sadece kullanıcı OK'e veya DEL'e bastığında
 * içerik değişir. Bu tasarım multi-stroke harflerde kullanıcıyı zaman
 * baskısından kurtarır (H/K/T/i/t/x için açıkça ↵'ye bas, kafanı yorma). */

/* Tek ham nokta ekle (interpolasyonsuz) */
static inline void raw_add_point(int32_t lx, int32_t ly)
{
    if (s_stroke_n >= SCRIBBLE_MAX_POINTS) return;
    s_stroke_pts[s_stroke_n].x = (int16_t)lx;
    s_stroke_pts[s_stroke_n].y = (int16_t)ly;
    s_stroke_n++;
}

/* İnterpolasyonlu ekleme — parmak dokunmatik örneklemesi seyrek olabilir
 * (30-60 Hz), hızlı hareketlerde peşpeşe raporlanan iki nokta aralarında
 * 15-30 px boşluk kalabilir. Bu boşlukları sabit SCRIBBLE_INTERP_STEP
 * (~3 px) adımlarla dolduruyoruz → çizgi uçlarındaki round-cap (yarıçap
 * line_width/2 = 4 px) örtüşüyor, çizgi kesintisiz akıyor. */
static void add_point_interp(int32_t lx, int32_t ly)
{
    if (s_stroke_n == 0 || s_stroke_n == s_cur_stroke_start) {
        raw_add_point(lx, ly);
        return;
    }
    int32_t px = s_stroke_pts[s_stroke_n - 1].x;
    int32_t py = s_stroke_pts[s_stroke_n - 1].y;
    int32_t dx = lx - px;
    int32_t dy = ly - py;
    int32_t d2 = dx*dx + dy*dy;
    if (d2 < SCRIBBLE_JITTER_PX * SCRIBBLE_JITTER_PX) return;

    /* Her durumda interpolate — parmağı yavaş bile oynatsa (d=4-6 px)
     * zaten sadece 1-2 ara nokta eklenir; hızlı oynattığında ise boşluk
     * tamamen dolar. */
    float d = sqrtf((float)d2);
    int steps = (int)(d / (float)SCRIBBLE_INTERP_STEP);
    if (steps < 1) steps = 1;
    if (steps > 64) steps = 64;
    for (int i = 1; i < steps; i++) {
        int32_t ix = px + dx * i / steps;
        int32_t iy = py + dy * i / steps;
        raw_add_point(ix, iy);
    }
    raw_add_point(lx, ly);
}

static void scribble_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_active();
    if (!indev || !s_pw_scribble) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t ca;
    lv_obj_get_coords(s_pw_scribble, &ca);
    int32_t lx = p.x - ca.x1;
    int32_t ly = p.y - ca.y1;

    if (code == LV_EVENT_PRESSED) {
        /* Max stroke sayısına ulaşıldıysa sıfırla — kullanıcı bir üst harfte
         * OK'e basmayı unutmuş demek. Temiz başla. */
        if (s_stroke_count >= SCRIBBLE_MAX_STROKES) {
            scribble_reset_all();
        }

        if (s_pw_hint_lbl) lv_obj_add_flag(s_pw_hint_lbl, LV_OBJ_FLAG_HIDDEN);

        /* Bir önceki stroke için grace-timer aktifse: touch-glitch olasılığı
         * var. Yeni basış noktası son kaydedilen noktaya yakınsa AYNI
         * stroke'a devam et — aradaki boşluğu interpolate et. Böylece
         * kapasitif ekranın hızlı eğri hareketlerdeki 1-2 kare kayıp
         * okumaları görsel olarak fark edilmez ve tek çizgi sürekli kalır. */
        if (s_commit_timer && s_stroke_n > s_cur_stroke_start) {
            int32_t px = s_stroke_pts[s_stroke_n - 1].x;
            int32_t py = s_stroke_pts[s_stroke_n - 1].y;
            int32_t ddx = lx - px;
            int32_t ddy = ly - py;
            if (ddx*ddx + ddy*ddy <= TOUCH_MERGE_PX * TOUCH_MERGE_PX) {
                /* MERGE: grace timer'ı iptal et, stroke'u devam ettir. */
                cancel_commit_timer();
                s_stroke_live = true;
                add_point_interp(lx, ly);
                s_last_inv_x = lx;
                s_last_inv_y = ly;
                lv_obj_invalidate(s_pw_scribble);
                return;
            }
            /* Uzak nokta: merge değil. Bekleyen stroke'u gerçekten commit et
             * ve normal yeni-stroke akışına düş. */
            commit_pending_stroke();
        }

        s_cur_stroke_start = s_stroke_n;
        s_stroke_live      = true;
        raw_add_point(lx, ly);
        s_last_inv_x = lx;
        s_last_inv_y = ly;
        lv_obj_invalidate(s_pw_scribble);
    }
    else if (code == LV_EVENT_PRESSING) {
        if (!s_stroke_live) return;
        add_point_interp(lx, ly);
        /* Full-invalidate'i 3 px threshold ile throttle et. DRAW_MAIN her
         * çağrıldığında tüm stroke'ları render ediyor; 50 Hz örneklemede
         * parmağı yavaş oynatırken 1 px farkla invalidate istemeyiz,
         * LVGL dispatch işini boşa yapar. */
        int32_t ddx = lx - s_last_inv_x;
        int32_t ddy = ly - s_last_inv_y;
        if (ddx*ddx + ddy*ddy >= 9) {
            s_last_inv_x = lx;
            s_last_inv_y = ly;
            lv_obj_invalidate(s_pw_scribble);
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_stroke_live) return;
        s_stroke_live = false;

        uint16_t stroke_len = s_stroke_n - s_cur_stroke_start;
        /* Çok kısa stroke'u (sadece tap) sil — buffer'dan çıkar, repaint. */
        if (stroke_len < 3) {
            s_stroke_n = s_cur_stroke_start;
            cancel_commit_timer();
            if (s_stroke_count == 0 && s_pw_hint_lbl) {
                lv_obj_clear_flag(s_pw_hint_lbl, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_invalidate(s_pw_scribble);
            return;
        }

        /* Hemen commit ETME — TOUCH_MERGE_MS süresince grace penceresi başlat.
         * Pencerede yeni bir PRESSED gelirse (touch-glitch senaryosu) PRESSED
         * handler'ı stroke'u merge'leyecek. Pencere biterse commit_timer_cb
         * gerçekten finalize eder. */
        cancel_commit_timer();
        s_commit_timer = lv_timer_create(commit_timer_cb, TOUCH_MERGE_MS, NULL);
        lv_timer_set_repeat_count(s_commit_timer, 1);
        lv_obj_invalidate(s_pw_scribble);
    }
}

/* OK (↵) butonuna basılınca: canvas'ta toplanan tüm stroke'ları classify
 * et, başarılıysa karakteri textarea'ya yaz, canvas'ı temizle. */
static void scribble_commit(void)
{
    /* Grace-period sonunda henüz commit edilmemiş son stroke varsa
     * (kullanıcı son stroke'u çizdikten hemen sonra OK'e bastı, timer
     * fire etmeden) zorla finalize et — yoksa classify bu noktaları
     * göremez ve yanlış/eksik sınıflandırma olur. */
    commit_pending_stroke();

    if (s_stroke_n < 4 || s_stroke_count == 0) {
        /* Hiçbir şey çizilmedi — sessizce yok say */
        return;
    }

    /* Multi-stroke classifier için stroke_meta'dan start/len dizilerini kur.
     * Geometrik analiz önce çalışır (H, T, t, i, x, k, L, + gibi harfleri
     * stroke sayısı + yön + konum ilişkisinden ayırır), karar veremezse
     * $1 unistroke'a düşer. Bu sayede "H yazınca 6" gibi ms-hatalar biter. */
    uint16_t starts[SCRIBBLE_MAX_STROKES];
    uint16_t lens[SCRIBBLE_MAX_STROKES];
    for (uint8_t i = 0; i < s_stroke_count; i++) {
        starts[i] = s_strokes[i].start;
        lens[i]   = s_strokes[i].count;
    }
    char c = unistroke_classify_multi(s_stroke_pts, s_stroke_n,
                                      starts, lens, s_stroke_count);
    if (c && s_pw_textarea) {
        /* Gerçek şifreyi kendi tamponumuza yazıyoruz; textarea sadece
         * görsel (maskeli + son karakter reveal). LVGL'in pwd_mode'u
         * yerine custom reveal zamanlayıcısı kullanılıyor (PW_REVEAL_MS). */
        pw_append_char(c);
    }
    /* Tanınsa da tanınmasa da canvas'ı temizle (kullanıcı tekrar dener).
     * scribble_reset_all canvas'ı bg+dots ile yeniden boyar. */
    scribble_reset_all();
    if (s_pw_hint_lbl) lv_obj_clear_flag(s_pw_hint_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* DEL butonuna basılınca: Ekranda stroke varsa sadece SON stroke'u sil
 * (yanlış çizmiş de olabilir). Stroke yoksa textarea'dan son karakteri sil. */
static void scribble_undo_or_delchar(void)
{
    /* Grace-period zamanlayıcısı aktifse → son "pending" stroke'u sil
     * (henüz meta'ya girmemişti). Zaten bu en taze çizim, DEL mantığıyla
     * birebir uyumlu: "son yazdığımı geri al". */
    if (s_commit_timer || (s_stroke_n > s_cur_stroke_start)) {
        cancel_commit_timer();
        s_stroke_n = s_cur_stroke_start;
        if (s_stroke_count == 0 && s_pw_hint_lbl) {
            lv_obj_clear_flag(s_pw_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_pw_scribble) lv_obj_invalidate(s_pw_scribble);
        return;
    }

    if (s_stroke_count > 0) {
        /* Son stroke'u buffer'dan çıkar — bir sonraki DRAW_MAIN zaten
         * artık onu render etmeyecek. */
        s_stroke_count--;
        s_stroke_n = s_strokes[s_stroke_count].start;
        s_cur_stroke_start = s_stroke_n;
        if (s_stroke_count == 0 && s_pw_hint_lbl) {
            lv_obj_clear_flag(s_pw_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_pw_scribble) lv_obj_invalidate(s_pw_scribble);
        return;
    }
    /* Ekran boş → şifreden son karakteri sil (custom buffer). */
    pw_backspace();
}

/* Şifre giriş ekranı — Apple Watch Scribble tarzı elyazısı girişi
 *
 * Düzen (410×502):
 *   y=  0– 62 : Header — Cancel | SSID | Join
 *   y= 62– 63 : Separator
 *   y= 72–108 : Password preview textarea (dots + mavi cursor)
 *   y=115–430 : Scribble canvas (parmakla çizim — 315 px yükseklik)
 *   y=440–486 : Alt bar — DEL | SPACE | DONE
 *
 * Tanıma: $1 Unistroke Recognizer (src/services/unistroke.*).
 * Kullanıcı canvas'a tek bir stroke çizer, release anında classify edilir
 * ve sonuç textarea'ya eklenir. Fade timer 200 ms sonra canvas'ı temizler.
 *
 * Root = lv_scr_act() → overlay'in scrollable atası yok. SCROLL_ON_FOCUS
 * kapalı; GESTURE_BUBBLE kapalı → çizim sırasında ekran kaymaz.
 */
static void show_pw_overlay(const char* ssid)
{
    close_conn_overlay();
    ui_service_lock_swipe(true);

    /* ── Overlay: ekran seviyesinde, scrollable ata yok ─────────────────── */
    lv_obj_t* ov = make_overlay_base();
    s_conn_overlay = ov;

    /* ── Header: y=0–62 ──────────────────────────────────────────────────
     * Yuvarlak köşe güvenli iç marj: ~34px. Font 16 (kompakt) çünkü üç
     * öğe (Cancel | SSID | Join) yan yana 410px'e sığacak.
     */
    /* Cancel — sol üst */
    lv_obj_t* cancel_lbl = lv_label_create(ov);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_make(0x0A,0x84,0xFF), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(cancel_lbl,  &lv_font_montserrat_14, 0);
    lv_obj_set_pos(cancel_lbl, 46, 14);
    lv_label_set_text(cancel_lbl, LV_SYMBOL_LEFT);   /* dar ekran: sadece ok */
#else
    lv_obj_set_style_text_font(cancel_lbl,  &lv_font_montserrat_16, 0);
    lv_obj_set_pos(cancel_lbl, 34, 28);
    lv_label_set_text(cancel_lbl, LV_SYMBOL_LEFT " Cancel");
#endif
    lv_obj_add_flag(cancel_lbl, LV_OBJ_FLAG_CLICKABLE);
    /* v1.4.64: 14 → 24 (font 16 daha küçük, 28 yan komşu element'le çakışır) */
    lv_obj_set_ext_click_area(cancel_lbl, 24);
    lv_obj_add_event_cb(cancel_lbl, [](lv_event_t* e){
        (void)e; close_conn_overlay();
    }, LV_EVENT_CLICKED, NULL);

    /* SSID — dar (150px), uzun isim DOT ile kısalır */
    lv_obj_t* ssid_lbl = lv_label_create(ov);
    lv_obj_set_style_text_color(ssid_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ssid_lbl,  &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(ssid_lbl, LV_TEXT_ALIGN_CENTER, 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(ssid_lbl, 84, 18);   /* tek satır — sarma yok, DOT keser */
    lv_obj_set_pos(ssid_lbl, (240-84)/2, 14);
#else
    lv_obj_set_size(ssid_lbl, 150, LV_SIZE_CONTENT);
    lv_obj_set_pos(ssid_lbl, (410-150)/2, 30);
#endif
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(ssid_lbl, ssid);

    /* Join — sağ üst */
    lv_obj_t* join_lbl = lv_label_create(ov);
    lv_obj_set_style_text_color(join_lbl, lv_color_make(0x0A,0x84,0xFF), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(join_lbl,  &lv_font_montserrat_14, 0);
    lv_obj_set_pos(join_lbl, 168, 14);
#else
    lv_obj_set_style_text_font(join_lbl,  &lv_font_montserrat_16, 0);
    lv_obj_set_pos(join_lbl, 336, 28);
#endif
#ifdef BOARD_VARIANT_LCD128
    lv_label_set_text(join_lbl, LV_SYMBOL_OK);   /* dar serit: sadece onay isareti */
#else
    lv_label_set_text(join_lbl, "Join");
#endif
    lv_obj_add_flag(join_lbl, LV_OBJ_FLAG_CLICKABLE);
    /* v1.4.64: 14 → 24 (font 16 küçük, 28 SSID label'ıyla çakışır) */
    lv_obj_set_ext_click_area(join_lbl, 24);
    lv_obj_add_event_cb(join_lbl, [](lv_event_t* e){
        (void)e; pw_do_connect();
    }, LV_EVENT_CLICKED, NULL);

    /* Separator — y=62 */
    lv_obj_t* sep = lv_obj_create(ov);
    lv_obj_remove_style_all(sep);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(sep, 240, 1);
    lv_obj_set_pos(sep, 0, 34);
#else
    lv_obj_set_size(sep, 410, 1);
    lv_obj_set_pos(sep, 0, 62);
#endif
    lv_obj_set_style_bg_color(sep, lv_color_make(0x2C,0x2C,0x2E), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sep, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                            LV_OBJ_FLAG_SCROLLABLE));

    /* ── Şifre preview (küçük, üst bar altı) ───────────────────────────────
     * Apple Watch Scribble'daki gibi minimal gösterim: nokta serisi +
     * son-girilen karakter kısa süre açık gösterilir (password_show_time). */
    s_pw_textarea = lv_textarea_create(ov);
    lv_textarea_set_one_line(s_pw_textarea, true);
    /* LVGL'in pwd_mode'unu kullanmıyoruz — kendi buffer'ımız + lv_timer'la
     * reveal'i tam kontrol altında tutuyoruz (bkz. pw_append_char, PW_REVEAL_MS).
     * Sebep: LVGL pwd_mode `auto_hide_characters` için lv_anim_path_step
     * kullanıyor; anim, invalidate veya state değişimi sırasında erken
     * iptal olabilir ve kullanıcı "karakter çok hızlı kayboldu" diye şikayet
     * ediyordu. Manuel buffer deterministik davranışı garanti eder. */
    lv_textarea_set_password_mode(s_pw_textarea, false);
    lv_textarea_set_placeholder_text(s_pw_textarea, "");
    /* Tamponu sıfırla — overlay her açıldığında temiz başlasın. */
    pw_reset_buffer();
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_pw_textarea, 180, 24);
    lv_obj_set_pos(s_pw_textarea, 30, 36);
#else
    lv_obj_set_size(s_pw_textarea, 330, 42);
    lv_obj_set_pos(s_pw_textarea, 40, 68);
#endif
    lv_obj_clear_flag(s_pw_textarea, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    /* Textarea'yı tıklanamaz yap: dokunduğunda klavye açmak istemiyoruz,
     * sadece scribble üzerinden yazılıyor. */
    lv_obj_clear_flag(s_pw_textarea, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_pw_textarea, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pw_textarea, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_pw_textarea, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_pw_textarea, lv_color_white(), LV_PART_MAIN);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_pw_textarea, &lv_font_montserrat_16, LV_PART_MAIN);
#else
    lv_obj_set_style_text_font(s_pw_textarea, &lv_font_montserrat_24, LV_PART_MAIN);
#endif
    lv_obj_set_style_text_align(s_pw_textarea, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(s_pw_textarea, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_pw_textarea, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_pw_textarea, 0, LV_PART_MAIN);
    /* ── Cursor: düz ince gri dik çizgi ──────────────────────────────────
     * LVGL 9'un CURSOR part'ı varsayılan olarak cur_area'yı letter_w
     * genişliğinde bir dikdörtgen çiziyor VE içine imleç altındaki
     * karakteri (password bullet) CURSOR part'ın text_color'ıyla
     * bastırıyor (lv_textarea.c: draw_cursor — bg_opa>0 VEYA cursor
     * text_color != label text_color koşulunda). Bu ikisi birleşince
     * "mavi/gri blok + beyaz harf üstüne çizilmiş" → şeritli garip
     * görünüm oluşuyordu.
     *
     * Çözüm:
     *   - bg_opa = TRANSP → blok çizilmez
     *   - SOL kenara 2 px gri border → klasik "I-beam" çubuk
     *   - CURSOR text_color == label text_color (white) → karakter-overlay
     *     koşulu false → üstüne tekrar harf çizilmez
     *   - anim_duration = 0 → blink kapalı, statik duruyor
     *   - outline/shadow da kesin sıfırlansın (tema default'ları için) */
    {
        lv_style_selector_t cur_sel = (lv_style_selector_t)LV_PART_CURSOR;
        lv_obj_set_style_bg_opa(s_pw_textarea,        LV_OPA_TRANSP,                    cur_sel);
        lv_obj_set_style_border_side(s_pw_textarea,   (lv_border_side_t)LV_BORDER_SIDE_LEFT, cur_sel);
        lv_obj_set_style_border_width(s_pw_textarea,  2,                                cur_sel);
        lv_obj_set_style_border_color(s_pw_textarea,  lv_color_make(0x7A,0x7A,0x7E),    cur_sel);
        lv_obj_set_style_border_opa(s_pw_textarea,    LV_OPA_COVER,                     cur_sel);
        lv_obj_set_style_text_color(s_pw_textarea,    lv_color_white(),                 cur_sel);
        lv_obj_set_style_outline_width(s_pw_textarea, 0,                                cur_sel);
        lv_obj_set_style_shadow_width(s_pw_textarea,  0,                                cur_sel);
        lv_obj_set_style_anim_duration(s_pw_textarea, 0,                                cur_sel);
    }

    /* ── Scribble çizim paneli — tam genişlik, alt bara kadar ───────────────
     * y=115..430 → h=315. lv_obj + LV_EVENT_DRAW_MAIN ile her frame
     * stroke'lar render edilir. Ne lv_canvas ne heap alloc; LVGL display
     * buffer doğrudan RGB565 pikselleri yazar. Arka plan koyu düz renk,
     * dotted grid yok — Apple Watch Scribble'daki temiz görünüm. */
    s_pw_scribble = lv_obj_create(ov);
    lv_obj_remove_style_all(s_pw_scribble);
    /* Boyut: y=115..410 (h=295). Alt bar y=416'dan başlıyor → 6 px temiz
     * boşluk. Önceki h=310 değerinde scribble y=425'e kadar uzanıp bar'ın
     * üst 9 px'iyle çakışıyordu: OK tıklamasında scribble_reset_all →
     * lv_obj_invalidate(scribble) tüm çakışan bölgeyi de re-render'a
     * sokuyor ve OK butonu kısa süre "bozulmuş" yeşil görünümünde
     * sıçrıyordu. Çakışmayı kaldırmak artefaktı da yok ediyor. */
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_pw_scribble, 240, 114);
    lv_obj_set_pos(s_pw_scribble, 0, 72);
#else
    lv_obj_set_size(s_pw_scribble, 410, 295);
    lv_obj_set_pos(s_pw_scribble, 0, 115);
#endif
    lv_obj_set_style_bg_color(s_pw_scribble, lv_color_make(0x0A,0x0A,0x0A), 0);
    lv_obj_set_style_bg_opa(s_pw_scribble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pw_scribble, 0, 0);
    lv_obj_set_style_radius(s_pw_scribble, 0, 0);
    lv_obj_set_style_pad_all(s_pw_scribble, 0, 0);
    /* KRİTİK: LV_OBJ_FLAG_PRESS_LOCK — parmak widget sınırlarının dışına
     * kısa süreli taşsa bile (ör. 'a' harfinde kıvrım, 'o'nun üst-kenarı,
     * hızlı eğrisel hareket sırasında touch controller jitter'ı) LVGL
     * PRESSING eventlerini scribble paneline atmaya devam eder. Bu flag
     * OLMADAN LVGL her frame hit-test yapıyor; parmağın altında farklı bir
     * widget bulduğunda (textarea y=68..110 veya bar y=416..454) scribble'a
     * LV_EVENT_PRESS_LOST fırlatıyor → stroke ortadan kesiliyor. Kullanıcı
     * bunu "bir yere kadar çiziyor sonra çizgi kesiliyor" olarak
     * deneyimliyordu. PRESS_LOCK bu davranışı kapatıyor: parmak fiziksel
     * olarak kalkana kadar press bu widget'ta kilitli kalır. */
    lv_obj_add_flag(s_pw_scribble,
        (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
    lv_obj_clear_flag(s_pw_scribble,
        (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS |
                        LV_OBJ_FLAG_GESTURE_BUBBLE));

    lv_obj_add_event_cb(s_pw_scribble, scribble_event_cb, LV_EVENT_PRESSED,     NULL);
    lv_obj_add_event_cb(s_pw_scribble, scribble_event_cb, LV_EVENT_PRESSING,    NULL);
    lv_obj_add_event_cb(s_pw_scribble, scribble_event_cb, LV_EVENT_RELEASED,    NULL);
    lv_obj_add_event_cb(s_pw_scribble, scribble_event_cb, LV_EVENT_PRESS_LOST,  NULL);
    lv_obj_add_event_cb(s_pw_scribble, scribble_draw_cb,  LV_EVENT_DRAW_MAIN,   NULL);

    /* İpucu etiketi — panelin ortasında, çizim başlayana kadar görünür. */
    s_pw_hint_lbl = lv_label_create(s_pw_scribble);
    lv_obj_set_style_text_color(s_pw_hint_lbl, lv_color_make(0x44,0x44,0x48), 0);
    lv_obj_set_style_text_font(s_pw_hint_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_pw_hint_lbl, "Draw a letter");
    lv_obj_align(s_pw_hint_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_pw_hint_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_pw_hint_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Stroke state'i sıfırla (hint görünür kalsın) */
    scribble_reset_all();

    /* ── Alt bar: DEL | SPACE | DONE ──────────────────────────────────────
     *
     * STATE-FREE PATTERN (OK/yeşil tik butonundaki ile aynı): hiçbir
     * LV_STATE_PRESSED stili tanımlanmıyor — LVGL, pressed durumunda stil
     * bulamayıp default'a düşer, böylece PRESSED→NORMAL flash/kayma
     * tamamen gider. Ayrıca:
     *   - transform_scale=256 (1.0x) default state'te EXPLICIT pinleniyor
     *     — LVGL default tema, remove_style_all sonrası bile bazı
     *     durumlarda pressed state'e ufak bir scale-down yansıtabiliyor,
     *     bu pin ile ara transform'ların bir anchor'ı oluyor.
     *   - Label da remove_style_all ile default tema'dan izole ediliyor
     *     (aksi halde parent pressed olduğunda label tema-kaynaklı ufak
     *     bir translate alabiliyor → "butonun içinde kayma" görünümü). */
    auto mk_btn = [](lv_obj_t* parent, int32_t x, int32_t y, int32_t w,
                     int32_t h, const char* label, lv_color_t text_col,
                     lv_event_cb_t cb) -> lv_obj_t* {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, w, h);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_style_bg_color(btn, lv_color_make(0x2C,0x2C,0x2E), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_transform_scale(btn, 256, 0); /* 1.0x pin */
        lv_obj_clear_flag(btn,
            (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE |
                             LV_OBJ_FLAG_SCROLL_ON_FOCUS));
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_obj_remove_style_all(lbl);
        lv_obj_set_style_text_color(lbl, text_col, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_label_set_text(lbl, label);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
        return btn;
    };

    /* ── Alt bar düzeni (4 buton, 410 px, yuvarlak AMOLED köşesi güvenli):
     *   Inset her iki kenardan 40 px; inner genişlik = 330
     *   DEL   (⌫)  x= 40, w= 58
     *   OK    (↵)  x=113, w= 58    (gap 15)
     *   SPACE      x=187, w=110    (gap 16)
     *   DONE       x=312, w= 58    (gap 15)
     *   Done bitiş = 370 = 410-40 ✓
     * BAR_Y=416, h=38 → alt y=454. Bottom-right köşe radius ≈120 px için
     * en uç butonun (x=40, y=454) köşe mesafesi √(40²+48²)≈62 > clip bölgesi
     * → güvenli. Buton yüksekliği 44→38, Y 428→416 taşındı; köşe clip'i
     * tamamen gitti. */
#ifdef BOARD_VARIANT_LCD128
    const int32_t BAR_Y = 190;
    const int32_t BAR_H = 28;
#else
    const int32_t BAR_Y = 416;
    const int32_t BAR_H = 38;
#endif

    /* DEL — son stroke'u geri al; ekran boşsa son karakteri sil */
#ifdef BOARD_VARIANT_LCD128
    s_pw_del_btn = mk_btn(ov, 51, BAR_Y, 30, BAR_H, LV_SYMBOL_BACKSPACE,
#else
    s_pw_del_btn = mk_btn(ov, 40, BAR_Y, 58, BAR_H, LV_SYMBOL_BACKSPACE,
#endif
           lv_color_white(), [](lv_event_t* e){
        (void)e;
        scribble_undo_or_delchar();
    });

    /* OK — çizimi onayla ve karakter olarak textarea'ya ekle.
     *
     * Bu buton mk_btn'i KULLANMIYOR çünkü mk_btn PRESSED state'i için
     * farklı bir arka plan rengi ayarlıyor (feedback gri) — önceki
     * denemelerde yeşil butona da böyle bir PRESSED overlay uygulanıyordu
     * ve "tasarımı bozuluyor" şikayeti tam olarak bu PRESSED→NORMAL
     * flash'inden geliyor. Çözüm: Hiçbir state-specific stil yazmıyoruz.
     * lv_obj_remove_style_all sonrası SADECE default (state=0) stilleri
     * ayarlıyoruz; LVGL, state bulunamayan her durumda default stile
     * düşer → PRESSED ile NORMAL aynı görünür, hiç flash yok. */
    lv_color_t ok_bg = lv_color_make(0x30, 0xB8, 0x50);
    lv_obj_t* ok_btn = lv_obj_create(ov);
    s_pw_ok_btn = ok_btn;
    lv_obj_remove_style_all(ok_btn);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(ok_btn, 30, BAR_H);
    lv_obj_set_pos(ok_btn, 84, BAR_Y);
#else
    lv_obj_set_size(ok_btn, 58, BAR_H);
    lv_obj_set_pos(ok_btn, 113, BAR_Y);
#endif
    lv_obj_set_style_bg_color(ok_btn, ok_bg, 0);
    lv_obj_set_style_bg_opa(ok_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ok_btn, 12, 0);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_outline_width(ok_btn, 0, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);
    lv_obj_set_style_pad_all(ok_btn, 0, 0);
    lv_obj_set_style_transform_scale(ok_btn, 256, 0); /* 1.0x */
    lv_obj_clear_flag(ok_btn,
        (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE |
                         LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    lv_obj_add_flag(ok_btn, LV_OBJ_FLAG_CLICKABLE);

    /* Tick sembolü — label da state-free: sadece default stil. */
    lv_obj_t* ok_lbl = lv_label_create(ok_btn);
    lv_obj_remove_style_all(ok_lbl);
    lv_obj_set_style_text_color(ok_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(ok_lbl, LV_SYMBOL_OK);
    lv_obj_center(ok_lbl);
    lv_obj_clear_flag(ok_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ok_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_add_event_cb(ok_btn, [](lv_event_t* e){
        (void)e;
        scribble_commit();
    }, LV_EVENT_CLICKED, NULL);

    /* SPACE — boşluk karakteri */
#ifdef BOARD_VARIANT_LCD128
    s_pw_space_btn = mk_btn(ov, 117, BAR_Y, 40, BAR_H, "spc",
#else
    s_pw_space_btn = mk_btn(ov, 187, BAR_Y, 110, BAR_H, "space",
#endif
           lv_color_white(), [](lv_event_t* e){
        (void)e;
        if (s_pw_textarea) pw_append_char(' ');
    });

    /* MOD TOGGLE — eskiden "Done" butonu vardı; artık scribble ↔ klavye
     * arasında geçiş yapıyor. Gerçek "bağlan" aksiyonu üst bardaki "Join"
     * butonuyla yapılıyor (klavyede Enter da çalışır).
     *
     * Manuel oluşturma (mk_btn değil) çünkü genişliği daha büyük (98 px) — bir
     * "space" + "Keyboard" yazısı sığmalı. Konum: x=272 → genişlik 98 →
     * bitiş 370 = 410-40, sağ inset korunur. Space butonunun bitişi 187+110=297
     * iken 272'den başlamak (gap=−25) çakışmaya yol açar; o yüzden space'i de
     * kısaltıyoruz: 187..272 → w=85.
     *
     * Aşağıdaki block çakışmayı önlemek için space'i resize ediyor + yeni
     * geometri ile mode toggle butonunu yaratıyor. */
#ifdef BOARD_VARIANT_LCD128
    /* LCD: space zaten 40 px — resize gerekmez. */
#else
    if (s_pw_space_btn) {
        lv_obj_set_width(s_pw_space_btn, 85);
    }
#endif
    s_pw_mode_btn = lv_obj_create(ov);
    lv_obj_remove_style_all(s_pw_mode_btn);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_pw_mode_btn, 30, BAR_H);
    lv_obj_set_pos(s_pw_mode_btn, 160, BAR_Y);
#else
    lv_obj_set_size(s_pw_mode_btn, 92, BAR_H);
    lv_obj_set_pos(s_pw_mode_btn, 278, BAR_Y);
#endif
    lv_obj_set_style_bg_color(s_pw_mode_btn, lv_color_make(0x2C,0x2C,0x2E), 0);
    lv_obj_set_style_bg_opa(s_pw_mode_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pw_mode_btn, 12, 0);
    lv_obj_set_style_border_width(s_pw_mode_btn, 0, 0);
    lv_obj_set_style_outline_width(s_pw_mode_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_pw_mode_btn, 0, 0);
    lv_obj_set_style_pad_all(s_pw_mode_btn, 0, 0);
    lv_obj_set_style_transform_scale(s_pw_mode_btn, 256, 0);
    lv_obj_clear_flag(s_pw_mode_btn,
        (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE |
                         LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    lv_obj_add_flag(s_pw_mode_btn, LV_OBJ_FLAG_CLICKABLE);

    s_pw_mode_btn_lbl = lv_label_create(s_pw_mode_btn);
    lv_obj_remove_style_all(s_pw_mode_btn_lbl);
    lv_obj_set_style_text_color(s_pw_mode_btn_lbl,
        lv_color_make(0x0A,0x84,0xFF), 0);
    lv_obj_set_style_text_font(s_pw_mode_btn_lbl, &lv_font_montserrat_16, 0);
#ifdef BOARD_VARIANT_LCD128
    lv_label_set_text(s_pw_mode_btn_lbl, LV_SYMBOL_KEYBOARD);
    lv_obj_center(s_pw_mode_btn_lbl);
#else
    lv_label_set_text(s_pw_mode_btn_lbl, "Keyboard");
    lv_obj_center(s_pw_mode_btn_lbl);
#endif
    lv_obj_clear_flag(s_pw_mode_btn_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_pw_mode_btn_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_add_event_cb(s_pw_mode_btn, pw_mode_toggle_cb, LV_EVENT_CLICKED, NULL);

    /* Başlangıç modu daima SCRIBBLE — kullanıcı isterse Keyboard'a geçer. */
    s_pw_input_mode = PW_INPUT_SCRIBBLE;
}

/* ── AP satırına tıklama ────────────────────────────────────────────────── */
static void ap_row_tap_cb(lv_event_t* e)
{
    uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= s_last_ap_count) return;

    const wifi_ap_t* ap = &s_last_aps[idx];
    strlcpy(s_pending_ssid, ap->ssid, sizeof(s_pending_ssid));
    s_pending_secured = ap->secured;

    if (ap->secured) {
        show_pw_overlay(ap->ssid);
    } else {
        show_connecting_overlay(ap->ssid);
        wifi_service_connect_to_ap(ap->ssid, "", on_ap_connect_result, NULL);
    }
}

// ─── WiFi callbacks ───────────────────────────────────────────────────────────
/* Forward declarations — wifi_row_cb aşağıda tanımlı helper'ları çağırıyor. */
static void wifi_val_apply(void* arg);
static inline void toggle_set_checked(lv_obj_t* outer, lv_obj_t* knob, bool on);

/* Artık lv_switch yok — row CLICKED geldiğinde s_wifi_enabled'ı doğrudan
 * çeviriyor, toggle görselini güncelliyor ve ağır DOM işini async'e atıyor.
 * Toggle visual update (outer bg color + knob x) SADECE iki stil
 * değişikliği — LVGL bunları tek frame'de temiz çiziyor, ara-state yok. */
static void wifi_row_cb(lv_event_t* e)
{
    (void)e;
    bool new_state = !s_wifi_enabled;
    s_wifi_enabled = new_state;

    /* Kullanıcı tercihini NVS'e kaydet — reboot/deep-sleep sonrası boot'ta
     * main.cpp bu tercihi okuyup auto-connect'i ona göre yapar. Eskiden
     * tercih persist edilmiyordu → reboot sonra WiFi her zaman auto-on. */
    wifi_service_set_user_preference(new_state);

    /* Toggle görselini hemen güncelle — kullanıcı dokunduğu anda durum
     * değişikliği görsel olarak yansıyor. */
    if (s_wifi_switch && s_wifi_knob) {
        toggle_set_checked(s_wifi_switch, s_wifi_knob, new_state);
        /* Yalnızca outer'ı invalidate etmek yeterli (knob outer'ın çocuğu
         * olduğu için onunla birlikte redraw ediliyor). */
        lv_obj_invalidate(s_wifi_switch);
    }

    /* Ağır DOM işi (liste unhide/hide, scanning label, section label vs.)
     * sonraki tick'te — toggle redraw'ıyla çarpışmasın. */
    lv_async_call(wifi_val_apply, (void*)(uintptr_t)(new_state ? 1 : 0));
}
/* wifi_val_cb'nin ağır DOM işi — deferred çağrılacak.
 *
 * KÖK SEBEP: Kullanıcı row'a tıklayınca switch'in VALUE_CHANGED event'i
 * SYNC fire oluyor. Eğer aynı callback içinde section label unhide,
 * s_wifi_list unhide + clean + yeni "Scanning..." label create işlemleri
 * de yapılırsa, LVGL'nin şu frame'inde:
 *   (1) switch kendi state-flip invalidate'i,
 *   (2) yukarıdaki DOM değişikliklerinin tetiklediği parent invalidate'ler,
 *   (3) LV_SIZE_CONTENT layout reflow,
 * hepsi TEK frame'e sıkışıyor → partial-invalidate birleştiricisi switch
 * bölgesini tam kapsayamıyor ve ilk render'da indicator/knob sınırında
 * kısmi-stale pixeller görünüyor ("kayma/wedge"). İkinci toggle'da layout
 * stabil kaldığı için sorun tekrar etmiyor.
 *
 * ÇÖZÜM: Switch state-flip'inin kendi frame'ini temizce tamamlamasına izin
 * ver (lv_async_call → sonraki tick), DOM reflow'u ONDAN SONRA yap. Böylece
 * switch hiç kısmi redraw görmeden state-flip eder; DOM değişiklikleri
 * sonraki frame'de ayrı invalidate pass ile çalışır. */
static void wifi_val_apply(void* arg)
{
    bool on = (uintptr_t)arg ? true : false;

    /* Full invalidate of the network view BEFORE any structural change.
     * s_wifi_list is LV_SIZE_CONTENT and its entire parent chain
     * (panel/sbox/view) is transparent. When we clean/hide the list,
     * LVGL only invalidates the object's CURRENT bounds — so old row
     * pixels outside the new (smaller) bounds are never cleared,
     * leaving horizontal streaks. Forcing a parent invalidate first
     * queues a full repaint of the whole view area. */
    if (s_view_network) lv_obj_invalidate(s_view_network);

    if (on) {
        s_wifi_enabled = true;
        if (s_wifi_section_lbl)
            lv_obj_clear_flag(s_wifi_section_lbl, LV_OBJ_FLAG_HIDDEN);
        if (s_wifi_list) {
            /* Invalidate OLD list bounds before shrinking/cleaning so the
             * soon-to-be-empty rows leave no ghost pixels behind. */
            lv_obj_invalidate(s_wifi_list);
            lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clean(s_wifi_list);
            lv_obj_t* scanning = lv_label_create(s_wifi_list);
            lv_obj_set_style_text_color(scanning, lv_color_make(0x8E, 0x8E, 0x93), 0);
            lv_obj_set_style_text_font(scanning, &lv_font_montserrat_20, 0);
            lv_obj_set_pos(scanning, 20, 10);
            lv_label_set_text(scanning, "Scanning...");
        }
        wifi_service_scan_async(wifi_scan_done_cb, NULL);   /* init + scan, no auto-connect */
    } else {
        wifi_service_scan_cancel();
        wifi_disable();
        memset(s_connected_ssid, 0, sizeof(s_connected_ssid));
        s_last_ap_count = 0;
        if (s_wifi_section_lbl) {
            lv_obj_add_flag(s_wifi_section_lbl, LV_OBJ_FLAG_HIDDEN);
            /* Etiket pozisyonunu da resetle, bir sonraki açılışta eski
             * kaymış konumda kalmasın. */
            lv_obj_set_y(s_wifi_section_lbl, 110);
        }
        if (s_wifi_list) {
            /* Same reason: invalidate BEFORE clean+hide. */
            lv_obj_invalidate(s_wifi_list);
            lv_obj_clean(s_wifi_list);
            lv_obj_add_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(s_wifi_list, 132);
        }
        if (s_connected_row) {
            lv_obj_invalidate(s_connected_row);
            lv_obj_clean(s_connected_row);
            lv_obj_add_flag(s_connected_row, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* And a second full invalidate AFTER the change, so any layout reflow
     * (section label hidden → panel shrinks → LV_SIZE_CONTENT height
     * recalculated) gets a clean final frame. */
    if (s_view_network) lv_obj_invalidate(s_view_network);
}

/* wifi_val_cb kaldırıldı — lv_switch VALUE_CHANGED event'i artık yok
 * (widget özel toggle ile değiştirildi). State flip wifi_row_cb içinde,
 * ağır DOM işi wifi_val_apply'da lv_async_call ile. */

// ─── Brightness bar (custom — lv_slider KULLANILMIYOR) ───────────────────────
/*
 * lv_slider yerine iki basit lv_obj_t ile özel bar:
 *   - s_br_bar  : dış pill (koyu gri track, clickable)
 *   - s_br_fill : iç pill (beyaz fill, sola dayalı, genişliği brightness'a oranlı)
 *
 * Avantajlar:
 *   - Büyük touch hedefi (bütün pill tıklanabilir, ince çubuk değil)
 *   - Tap-to-set: bar'ın herhangi bir noktasına dokununca oraya atlar
 *   - Drag: parmak kaydıkça fill anlık takip eder (LV_EVENT_PRESSING)
 *   - lv_slider'ın knob animasyonu / partial-redraw artefaktı yok
 *     (WiFi toggle'da karşılaştığımız lv_switch problemi ile aynı kategoride)
 *   - Görsel: iOS Control Center tarzı büyük beyaz pill — şık ve anlaşılır
 */
#define BR_MIN 20          /* min parlaklık — hiç görünmez olmasın */
#define BR_MAX 255

static void brightness_update_visual(uint8_t val)
{
    if (!s_br_fill || s_br_bar_w <= 0) return;
    int32_t span = BR_MAX - BR_MIN;
    /* val uint8_t — sınırı 255, BR_MAX=255 zaten garantili. Sadece alt clamp. */
    int32_t clamped = val < BR_MIN ? BR_MIN : val;
    int32_t fill_w = ((int32_t)(clamped - BR_MIN) * s_br_bar_w) / span;
    /* Minimum görünür genişlik: bar yüksekliği kadar (tam daire kalabilsin).
     * Aksi halde fill çok darken sadece sol uçta küçük bir nokta gibi görünür. */
    int32_t min_w = lv_obj_get_height(s_br_bar);
    if (fill_w < min_w) fill_w = min_w;
    if (fill_w > s_br_bar_w) fill_w = s_br_bar_w;
    /* Fill width azaldığında LVGL set_width hem eski hem yeni bbox'ı
     * invalidate eder (LVGL 9.2 davranışı). Bizim ek explicit invalidate
     * gereksiz + extra render trigger ediyordu → kaldırıldı, kayma daha
     * akıcı oldu. */
    lv_obj_set_width(s_br_fill, fill_w);
}

static void br_bar_set_from_abs_x(int32_t abs_x)
{
    if (!s_br_bar || s_br_bar_w <= 0) return;
    lv_area_t a;
    lv_obj_get_coords(s_br_bar, &a);
    int32_t rel_x = abs_x - a.x1;
    if (rel_x < 0) rel_x = 0;
    if (rel_x > s_br_bar_w) rel_x = s_br_bar_w;

    int32_t span = BR_MAX - BR_MIN;
    /* +s_br_bar_w/2 → integer div yuvarlama (nearest), daha pürüzsüz takip */
    int32_t val = BR_MIN + (rel_x * span + s_br_bar_w / 2) / s_br_bar_w;
    if (val < BR_MIN) val = BR_MIN;
    if (val > BR_MAX) val = BR_MAX;

    /* Aynı değer gelirse hiç yapma. */
    if ((uint8_t)val == s_brightness) return;

    s_brightness = (uint8_t)val;
    /* VISUAL update HER tick (kayma akıcı olsun). */
    brightness_update_visual(s_brightness);

    /* HW brightness update THROTTLED (60 ms aralıkla).
     * co5300_set_brightness QSPI/I2C blocking write — drag boyunca her
     * PRESSING'de çağırılırsa LVGL task ~10-30 ms blok olur, slider visual
     * gecikmeli güncellenir = jerky drag (kullanıcı raporu 2026-05-04
     * "yumuşak kaymıyor"). 60 ms'de 16 HW update/sn → kullanıcı LIVE
     * brightness değişimi gözünde algılar ama task bloke olmaz. */
    /* v1.2.1 (2026-05-18): 60 → 50 ms (50000 µs). LVGL 16 ms refresh
     * period'unun tam 3 katı → faz-aligned, blocking write bir sonraki
     * render'a sığar, jerky drag deterministik olarak gider. 60 ms
     * (3.6 frame) faz kayması nedeniyle bazen aktif render frame'e
     * çakışıyordu. 50 ms / 60 ms insan algısında ayırt edilemez. */
    static int64_t s_last_hw_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_hw_us >= 50000) {
        s_last_hw_us = now_us;
        /* Session 34: doğrudan panele yazmak yerine ortak seviye servisi —
         * değer NVS'e yazılır ve uyanış/DIM yolları da onu kullanır. */
        power_service_set_brightness(s_brightness);
    }
}

static void br_bar_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        /* Bar'a basılıyken swipe'a kilit koy — aksi halde parmak yatay
         * hareket edince app_manager check_swipe nav back tetikliyor
         * (kullanıcı raporu 2026-05-04: "parlaklığı tekrar açarken bir
         * önceki menüye döndü"). Lock + swipe state reset birlikte. */
        ui_service_lock_swipe(true);
        /* g_swipe_dir'i de sıfırla — touch_read_cb PRESSED öncesi early
         * swipe set ettiyse bile temizle. */
        g_swipe_dir = 0;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        /* Final HW commit — throttle nedeniyle son değer atlanmış olabilir,
         * release'de kesin uygula (Session 34: ortak seviye servisi). */
        power_service_set_brightness(s_brightness);
        /* Release sırasında g_swipe_dir'i sıfırla AMA lock'u 200 ms daha tut.
         * Touch_read_cb release detection birkaç tick sonra çalışır ve
         * dx > threshold olursa g_swipe_dir set edebilir → check_swipe nav
         * tetikler. Lock'u biraz daha tutmak post-release tick'lerde
         * `s_swipe_locked` true gör → swipe yutulur. 200 ms sonra
         * otomatik unlock için lv_timer (one-shot, auto-delete). */
        g_swipe_dir = 0;
        lv_timer_t* unlock_t = lv_timer_create([](lv_timer_t* t) {
            ui_service_lock_swipe(false);
            g_swipe_dir = 0;
            lv_timer_del(t);
        }, 200, NULL);
        lv_timer_set_repeat_count(unlock_t, 1);
    }
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        /* Drag boyunca her tick'te g_swipe_dir'i clear et + lock'u garantile.
         * Touch_read_cb PRESSING'den önce çalışıp swipe set edebilir; bu
         * ek clear sayesinde drag süresince g_swipe_dir ASLA set kalmaz →
         * app_manager check_swipe nav back tetikleyemez. */
        ui_service_lock_swipe(true);
        g_swipe_dir = 0;

        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        br_bar_set_from_abs_x(p.x);
    }
}

// ─── Widget yardımcıları ──────────────────────────────────────────────────────

typedef struct {
    lv_obj_t* content;
    lv_obj_t* rows[8];
    lv_event_cb_t cbs[8];
    int count;
    lv_obj_t* pressed_row;
    int16_t x0;
    int16_t y0;
    int16_t max_adx;
    int16_t max_ady;
    int64_t t0_us;
    bool swiping;
} nav_list_state_t;

static nav_list_state_t s_main_nav    = {};
static nav_list_state_t s_general_nav = {};

/* v1.2.1 (2026-05-18): NAV_SWIPE_THRESHOLD 6 → 12, RATIO 1.10 → 1.30.
 * Önceki değerlerde jitter envelope (4 px JITTER_RADIUS + FT3168 sample
 * noise 1-5 px) tap eşiği ile çakışıyordu — kullanıcı row'a basıp 6 px
 * istemsiz oynatınca tap iptal ediliyor, hiçbir şey olmuyordu.
 * Yeni değerler ui_service.cpp'deki global SWIPE_THRESHOLD=10/RATIO=1.2
 * ile aynı sınıfta, gerçek niyetli swipe'ı kazara hareketten ayırır. */
#define NAV_SWIPE_THRESHOLD 12
#define NAV_SWIPE_RATIO     1.30f
#define NAV_TAP_MAX_MOVE    6
#define NAV_TAP_MAX_US      400000

static bool point_in_obj(lv_obj_t* obj, int16_t x, int16_t y)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2);
}

static void nav_list_cb(lv_event_t* e)
{
    nav_list_state_t* st = (nav_list_state_t*)lv_event_get_user_data(e);
    if (!st) return;

    lv_event_code_t code = lv_event_get_code(e);

    // Sadece dokunuş eventlerini işle
    if (code != LV_EVENT_PRESSED   &&
        code != LV_EVENT_PRESSING  &&
        code != LV_EVENT_RELEASED  &&
        code != LV_EVENT_PRESS_LOST) return;

    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        st->pressed_row = NULL;
        st->swiping     = false;
        st->t0_us       = esp_timer_get_time();
        st->x0          = (int16_t)p.x;
        st->y0          = (int16_t)p.y;
        st->max_adx     = 0;
        st->max_ady     = 0;
        for (int i = 0; i < st->count; i++) {
            if (st->rows[i] && point_in_obj(st->rows[i], p.x, p.y)) {
                st->pressed_row = st->rows[i];
                break;
            }
        }
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (st->pressed_row) {
            int dx  = (int)p.x - st->x0;
            int dy  = (int)p.y - st->y0;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (adx > st->max_adx) st->max_adx = (int16_t)adx;
            if (ady > st->max_ady) st->max_ady = (int16_t)ady;
            // Yatay baskın hareket → swipe niyeti, tap'i iptal et
            if (!st->swiping &&
                adx >= NAV_SWIPE_THRESHOLD &&
                adx > (int)(ady * NAV_SWIPE_RATIO)) {
                st->swiping = true;
            }
        }
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (st->pressed_row) {
            // Bırakma anındaki son delta
            int dx  = (int)p.x - st->x0;
            int dy  = (int)p.y - st->y0;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (adx > st->max_adx) st->max_adx = (int16_t)adx;
            if (ady > st->max_ady) st->max_ady = (int16_t)ady;
            int64_t dt = esp_timer_get_time() - st->t0_us;

            // LVGL gesture/scroll tetiklendiyse tap değil
            bool gesture_active = false;
            lv_dir_t gdir = lv_indev_get_gesture_dir(indev);
            if (gdir != LV_DIR_NONE) gesture_active = true;
            if (lv_indev_get_scroll_obj(indev) != NULL) gesture_active = true;

            // Yatay baskın hareket → swipe (PRESSING'de sahnelenmemiş olsa bile)
            if (!st->swiping &&
                st->max_adx >= NAV_SWIPE_THRESHOLD &&
                st->max_adx > (int)(st->max_ady * NAV_SWIPE_RATIO)) {
                st->swiping = true;
            }

            bool is_tap = !st->swiping &&
                          !gesture_active &&
                          code == LV_EVENT_RELEASED &&
                          (st->max_adx <= NAV_TAP_MAX_MOVE) &&
                          (st->max_ady <= NAV_TAP_MAX_MOVE) &&
                          (adx <= NAV_TAP_MAX_MOVE) &&
                          (ady <= NAV_TAP_MAX_MOVE) &&
                          (dt  <= NAV_TAP_MAX_US);
            if (is_tap) {
                for (int i = 0; i < st->count; i++) {
                    if (st->rows[i] == st->pressed_row && st->cbs[i]) {
                        st->cbs[i](e);
                        break;
                    }
                }
            }
        }
        st->pressed_row = NULL;
        st->swiping     = false;
        st->max_adx     = 0;
        st->max_ady     = 0;
        return;
    }
}

static void format_bytes(uint32_t bytes, char* out, size_t out_size)
{
    const char* unit = "B";
    double value = (double)bytes;
    if (value >= 1024.0 * 1024.0 * 1024.0) {
        value /= (1024.0 * 1024.0 * 1024.0);
        unit = "GB";
    } else if (value >= 1024.0 * 1024.0) {
        value /= (1024.0 * 1024.0);
        unit = "MB";
    } else if (value >= 1024.0) {
        value /= 1024.0;
        unit = "KB";
    }

    if (value >= 100.0 || bytes < 1024) {
        snprintf(out, out_size, "%u %s", (unsigned)llround(value), unit);
    } else if (value >= 10.0) {
        snprintf(out, out_size, "%.1f %s", value, unit);
    } else {
        snprintf(out, out_size, "%.2f %s", value, unit);
    }
}

static uint32_t get_flash_capacity_bytes(void)
{
    uint32_t size = 0;
    if (esp_flash_get_size(NULL, &size) != ESP_OK) return 0;
    return size;
}

static uint32_t get_flash_used_bytes(void)
{
    uint32_t max_end = 0;
    for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                          ESP_PARTITION_SUBTYPE_ANY,
                                                          NULL);
         it != NULL;
         it = esp_partition_next(it)) {
        const esp_partition_t* p = esp_partition_get(it);
        uint32_t end = p->address + p->size;
        if (end > max_end) max_end = end;
    }
    return max_end;
}

/* ─── v1.2.3 (2026-05-19): Storage view helper'ları ───────────────────────────
 *
 * 5 kategori bilgisi gather:
 *   1. App (Flash) — running OTA partition size (firmware)
 *   2. SPIFFS ("assets" partition) — kullanıcı dosyaları
 *   3. PSRAM — external RAM (octal mode, ~8 MB)
 *   4. SRAM — internal RAM (~512 KB)
 *   5. NVS — settings storage
 *
 * Her helper "used" ve "total" döndürür (uint32_t byte). SPIFFS mount edilmemiş
 * veya partition yoksa total=0 → UI "—" gösterir. PSRAM yoksa benzeri.
 */

/* App (firmware) partition — running OTA slot büyüklüğü.
 *   total = OTA slot size (partition table'da ayrılmış kapasite, ~3 MB)
 *   used  = gerçek firmware image boyutu (image header parse).
 *
 * v1.2.3 (2026-05-19) düzeltme #2: esp_image_verify(SILENT) saatte fail
 * ediyordu (out_used=0 → UI "0 B / 3 MB" gösteriyordu). SHA256 doğrulamasının
 * fail sebebi belirsiz (secure boot config farkı veya silent mode quirk).
 *
 * Yeni yaklaşım: image header'ı doğrudan partition'dan oku, segment
 * length'lerini topla. Daha hafif (~1 ms, SHA hesaplama yok) ve doğru:
 *   1. ESP image header (24 byte) — magic + segment_count + flash params
 *   2. Her segment için: segment header (8 byte) + segment data (data_len)
 *   3. Padding (16-byte align) + 1 byte checksum
 *   4. (Append) SHA-256 hash (32 byte) — eğer image hash_appended flag set */
static void get_app_partition_info(uint32_t* out_total, uint32_t* out_used)
{
    *out_total = 0;
    *out_used  = 0;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;
    *out_total = running->size;

    esp_image_header_t header;
    if (esp_partition_read(running, 0, &header, sizeof(header)) != ESP_OK) return;
    if (header.magic != ESP_IMAGE_HEADER_MAGIC) return;

    /* Segment'leri tara — her segment'in length'i header'da yazılı. */
    uint32_t pos = sizeof(esp_image_header_t);
    for (int i = 0; i < header.segment_count; i++) {
        esp_image_segment_header_t seg;
        if (esp_partition_read(running, pos, &seg, sizeof(seg)) != ESP_OK) return;
        pos += sizeof(esp_image_segment_header_t) + seg.data_len;
    }
    /* Checksum byte + 16-byte align padding. Image build process her zaman
     * 16-byte boundary'e align eder; +1 (checksum) sonra ((+15) & ~15). */
    pos += 1;
    pos = (pos + 15) & ~0xFu;
    /* Image trailer'da SHA-256 hash varsa (hash_appended bit set), +32 byte. */
    if (header.hash_appended) pos += 32;

    *out_used = pos;
}

/* SPIFFS "assets" partition — kullanıcı verileri (face image'lar).
 * Mount edilmemişse used=0, total=partition size; mount edilmişse gerçek used. */
static void get_spiffs_info(uint32_t* out_total, uint32_t* out_used)
{
    *out_total = 0;
    *out_used  = 0;
    /* Önce partition'ı bul (mount durumundan bağımsız) */
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "assets");
    if (part) *out_total = part->size;

    /* Mount edilmişse gerçek used'i al */
    size_t total = 0, used = 0;
    if (esp_spiffs_info("assets", &total, &used) == ESP_OK) {
        if (total > 0) *out_total = (uint32_t)total;
        *out_used = (uint32_t)used;
    }
}

/* PSRAM — external octal RAM. */
static void get_psram_info(uint32_t* out_total, uint32_t* out_used)
{
    *out_total = (uint32_t)esp_psram_get_size();
    if (*out_total == 0) { *out_used = 0; return; }
    size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    *out_used = (total_heap > free) ? (uint32_t)(total_heap - free) : 0;
}

/* Internal SRAM — heap_caps DRAM tarafı (LVGL DMA buffer dahil). */
static void get_sram_info(uint32_t* out_total, uint32_t* out_used)
{
    *out_total = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    *out_used = (*out_total > free) ? (*out_total - free) : 0;
}

/* NVS partition — used = sadece named entries × entry size yaklaşımı.
 * nvs_get_stats sadece varsayılan partition için NULL kabul eder. */
static void get_nvs_info(uint32_t* out_total, uint32_t* out_used)
{
    *out_total = 0;
    *out_used  = 0;
    nvs_stats_t s = {};
    if (nvs_get_stats(NULL, &s) != ESP_OK) return;
    /* Her entry 32 byte (ESP-IDF NVS pattern). total_entries = pool size. */
    *out_total = (uint32_t)s.total_entries * 32u;
    *out_used  = (uint32_t)s.used_entries  * 32u;
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings view mimarisi:
//
//   view  (410×502, NON-scrollable, transparan)
//   ├── sbox  (410×428, y=74 — header ALTINDA başlar, SCROLLABLE)
//   │   └── panel  (410×LV_SIZE_CONTENT — içerik büyüdükçe uzar)
//   ├── header label  (sabit, y=26 — sbox'un üstünde render edilir)
//   └── clock label   (sabit, y=26)
//
// Mimari:
//   v  (tam ekran, non-scrollable)
//   ├── sbox  (y=0, TAM YÜKSEKLİK — hard clip yok, içerik header arkasına geçebilir)
//   │   └── panel  (pad_top=VIEW_HEADER_H → içerik başlangıçta header altında)
//   ├── hdr_bg  (siyah opak bar, y=0-74 → scroll eden içeriği maskeler)
//   ├── header label  (z-order en üstte)
//   └── clock label   (z-order en üstte)
//
// Etki: içerik scroll edilince header arkasından akıyor gibi görünür.
// Hard clip yok — Apple Watch gibi yumuşak geçiş.
// ─────────────────────────────────────────────────────────────────────────────

#ifdef BOARD_VARIANT_LCD128
#define VIEW_HEADER_H  46
#else
#define VIEW_HEADER_H  74    // header bölgesinin piksel yüksekliği
#endif
#ifdef BOARD_VARIANT_LCD128
#define VIEW_SCR_H     240
#else
#define VIEW_SCR_H     502   // ekran yüksekliği
#endif

static lv_obj_t* make_view(lv_obj_t* parent, lv_obj_t** out_content)
{
    // Dış container — ekranı kaplar, scroll yok.
    // BG OPAQUE BLACK — slide animasyonunda altında kalan view'in görünmesini
    // engeller (eski davranış: transparent → slide sırasında iki view üst üste
    // görünüyordu, kullanıcı raporu 2026-05-03). Görsel tasarım değişmez:
    // sbox + panel zaten transparent → siyah outer'ın bg'si gösterir.
    lv_obj_t* v = lv_obj_create(parent);
    lv_obj_remove_style_all(v);
    lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(v, 0, 0);
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(v, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(v, 0, 0);
    lv_obj_set_style_border_width(v, 0, 0);

    // Scroll kutusu — y=0'dan başlar, tam ekran yüksekliğinde.
    // Hard clip yok: içerik header bölgesine görsel olarak girebilir,
    // hdr_bg (siyah bar) onu maskeler. Apple Watch gibi yumuşak geçiş.
    lv_obj_t* sbox = lv_obj_create(v);
    lv_obj_remove_style_all(sbox);
    lv_obj_set_size(sbox, LV_PCT(100), VIEW_SCR_H);
    lv_obj_set_pos(sbox, 0, 0);
    lv_obj_set_style_bg_opa(sbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(sbox, 0, 0);
    lv_obj_set_style_border_width(sbox, 0, 0);
    lv_obj_add_flag(sbox,   LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sbox,   LV_OBJ_FLAG_CLICKABLE);
    /* v1.2.1 (2026-05-18): explicit momentum + elastic clear.
     *  - SCROLL_MOMENTUM: parmak bırakıldıktan sonra inertia ile devam et
     *    (gallery_app pattern'i, tutarlılık için settings'te de).
     *  - SCROLL_ELASTIC clear: over-scroll bounce SW renderer'da pahalı +
     *    410×410 yuvarlak ekranda görsel olarak çirkin (ekran dışı taşma). */
    lv_obj_add_flag(sbox,   LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(sbox, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(sbox, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(sbox, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scrollbar_mode(sbox, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(sbox, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(sbox, LV_OBJ_FLAG_EVENT_BUBBLE);
    // Scrollbar stili
    lv_obj_set_style_bg_color(sbox, lv_color_make(0x66, 0x66, 0x66), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(sbox,   LV_OPA_COVER,     LV_PART_SCROLLBAR);
    lv_obj_set_style_width(sbox,    3,                LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(sbox,   LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(sbox, 0,               LV_PART_SCROLLBAR);

    // İç panel — yüksekliği içeriğe göre otomatik büyür.
    // pad_top=VIEW_HEADER_H: ilk içerik başlangıçta header'ın tam altında başlar.
    lv_obj_t* panel = lv_obj_create(sbox);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel,  SETTINGS_SAFE_W);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_pos(panel, SETTINGS_SAFE_X, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_pad_top(panel,    VIEW_HEADER_H, 0);  // içerik header altından başlar
    lv_obj_set_style_pad_bottom(panel, 48, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel,   LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(panel,   LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(panel,   LV_OBJ_FLAG_EVENT_BUBBLE);

    if (out_content) *out_content = panel;
    return v;
}

static void set_content_height(lv_obj_t* content, int32_t bottom_y)
{
    // Content yüksekliği LV_PCT(100) = 502px olarak sabit kalır.
    // Children bu sınırı aşınca LVGL otomatik scroll range hesaplar.
    // Yüksekliği children extent'e eşitlemek scroll range'i sıfırlar → kaydırma çalışmaz.
    (void)content;
    (void)bottom_y;
}

// Yatay ayraç çizgisi
static void make_sep(lv_obj_t* parent, int32_t y)
{
    lv_obj_t* sep = lv_obj_create(parent);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, SETTINGS_SAFE_W, 1);
    lv_obj_set_pos(sep, 0, y);
    lv_obj_set_style_bg_color(sep, lv_color_make(0x2C, 0x2C, 0x2E), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sep, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
}

// Üst bar: sol=mavi başlık (geri oku ile veya düz), sağ=saat
// back_cb != NULL → "< Başlık" + tıklanabilir (alt ekran)
// back_cb == NULL → "Başlık" düz (ana ekran)
static void make_header(lv_obj_t* parent, const char* title, lv_event_cb_t back_cb)
{
    // Siyah opak bar: scroll eden içeriği header bölgesinde maskeler.
    // sbox'un arkasında ama header label'larının önünde render edilir.
    lv_obj_t* hdr_bg = lv_obj_create(parent);
    lv_obj_remove_style_all(hdr_bg);
    lv_obj_set_size(hdr_bg, LV_PCT(100), VIEW_HEADER_H);
    lv_obj_set_pos(hdr_bg, 0, 0);
    lv_obj_set_style_bg_color(hdr_bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hdr_bg, LV_OPA_COVER, 0);
    lv_obj_clear_flag(hdr_bg, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                               LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t* hdr = lv_label_create(parent);
    lv_obj_set_style_text_color(hdr, lv_color_make(0x0A, 0x84, 0xFF), 0);
#ifdef BOARD_VARIANT_LCD128
    /* 240 yuvarlakta başlık+saat yan yana sığmıyor (chord ~148 px) —
     * iki katman: saat üstte ortada (y=4, 14px), başlık altında (y=26).
     * VIEW_HEADER_H=46: 4+14 | 26+16=42 → ikisi de header bandında. */
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_16, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 58, 26);
#else
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 50, 26);
#endif

    if (back_cb) {
        char buf[48];
        snprintf(buf, sizeof(buf), LV_SYMBOL_LEFT "  %s", title);
        lv_label_set_text(hdr, buf);
        lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
        /* v1.4.64: 18 → 28 — Settings'in tüm alt sayfaları bu header'ı kullanır,
         * en sık tıklanan back hedefi. Apple HIG min 44×44'e ulaşır. */
        lv_obj_set_ext_click_area(hdr, 28);
        lv_obj_add_event_cb(hdr, back_cb, LV_EVENT_CLICKED, NULL);
    } else {
        lv_label_set_text(hdr, title);
    }

    // Saat — sağ üst. AMOLED yuvarlak köşe (r≈120) + 24 px font ile
    // "%02d:%02d" yaklaşık 75 px genişlik. y=26'da sağ görünür sınır
    // ~x=365; -28 ofset → 382'de bitiyordu (17 px taşma). -50'ye
    // çekiyoruz: 360 → görünür alan içinde 5 px marj.
    //
    // LCD128: saat başlıkla aynı satırda değil — ekranın tepesinde ortada
    // (yuvarlak ekranın o yüksekliğindeki ~100 px'lik şerit saate yeter),
    // başlık bir alt satırda tam genişlikte (cihaz testi 2026-08-04).
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    lv_obj_t* clk = lv_label_create(parent);
    lv_obj_set_style_text_color(clk, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(clk, &lv_font_montserrat_14, 0);
    lv_obj_align(clk, LV_ALIGN_TOP_MID, 0, 4);
#else
    lv_obj_set_style_text_font(clk, &lv_font_montserrat_24, 0);
    lv_obj_align(clk, LV_ALIGN_TOP_RIGHT, -50, 26);
#endif
    lv_label_set_text(clk, tbuf);

    // Z-order: sbox(arka) → hdr_bg(maske) → hdr → clk(ön)
    lv_obj_move_to_index(hdr_bg, -1);
    lv_obj_move_to_index(hdr,    -1);
    lv_obj_move_to_index(clk,    -1);
}

// Koyu kart satırı (text=NULL ise etiket oluşturulmaz)
static lv_obj_t* make_row(lv_obj_t* parent, int32_t y, const char* text)
{
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_style_bg_color(row,      lv_color_make(0x1C, 0x1C, 0x1E), 0);
    lv_obj_set_style_bg_opa(row,        LV_OPA_COVER,  0);
    lv_obj_set_style_bg_grad_opa(row,   LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row,        16,            0);
    lv_obj_set_style_border_width(row,  1,             0);
    lv_obj_set_style_border_color(row,  lv_color_make(0x2C, 0x2C, 0x2E), 0);
    lv_obj_set_style_outline_width(row, 0,             0);
    lv_obj_set_style_shadow_width(row,  0,             0);
    lv_obj_set_style_pad_all(row,       0,             0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(row, SETTINGS_SAFE_W, 52);
#else
    lv_obj_set_size(row, SETTINGS_SAFE_W, 90);
#endif
    lv_obj_set_pos(row, 0, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);

    if (text && text[0]) {
        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
#else
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
#endif
        /* DEFANS 1: long_mode CLIP (text wraparound olmasın). */
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        /* DEFANS 2: Explicit width — toggle 64 px sağda; label sol kenardan
         * 18 + 250 = sağ kenardan ~118 px = 386-118=268 → güvenli. Label
         * width AÇIKÇA set edilirse LVGL'in herhangi bir state geçişinde
         * width'i değiştirmesinin önüne geçmiş oluruz. */
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_width(lbl, 126);
#else
        lv_obj_set_width(lbl, 250);
#endif
        /* DEFANS 3: Tüm interactive state'lerde transform_scale=256,
         * translate=0 lock. Theme PRESSED'inin label'a sızdırdığı subtle
         * transform = label'in pozisyonunun anlık değişmesi → render
         * artifact (eski + yeni position aynı frame'de). */
        lv_obj_set_style_transform_scale_x(lbl, 256, 0);
        lv_obj_set_style_transform_scale_y(lbl, 256, 0);
        lv_obj_set_style_translate_x(lbl, 0, 0);
        lv_obj_set_style_translate_y(lbl, 0, 0);
        lv_obj_set_style_transform_scale_x(lbl, 256, LV_STATE_PRESSED);
        lv_obj_set_style_transform_scale_y(lbl, 256, LV_STATE_PRESSED);
        lv_obj_set_style_translate_x(lbl, 0, LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(lbl, 0, LV_STATE_PRESSED);
#ifdef BOARD_VARIANT_LCD128
        lv_obj_set_pos(lbl, 14, 16);   /* row 52 px, label ~20 px → dikey orta */
#else
        lv_obj_set_pos(lbl, 18, 31);   /* Manuel pos: row 90 px, label ~28 px → y=31 (vertical center) */
#endif
        lv_label_set_text(lbl, text);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    return row;
}

// Navigasyon satırı: yuvarlak renkli ikon + metin + sağ chevron (>)
// symbol → LVGL sembolü (LV_SYMBOL_WIFI vb.), NULL ise img_icon kullanılır
// img_icon → lv_image_dsc_t (ör. sun_icon_16), NULL ise symbol kullanılır
static lv_obj_t* make_nav_row(lv_obj_t* parent, int32_t y,
                               const char* text,
                               const char* symbol,
                               lv_color_t icon_bg,
                               const lv_image_dsc_t* img_icon = nullptr)
{
    lv_obj_t* row = make_row(parent, y, NULL);   // metinsiz kart

    // ── İkon (sadece symbol veya img_icon verilmişse) ────────────────────────
#ifdef BOARD_VARIANT_LCD128
    const int32_t ICON_D  = 26;
    const int32_t ICON_X  = 8;
    bool has_icon = (symbol != nullptr || img_icon != nullptr);
    const int32_t TEXT_X  = has_icon ? (ICON_X + ICON_D + 8) : 10;
#else
    const int32_t ICON_D  = 40;
    const int32_t ICON_X  = 16;
    bool has_icon = (symbol != nullptr || img_icon != nullptr);
    const int32_t TEXT_X  = has_icon ? (ICON_X + ICON_D + 12) : 18;  // 68 veya 18
#endif

    if (has_icon) {
        lv_obj_t* circle = lv_obj_create(row);
        lv_obj_remove_style_all(circle);
        lv_obj_set_size(circle, ICON_D, ICON_D);
        lv_obj_align(circle, LV_ALIGN_LEFT_MID, ICON_X, 0);
        lv_obj_set_style_bg_color(circle, icon_bg, 0);
        lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(circle, 0, 0);
        lv_obj_set_style_pad_all(circle, 0, 0);
        lv_obj_clear_flag(circle, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_add_flag(circle, LV_OBJ_FLAG_EVENT_BUBBLE);

        if (img_icon) {
            lv_obj_t* img = lv_image_create(circle);
            lv_image_set_src(img, img_icon);
            /* 40px circle içinde ikonu padding'li yerleştir.
             * Küçük native ikonlar (örn. 16px sun) zaten dar; onları
             * orijinal 1.25× scale'inde tut. Büyük native ikonları
             * (örn. 56px plane) daireye sığdırmak için scale'le. */
            int32_t native = (int32_t)img_icon->header.w;
            int32_t scale;
#ifdef BOARD_VARIANT_LCD128
            /* 26px daire: küçük ikon 1.0× (16→16px), büyük ikon hedef 26px
             * (42px AMOLED hedefi 26px daireden taşıyordu — uçak ikonu). */
            if (native <= 24) {
                scale = 256;
            } else {
                int32_t target = 26;
                scale = (target * 256) / native;
            }
#else
            if (native <= 24) {
                scale = 320;                 /* küçük ikon: 1.25× */
            } else {
                int32_t target = 42;         /* daire içi 42px — plane glyph etrafında native whitespace olduğu için biraz taşma OK */
                scale = (target * 256) / native;
            }
#endif
            lv_image_set_scale(img, scale);
            lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
            lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
            lv_obj_clear_flag(img, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
        } else {
            lv_obj_t* sym_lbl = lv_label_create(circle);
            lv_obj_set_style_text_color(sym_lbl, lv_color_white(), 0);
            /* İkon/daire oranı iki varyantta aynı kalsın: AMOLED 24/40 ≈ 0.60,
             * LCD 16/26 ≈ 0.62 (24px font 26px daireden taşıyordu). */
#ifdef BOARD_VARIANT_LCD128
            lv_obj_set_style_text_font(sym_lbl, &lv_font_montserrat_16, 0);
#else
            lv_obj_set_style_text_font(sym_lbl, &lv_font_montserrat_24, 0);
#endif
            lv_obj_align(sym_lbl, LV_ALIGN_CENTER, 0, 0);
            lv_label_set_text(sym_lbl, symbol);
            lv_obj_clear_flag(sym_lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(sym_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
    }

    // ── Menü metni ────────────────────────────────────────────────────────────
    lv_obj_t* lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
#endif
    // Row ~389px; chevron sağdan ~40px → kullanılabilir alan: 389-40 = 349px
    // İkon varsa TEXT_X=68 → 349-68 = 281px; yoksa TEXT_X=18 → 349-18 = 331px
    // long_mode → set_width sırası: LVGL 9.2 mode değişiminde label state'i
    // sıfırlar; width set edildikten sonra çağrılırsa çalışmaz (scroll-circular).
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_width(lbl, has_icon ? 132 : 164);
#else
    lv_obj_set_width(lbl, has_icon ? 281 : 331);
#endif
    lv_label_set_text(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, TEXT_X, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    // ── Sağ ok (chevron) ──────────────────────────────────────────────────────
    lv_obj_t* chev = lv_label_create(row);
    lv_obj_set_style_text_color(chev, lv_color_make(0x8E, 0x8E, 0x93), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(chev, &lv_font_montserrat_16, 0);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -8, 0);
#else
    lv_obj_set_style_text_font(chev, &lv_font_montserrat_24, 0);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -16, 0);
#endif
    lv_label_set_text(chev, LV_SYMBOL_RIGHT);
    lv_obj_clear_flag(chev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chev, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Apple Watch tap tactility: nav row tıklanınca 0.96× scale + spring
     * back. Helper PRESSED state transform pinning'i kaldırır → row önceden
     * pinli olduğu için make_row'da uygulanan defansif pinning override
     * edilmiş olur (bu OK, helper kendi pinning'ini yönetiyor). */
    wear_press_feedback_attach(row);

    return row;
}

// iOS toggle (yeşil açık, gri kapalı)
/* iOS-tarzı toggle — LVGL lv_switch widget'ı KULLANILMIYOR.
 *
 * NEDEN SIFIRDAN ÖZEL: lv_switch'in dahili state-transition mantığı
 * (knob animasyonu, indicator part'ı, çoklu state-specific stiller,
 * default tema hook'ları) tekrarlanan partial-redraw artefaktlarına
 * sebep oluyordu — anim_duration=0, tüm border/outline/shadow sıfırlama,
 * transform pinleme ve async DOM defer denendi, hiçbiri ilk-toggle
 * "kayma/wedge" artefaktını tamamen gidermedi.
 *
 * Bu implementation sadece İKİ lv_obj_t kullanıyor:
 *   - outer  : 64x34 yuvarlak dikdörtgen, bg rengi state'e göre
 *              (gri: off / iOS yeşil: on)
 *   - knob   : 30x30 beyaz daire, outer içinde sola/sağa hizalı
 *
 * State flip'i `toggle_set_checked()` fonksiyonu YÖNETIR: iki style
 * değişikliği (outer bg_color + knob align). Animasyon yok, state-
 * specific style yok, indicator part yok → ara-render artefaktı da yok.
 * LVGL'in kendi state-transition pipeline'ına hiç girmiyoruz. */

static inline void toggle_set_checked(lv_obj_t* outer, lv_obj_t* knob, bool on)
{
    if (!outer || !knob) return;
    lv_obj_set_style_bg_color(outer,
        on ? lv_color_make(0x30, 0xD1, 0x58)     /* iOS green */
           : lv_color_make(0x39, 0x39, 0x3B),    /* iOS off gray */
        0);
#ifdef BOARD_VARIANT_LCD128
    /* Knob 20px, outer 44px → On: x=44-2-20=22. */
    lv_obj_set_x(knob, on ? 22 : 2);
#else
    /* Knob 30px, outer 64px → kenardan 2px pad, slot 32px.
     * Off: x=2 (sola dayalı), On: x=32 (sağa dayalı). */
    lv_obj_set_x(knob, on ? 32 : 2);
#endif
    /* Toggle'ı "CHECKED" state'ine sokmuyoruz — tema default'una
     * düşmesin diye. Durum bilgisi s_wifi_enabled'da tutuluyor. */
}

static lv_obj_t* make_toggle_ex(lv_obj_t* parent, bool checked, lv_obj_t** out_knob)
{
    lv_obj_t* sw = lv_obj_create(parent);
    lv_obj_remove_style_all(sw);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(sw, 44, 24);
#else
    lv_obj_set_size(sw, 64, 34);
#endif
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(sw, 0, 0);
    lv_obj_set_style_outline_width(sw, 0, 0);
    lv_obj_set_style_shadow_width(sw, 0, 0);
    lv_obj_set_style_pad_all(sw, 0, 0);
    /* Transform pinning: tüm state'lerde scale=256, translate=0. Theme
     * PRESSED state'inin (parent row press olunca child SW'ye sızabilir)
     * subtle transform uygulamasını engelle. */
    lv_obj_set_style_transform_scale_x(sw, 256, 0);
    lv_obj_set_style_transform_scale_y(sw, 256, 0);
    lv_obj_set_style_translate_x(sw, 0, 0);
    lv_obj_set_style_translate_y(sw, 0, 0);
    lv_obj_set_style_transform_scale_x(sw, 256, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_y(sw, 256, LV_STATE_PRESSED);
    lv_obj_set_style_translate_x(sw, 0, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(sw, 0, LV_STATE_PRESSED);
    lv_obj_clear_flag(sw, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                           LV_OBJ_FLAG_CLICKABLE |
                                           LV_OBJ_FLAG_CLICK_FOCUSABLE |
                                           LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    lv_obj_add_flag(sw, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* knob = lv_obj_create(sw);
    lv_obj_remove_style_all(knob);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(knob, 20, 20);
    lv_obj_set_y(knob, 2);
#else
    lv_obj_set_size(knob, 30, 30);
    lv_obj_set_y(knob, 2);
#endif
    lv_obj_set_style_bg_color(knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(knob, 0, 0);
    lv_obj_set_style_outline_width(knob, 0, 0);
    lv_obj_set_style_shadow_width(knob, 0, 0);
    lv_obj_set_style_pad_all(knob, 0, 0);
    /* Transform pinning — knob için de tüm state'lerde lock. */
    lv_obj_set_style_transform_scale_x(knob, 256, 0);
    lv_obj_set_style_transform_scale_y(knob, 256, 0);
    lv_obj_set_style_translate_x(knob, 0, 0);
    lv_obj_set_style_translate_y(knob, 0, 0);
    lv_obj_set_style_transform_scale_x(knob, 256, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_y(knob, 256, LV_STATE_PRESSED);
    lv_obj_set_style_translate_x(knob, 0, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(knob, 0, LV_STATE_PRESSED);
    lv_obj_clear_flag(knob, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                             LV_OBJ_FLAG_CLICKABLE |
                                             LV_OBJ_FLAG_CLICK_FOCUSABLE |
                                             LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    lv_obj_add_flag(knob, LV_OBJ_FLAG_EVENT_BUBBLE);

    if (out_knob) *out_knob = knob;
    toggle_set_checked(sw, knob, checked);
    return sw;
}

static lv_obj_t* make_toggle(lv_obj_t* parent, bool checked)
{
    return make_toggle_ex(parent, checked, &s_wifi_knob);
}

// ─── Uçak Modu callbacks ──────────────────────────────────────────────────────

static void apm_apply_effect(bool on)
{
    if (on) {
        s_wifi_was_on_pre_apm = s_wifi_enabled;
        if (s_airplane_pref_wifi && s_wifi_enabled) {
            /* Toggle görselini hemen flip et — kullanıcı dokunduğunda
             * anında geri bildirim. */
            if (s_wifi_switch && s_wifi_knob) {
                toggle_set_checked(s_wifi_switch, s_wifi_knob, false);
                lv_obj_invalidate(s_wifi_switch);
            }
            /* WiFi pref de güncelle: airplane mode WiFi'yi kapadıysa, reboot
             * sonra da kapalı kalsın. wifi_val_apply içinde s_wifi_enabled
             * = false yapılacak. */
            wifi_service_set_user_preference(false);
            /* Tam WiFi-OFF teardown: connected_row, ağ listesi, section
             * label, s_connected_ssid hepsi temizlenir. wifi_row_cb ile
             * aynı yol. */
            lv_async_call(wifi_val_apply, (void*)(uintptr_t)0);
        }
    } else {
        if (s_airplane_pref_wifi && s_wifi_was_on_pre_apm && !s_wifi_enabled) {
            if (s_wifi_switch && s_wifi_knob) {
                toggle_set_checked(s_wifi_switch, s_wifi_knob, true);
                lv_obj_invalidate(s_wifi_switch);
            }
            /* Airplane mode kapanıyor + öncesinde WiFi açıktı → tekrar aç. */
            wifi_service_set_user_preference(true);
            lv_async_call(wifi_val_apply, (void*)(uintptr_t)1);
        }
    }
}

static void apm_master_row_cb(lv_event_t* e)
{
    (void)e;
    bool new_state = !s_airplane_enabled;
    s_airplane_enabled = new_state;
    if (s_apm_master_switch && s_apm_master_knob) {
        toggle_set_checked(s_apm_master_switch, s_apm_master_knob, new_state);
        lv_obj_invalidate(s_apm_master_switch);
    }
    apm_apply_effect(new_state);
}

static void apm_wifi_row_cb(lv_event_t* e)
{
    (void)e;
    s_airplane_pref_wifi = !s_airplane_pref_wifi;
    if (s_apm_wifi_switch && s_apm_wifi_knob) {
        toggle_set_checked(s_apm_wifi_switch, s_apm_wifi_knob, s_airplane_pref_wifi);
        lv_obj_invalidate(s_apm_wifi_switch);
    }
    /* Eğer uçak modu açıkken bu tercih değiştirildi → etkiyi anında uygula */
    if (s_airplane_enabled) {
        if (s_airplane_pref_wifi && s_wifi_enabled) {
            s_wifi_was_on_pre_apm = true;
            if (s_wifi_switch && s_wifi_knob) {
                toggle_set_checked(s_wifi_switch, s_wifi_knob, false);
                lv_obj_invalidate(s_wifi_switch);
            }
            lv_async_call(wifi_val_apply, (void*)(uintptr_t)0);
        }
    }
}

static void apm_bt_row_cb(lv_event_t* e)
{
    (void)e;
    s_airplane_pref_bt = !s_airplane_pref_bt;
    if (s_apm_bt_switch && s_apm_bt_knob) {
        toggle_set_checked(s_apm_bt_switch, s_apm_bt_knob, s_airplane_pref_bt);
        lv_obj_invalidate(s_apm_bt_switch);
    }
}

// ─── View builder'ları ────────────────────────────────────────────────────────

// ANA EKRAN — General / WiFi / Display & Brightness
static void build_main(lv_obj_t* screen)
{
    s_view_main = make_view(screen, &s_main_content);
    make_header(s_view_main, "Settings", NULL);  // geri oku yok
    make_sep(s_main_content, 0);

#ifdef BOARD_VARIANT_LCD128
    const int32_t Y0   = 8;
    const int32_t STEP = 60;
#else
    const int32_t Y0   = 8;
    const int32_t STEP = 98;
#endif
    s_main_nav.count = 0;
    s_main_nav.content = s_main_content;
    s_main_nav.rows[s_main_nav.count] = make_nav_row(s_main_content, Y0,
        "General", LV_SYMBOL_SETTINGS, lv_color_make(0x63, 0x63, 0x66));
    s_main_nav.cbs[s_main_nav.count++] = go_general_cb;
    s_main_nav.rows[s_main_nav.count] = make_nav_row(s_main_content, Y0 + STEP,
        "Airplane Mode", nullptr, lv_color_make(0xFF, 0x9F, 0x0A), &plane_icon);
    s_main_nav.cbs[s_main_nav.count++] = go_airplane_cb;
    s_main_nav.rows[s_main_nav.count] = make_nav_row(s_main_content, Y0 + STEP * 2,
        "WiFi", LV_SYMBOL_WIFI, lv_color_make(0x0A, 0x84, 0xFF));
    s_main_nav.cbs[s_main_nav.count++] = go_network_cb;
    s_main_nav.rows[s_main_nav.count] = make_nav_row(s_main_content, Y0 + STEP * 3,
        "Display & Brightness", nullptr, lv_color_make(0x30, 0xB0, 0xC7), &sun_icon_16);
    s_main_nav.cbs[s_main_nav.count++] = go_display_cb;
    /* v1.1.1: Güç Rayları — AXP rail toggle'ları + Soft Power Off butonu.
     * Kırmızı bg (iOS red) ikon: power simgesi. Settings ana menüsünün en
     * altında, en görünür/dikkat çekici slot. */
    s_main_nav.rows[s_main_nav.count] = make_nav_row(s_main_content, Y0 + STEP * 4,
        "Power Rails", LV_SYMBOL_POWER, lv_color_make(0xFF, 0x3B, 0x30));
    s_main_nav.cbs[s_main_nav.count++] = go_power_cb;
    lv_obj_add_event_cb(s_main_content, nav_list_cb, LV_EVENT_PRESSED,    &s_main_nav);
    lv_obj_add_event_cb(s_main_content, nav_list_cb, LV_EVENT_PRESSING,   &s_main_nav);
    lv_obj_add_event_cb(s_main_content, nav_list_cb, LV_EVENT_RELEASED,   &s_main_nav);
    lv_obj_add_event_cb(s_main_content, nav_list_cb, LV_EVENT_PRESS_LOST, &s_main_nav);
    set_content_height(s_main_content, Y0 + STEP * 4 + 90);
}

// ALT EKRAN: General → About + Software Updates
// v1'den itibaren Software Updates aktif. About'un altında ikinci nav satırı.
//
// STEP=108 (önceki 90): Apple Watch tarzı row-arası ~25 px breathing room.
// 90'da rows birbirine yapışmış görünüyordu (kullanıcı raporu 2026-05-05).
//
// İkonlar:
//   About            → LV_SYMBOL_HOME (gri bg) — cihaz/info
//   Software Updates → LV_SYMBOL_DOWNLOAD (mavi bg) — Apple Watch'ın SWU ikonu
static void build_general(lv_obj_t* screen)
{
    s_view_general = make_view(screen, &s_general_content);
    make_header(s_view_general, "General", go_main_cb);
    make_sep(s_general_content, 0);

#ifdef BOARD_VARIANT_LCD128
    const int32_t Y0   = 8;
    const int32_t STEP = 60;    /* row 52 + 8 boşluk — ana listeyle aynı ritim */
#else
    const int32_t Y0 = 8;
    const int32_t STEP = 108;
#endif
    s_general_nav.count = 0;
    s_general_nav.content = s_general_content;

    /* About — gri ikon, cihaz bilgileri */
    s_general_nav.rows[s_general_nav.count] = make_nav_row(s_general_content, Y0,
        "About", LV_SYMBOL_HOME, lv_color_make(0x63, 0x63, 0x66));
    s_general_nav.cbs[s_general_nav.count++] = go_about_cb;

    /* v1.2.3 (2026-05-19): Storage — mor ikon (SD_CARD sembolü),
     * depolama kullanım detayları (Flash + SPIFFS + PSRAM + SRAM + NVS). */
    s_general_nav.rows[s_general_nav.count] = make_nav_row(s_general_content,
        Y0 + STEP, "Storage", LV_SYMBOL_SD_CARD,
        lv_color_make(0xAF, 0x52, 0xDE));
    s_general_nav.cbs[s_general_nav.count++] = go_storage_cb;

    /* Software Updates — mavi ikon, WiFi üzerinden firmware güncelleme.
     * v1'den itibaren aktif (v0.1'de kaldırılmıştı, partition tablosu
     * korunmuştu). Tıklanınca go_swupdate_cb otomatik olarak
     * ota_service_check_async() tetikler. */
    s_general_nav.rows[s_general_nav.count] = make_nav_row(s_general_content,
        Y0 + STEP * 2, "Software Updates", LV_SYMBOL_DOWNLOAD,
        lv_color_make(0x0A, 0x84, 0xFF));
    s_general_nav.cbs[s_general_nav.count++] = go_swupdate_cb;

    /* Otomatik Güncelleme — yeşil ikon (refresh/loop). Alt sayfada toggle
     * + Apple Watch tarzı açıklama metni. NVS-persisted preference. */
    s_general_nav.rows[s_general_nav.count] = make_nav_row(s_general_content,
        Y0 + STEP * 3, "Automatic Updates", LV_SYMBOL_REFRESH,
        lv_color_make(0x32, 0xD7, 0x4D));
    s_general_nav.cbs[s_general_nav.count++] = go_auto_update_cb;

    lv_obj_add_event_cb(s_general_content, nav_list_cb, LV_EVENT_PRESSED,    &s_general_nav);
    lv_obj_add_event_cb(s_general_content, nav_list_cb, LV_EVENT_PRESSING,   &s_general_nav);
    lv_obj_add_event_cb(s_general_content, nav_list_cb, LV_EVENT_RELEASED,   &s_general_nav);
    lv_obj_add_event_cb(s_general_content, nav_list_cb, LV_EVENT_PRESS_LOST, &s_general_nav);
#ifdef BOARD_VARIANT_LCD128
    set_content_height(s_general_content, Y0 + STEP * 4 + 60);   /* 4 row, LCD alt payı */
#else
    set_content_height(s_general_content, Y0 + STEP * 4 + 90);   /* v1.2.3: 4 row */
#endif
}

// ─── Apple Watch tarzı düz bilgi satırı ──────────────────────────────────────
// Kart/box yok. Siyah arka plan, güçlü başlık + belirgin değer.
// Satırlar arasında ince separator çizgisi.
static int32_t make_about_row(lv_obj_t* parent, int32_t y,
                               const char* label, const char* value)
{
#ifdef BOARD_VARIANT_LCD128
    /* Session 38: 240 yuvarlak ekran — panel 204 px geniş. Eski değerler
     * (LEFT 20, W 370, font 24) AMOLED içindi; "Corezzle Smart Watch" gibi
     * değerler sağdan kırpılıyordu (tw=271 > 204). */
    const int32_t LEFT  = 14;
    const int32_t W     = 176;
    const lv_font_t* F  = &lv_font_montserrat_16;
    const int32_t GAP_LV = 2;
    const int32_t GAP_ROW = 12;
#else
    const int32_t LEFT  = 20;
    const int32_t W     = 370;
    const lv_font_t* F  = &lv_font_montserrat_24;
    const int32_t GAP_LV = 6;   // label -> value
    const int32_t GAP_ROW = 18; // block spacing
#endif

    // Başlık — long_mode set'i width'ten ÖNCE; aksi halde değer string'i
    // bir satıra sığmazsa LVGL default SCROLL_CIRCULAR'a düşer (wraparound).
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl,  F, 0);
    lv_obj_set_style_text_line_space(lbl, 4, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, W);
    lv_obj_set_pos(lbl, LEFT, y);
    lv_label_set_text(lbl, label);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    // Değer — bir satır aşağıda, daha büyük
    lv_obj_t* val = lv_label_create(parent);
    lv_obj_set_style_text_color(val, lv_color_make(0x8E, 0x8E, 0x93), 0);
    lv_obj_set_style_text_font(val,  F, 0);
    lv_obj_set_style_text_line_space(val, 4, 0);
    lv_label_set_long_mode(val, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(val, W);
    lv_obj_set_pos(val, LEFT, y + lv_font_get_line_height(F) + GAP_LV);
    lv_label_set_text(val, value);
    lv_obj_clear_flag(val, LV_OBJ_FLAG_CLICKABLE);

    lv_point_t val_size;
    lv_text_get_size(&val_size, value, F, 0, 0, W, LV_TEXT_FLAG_NONE);
    const int32_t label_h = lv_font_get_line_height(F);
    return y + label_h + GAP_LV + val_size.y + GAP_ROW;
}

// ─── Otomatik Güncelleme toggle row click handler ────────────────────────────
// Row tıklanınca s_auto_update_enabled flip, toggle visual güncelle, NVS'e yaz.
static bool s_auto_update_enabled = false;
static void au_row_clicked_cb(lv_event_t* /*e*/)
{
    s_auto_update_enabled = !s_auto_update_enabled;
    if (s_au_switch && s_au_knob) {
        toggle_set_checked(s_au_switch, s_au_knob, s_auto_update_enabled);
    }
    ota_service_set_auto_update(s_auto_update_enabled);
}

// ALT EKRAN: General → Otomatik Güncelleme (toggle + açıklama metni)
//
// Layout: üstte kart (label + toggle), altında uzun wrap'lı açıklama
// paragrafı. Apple Watch'taki Otomatik Güncellemeler sayfasının birebir
// karşılığı. Sayfa açıldığında NVS'ten okunup toggle pozisyonu güncellenir.
static void build_auto_update(lv_obj_t* screen)
{
    lv_obj_t* content = NULL;
    s_view_auto_update = make_view(screen, &content);
    make_header(s_view_auto_update, "Automatic Updates", go_general_cb);

    /* NVS'ten mevcut tercihi al */
    s_auto_update_enabled = ota_service_get_auto_update();

    /* Kart row: title sol, toggle sağ.
     * make_row'a NULL text geçiyoruz — kendi label'ımızı oluşturup width'i
     * "Automatic Updates" (19 char, font_22) için yeterli set ediyoruz.
     * make_row'un default 250px width'i sığmıyordu, son "e" harfi kırpılıyordu
     * (kullanıcı raporu 2026-05-11). */
    lv_obj_t* row = make_row(content, 8, NULL);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_make(0x1C, 0x1C, 0x1E),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(row, 256, 0);
    lv_obj_set_style_transform_scale(row, 256, LV_STATE_PRESSED);

    /* Custom title label — daha küçük font + yeterli width */
    lv_obj_t* title_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(title_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_22, 0);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(title_lbl, 290);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 18, 0);
    lv_label_set_text(title_lbl, "Automatic Updates");
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(title_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Toggle widget'ı make_toggle_ex ile — knob pointer'ını s_au_knob'a al */
    s_au_switch = make_toggle_ex(row, s_auto_update_enabled, &s_au_knob);
    lv_obj_align(s_au_switch, LV_ALIGN_RIGHT_MID, -14, 0);

    lv_obj_add_event_cb(row, au_row_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Açıklama paragrafı — kartın altında, gri, multi-line wrap. Gece
     * penceresi (00:00-06:00) bilgisi açıkça yer alıyor ki kullanıcı ne
     * zaman güncelleneceğini bilsin. */
    lv_obj_t* desc = lv_label_create(content);
    lv_obj_set_style_text_color(desc, lv_color_make(0x8E, 0x8E, 0x93), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(desc, 4, 0);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, SETTINGS_SAFE_W - 16);
    lv_obj_set_pos(desc, 8, 8 + 90 + 16);  // row bittikten 16 px sonra
    lv_label_set_text(desc,
        "When on: new versions are downloaded and installed automatically "
        "at night (between 00:00 - 06:00) and the watch restarts.\n\n"
        "When off: you can manually start each update using the "
        "'Download and Install' button.");
    lv_obj_clear_flag(desc, LV_OBJ_FLAG_CLICKABLE);

    set_content_height(content, 8 + 90 + 16 + 260);
}

// ALT EKRAN: About → Cihaz bilgileri
static void build_about(lv_obj_t* screen)
{
    s_view_about = make_view(screen, &s_about_content);
    make_header(s_view_about, "About", go_general_cb);

    char capacity_buf[24];
    char space_buf[24];
    uint32_t cap  = get_flash_capacity_bytes();
    uint32_t used = get_flash_used_bytes();
    uint32_t free = (cap > used) ? (cap - used) : 0;
    if (cap == 0) {
        snprintf(capacity_buf, sizeof(capacity_buf), "Unknown");
        snprintf(space_buf,    sizeof(space_buf),    "Unknown");
    } else {
        format_bytes(cap,  capacity_buf, sizeof(capacity_buf));
        format_bytes(free, space_buf,    sizeof(space_buf));
    }

    int32_t y = 10;
    y = make_about_row(s_about_content, y, "Ad",               "Corezzle Smart Watch");
    y = make_about_row(s_about_content, y, "Software Version", OS_VERSION_STR);
    y = make_about_row(s_about_content, y, "Model Name",       "NAR-ESP32S3");
    y = make_about_row(s_about_content, y, "Capacity",         capacity_buf);
    y = make_about_row(s_about_content, y, "Space",            space_buf);
    (void)y;
}

// ─── v1.2.3 (2026-05-19): ALT EKRAN — General → Storage ──────────────────────
//
// 5 kategori storage info: App / SPIFFS / PSRAM / SRAM / NVS.
// Üstte combined Flash kullanım bar'ı (capacity vs used), altta kategori
// listesi (renkli icon + ad + "X / Y" formatlı boyut + yüzde).
//
// Apple Watch tarzı: kart yok, düz liste, satır arası ince separator.
// İlk açılışta ölçüm bir kez yapılır; refresh için sayfayı kapatıp tekrar
// açmak yeter (heap fragmentation snapshot için yeterli).

/* Tek kategori satırı — sol icon kutusu (renkli arka plan), ortada label
 * üstte ad altta "used / total", sağda yüzde. Apple Watch Storage'a benzer. */
static int32_t make_storage_row(lv_obj_t* parent, int32_t y,
                                 const char* name,
                                 lv_color_t icon_color,
                                 uint32_t used, uint32_t total)
{
#ifdef BOARD_VARIANT_LCD128
    /* Session 38: 240 yuvarlak — panel 204 px. Eski değerler AMOLED içindi:
     * yüzde etiketi x=359'a düşüyordu (EKRAN DIŞI, hiç görünmüyordu) ve
     * ad/boyut yazıları sağdan kırpılıyordu. */
    const int32_t LEFT     = 8;
    const int32_t ICON_SZ  = 30;
    const int32_t LABEL_X  = LEFT + ICON_SZ + 8;
    const int32_t ROW_H    = 48;
    const int32_t RIGHT_X  = 196;
    const lv_font_t* F_NAME = &lv_font_montserrat_14;
    const lv_font_t* F_SIZE = &lv_font_montserrat_12;
    const lv_font_t* F_PCT  = &lv_font_montserrat_14;
    const int32_t NAME_DY  = 8;
    const int32_t SIZE_DY  = 26;
#else
    const int32_t LEFT     = 16;
    const int32_t ICON_SZ  = 44;
    const int32_t LABEL_X  = LEFT + ICON_SZ + 14;
    const int32_t ROW_H    = 76;
    const int32_t RIGHT_X  = LEFT + 370;
    const lv_font_t* F_NAME = &lv_font_montserrat_22;
    const lv_font_t* F_SIZE = &lv_font_montserrat_20;
    const lv_font_t* F_PCT  = &lv_font_montserrat_22;
    const int32_t NAME_DY  = 12;
    const int32_t SIZE_DY  = 40;
#endif

    /* Icon kutusu — renkli yuvarlak köşeli kare (iOS tarzı app icon mimic) */
    lv_obj_t* icon_bg = lv_obj_create(parent);
    lv_obj_remove_style_all(icon_bg);
    lv_obj_set_size(icon_bg, ICON_SZ, ICON_SZ);
    lv_obj_set_pos(icon_bg, LEFT, y + (ROW_H - ICON_SZ) / 2);
    lv_obj_set_style_bg_color(icon_bg, icon_color, 0);
    lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_bg, 10, 0);
    lv_obj_clear_flag(icon_bg, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    /* Kategori adı */
    lv_obj_t* name_lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(name_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(name_lbl, F_NAME, 0);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_pos(name_lbl, LABEL_X, y + NAME_DY);
    lv_obj_clear_flag(name_lbl, LV_OBJ_FLAG_CLICKABLE);

    /* "used / total" string — alt satır, gri */
    char size_buf[40];
    if (total == 0) {
        snprintf(size_buf, sizeof(size_buf), "Not available");
    } else {
        char used_str[16], total_str[16];
        format_bytes(used,  used_str,  sizeof(used_str));
        format_bytes(total, total_str, sizeof(total_str));
        snprintf(size_buf, sizeof(size_buf), "%s / %s", used_str, total_str);
    }
    lv_obj_t* size_lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(size_lbl, lv_color_make(0x8E, 0x8E, 0x93), 0);
    lv_obj_set_style_text_font(size_lbl, F_SIZE, 0);
    lv_label_set_text(size_lbl, size_buf);
    lv_obj_set_pos(size_lbl, LABEL_X, y + SIZE_DY);
    lv_obj_clear_flag(size_lbl, LV_OBJ_FLAG_CLICKABLE);

    /* Yüzde (sağa hizalı) */
    if (total > 0) {
        int pct = (int)((uint64_t)used * 100u / total);
        if (pct > 100) pct = 100;
        if (pct < 0)   pct = 0;
        char pct_buf[16];   /* int -> max 11 byte + "%" + null → safe 16 */
        snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);
        lv_obj_t* pct_lbl = lv_label_create(parent);
        lv_obj_set_style_text_color(pct_lbl, lv_color_make(0xC7, 0xC7, 0xCC), 0);
        lv_obj_set_style_text_font(pct_lbl, F_PCT, 0);
        lv_label_set_text(pct_lbl, pct_buf);
        lv_obj_update_layout(pct_lbl);
        lv_obj_set_pos(pct_lbl, RIGHT_X - lv_obj_get_width(pct_lbl),
                       y + (ROW_H - 28) / 2);
        lv_obj_clear_flag(pct_lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    /* İnce separator çizgisi (satır altı) */
    make_sep(parent, y + ROW_H + 2);

    return y + ROW_H + 8;
}

static void build_storage(lv_obj_t* screen)
{
    s_view_storage = make_view(screen, &s_storage_content);
    make_header(s_view_storage, "Storage", go_general_cb);

    /* v1.2.3 (2026-05-19): üst combined Flash bar kaldırıldı (eski make_storage_top_bar).
     * Sebep: get_flash_used_bytes() partition'ların kapladığı max end address'i
     * veriyordu (= "ayrılmış alan"), gerçek "kullanılan" değildi. Bu hep ~100%
     * gösteriyordu (partition table tüm flash'ı kaplar). 5 kategori listesi
     * zaten her birinin doğru used/total değerini gösterdiği için üst bar
     * gereksiz tekrar + yanıltıcı oluyordu. */
    int32_t y = 16;

    /* 5 kategori satırı — iOS Watch tarzı renkli icon'lar */
    uint32_t total, used;

    /* 1) App (firmware) — mavi */
    get_app_partition_info(&total, &used);
    y = make_storage_row(s_storage_content, y, "Application",
                         lv_color_make(0x0A, 0x84, 0xFF), used, total);

    /* 2) SPIFFS (Yerleşik Depolama) — yeşil */
    get_spiffs_info(&total, &used);
    y = make_storage_row(s_storage_content, y, "Internal Storage",
                         lv_color_make(0x32, 0xD7, 0x4D), used, total);

    /* 3) PSRAM — mor */
    get_psram_info(&total, &used);
    y = make_storage_row(s_storage_content, y, "PSRAM",
                         lv_color_make(0xAF, 0x52, 0xDE), used, total);

    /* 4) SRAM (internal) — turuncu */
    get_sram_info(&total, &used);
    y = make_storage_row(s_storage_content, y, "SRAM",
                         lv_color_make(0xFF, 0x9F, 0x0A), used, total);

    /* 5) NVS (settings) — gri */
    get_nvs_info(&total, &used);
    y = make_storage_row(s_storage_content, y, "NVS (Settings)",
                         lv_color_make(0x8E, 0x8E, 0x93), used, total);

    set_content_height(s_storage_content, y + 24);
}

// ─────────────────────────────────────────────────────────────────────────────
// ALT EKRAN: General → Software Updates
// ─────────────────────────────────────────────────────────────────────────────
//
// Akış:
//   View açılır → go_swupdate_cb otomatik ota_service_check_async() çağırır.
//   1 Hz s_swu_poll_timer state polling yapar; ota_state değişince UI yeniden
//   düzenlenir (icon + title + subtitle + notes + button).
//
// State'ler ve UI:
//   IDLE/CHECKING        → spinner ikon, "Guncelleme aranıyor..."
//   UP_TO_DATE           → ✓ ikon, "Sisteminiz guncel" + mevcut sürüm
//   UPDATE_AVAILABLE     → ↓ ikon, yeni sürüm + notes + "Indir" butonu
//   DOWNLOADING          → progress bar + %, "Please wait"
//   INSTALLED            → ✓ ikon, "Kurulum tamam" + "Yeniden Baslat" butonu
//   ERROR                → ✕ ikon, error_msg + "Tekrar Dene" butonu
//
// View widget'ları statik global olarak deklare edildi (s_swu_*) — view kapanıp
// açıldığında pointer'lar yeniden set ediliyor (build_software_update tüm
// pointer'ları reassign ediyor). Bu sırada eski objeler lv_obj_clean ile
// temizleniyor (view child'ları olduğu için view yeniden açılınca otomatik).

/* Tek event handler — state'e göre dispatch eder. Önceki versiyonda her
 * poll'da (500 ms) lv_obj_remove_event_cb + add yapılıyordu; LVGL 9'da
 * remove_event_cb(event_cb=NULL) hiçbir şey kaldırmadığı için callback'ler
 * yığılıyordu. Tıklama da scrollable parent ile çakışıp düşüyordu.
 *
 * Şimdi tek cb register'lanıyor (view oluşturulurken), state'e göre dispatch:
 *   UPDATE_AVAILABLE → install
 *   INSTALLED        → restart
 *   UP_TO_DATE/ERROR → re-check */
static void swupdate_button_clicked(lv_event_t* /*e*/)
{
    ota_state_t st = ota_service_get_status(NULL);
    ESP_LOGI(TAG, "Software Update button clicked, state=%d", (int)st);
    switch (st) {
    case OTA_STATE_UPDATE_AVAILABLE: {
        ota_service_install_async();
        /* Anlık görsel feedback: poll'u beklemeden DOWNLOADING UI'sine
         * geç. Aksi halde tap sessiz hissediyor (press feedback yok),
         * install_task spawn'lanıp set_state(DOWNLOADING) yapana kadar
         * 200-500 ms gecikme oluyor → kullanıcı "tıklamadı" sanıyor.
         * Poll geldiğinde gerçek progress_pct ile override eder. */
        ota_status_t pre = {};
        pre.state = OTA_STATE_DOWNLOADING;
        pre.progress_pct = 0;
        s_swu_last_state = OTA_STATE_DOWNLOADING;
        swupdate_apply_state(&pre);
        break;
    }
    case OTA_STATE_INSTALLED: {
        /* "Yeniden baslatiliyor / Lutfen bekleyin" UI'sına geç ve butonu
         * gizle, böylece kullanıcı reboot'un başladığını anlasın. 1.5 sn
         * bekle, sonra esp_restart. Önceki 100 ms çok kısaydı — kullanıcı
         * tıklayınca hiçbir şey olmamış gibi hissediyordu (raporu
         * 2026-05-11). Poll'u durduruyoruz, yoksa OTA_STATE_INSTALLED'ı
         * tekrar algılayıp "Kurulum tamam" UI'sını üzerine yazacak. */
        swupdate_stop_polling();
        if (s_swu_icon && s_swu_title && s_swu_subtitle) {
            lv_label_set_text(s_swu_icon, LV_SYMBOL_REFRESH);
            lv_obj_set_style_text_color(s_swu_icon, lv_color_make(0x4F, 0xC3, 0xF7), 0);
            lv_label_set_text(s_swu_title, "Restarting");
            lv_label_set_text(s_swu_subtitle, "Please wait");
        }
        swupdate_set_button(NULL);
        if (s_view_swupdate) lv_obj_invalidate(s_view_swupdate);
        lv_timer_t* t = lv_timer_create([](lv_timer_t* tt) {
            lv_timer_delete(tt);
            esp_restart();
        }, 1500, NULL);
        if (t) lv_timer_set_repeat_count(t, 1);
        break;
    }
    case OTA_STATE_UP_TO_DATE:
    case OTA_STATE_ERROR: {
        ota_service_check_async();
        /* Anlık görsel feedback: CHECKING UI'sine geç. */
        ota_status_t pre = {};
        pre.state = OTA_STATE_CHECKING;
        s_swu_last_state = OTA_STATE_CHECKING;
        swupdate_apply_state(&pre);
        break;
    }
    default:
        /* CHECKING, DOWNLOADING — buton zaten gizli, bu state'lerde bu
         * fonksiyon çağrılmamalı; defansif olarak no-op. */
        break;
    }
}

/* Sadece text ve görünürlük yönetimi — callback dokunulmuyor (build_*'ta
 * tek seferlik register'landı). */
static void swupdate_set_button(const char* text)
{
    if (!s_swu_button || !s_swu_button_lbl) return;
    if (!text) {
        lv_obj_add_flag(s_swu_button, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_swu_button, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_swu_button_lbl, text);
}

/* State'e göre UI'yi yeniden düzenle. ota_status'tan field'ları al. */
static void swupdate_apply_state(const ota_status_t* st)
{
    if (!s_swu_title || !s_swu_subtitle || !s_swu_notes ||
        !s_swu_progress || !s_swu_progress_lbl) return;

    /* Default: progress bar + button gizli; alt-state'e göre uygun olanlar açılır */
    lv_obj_add_flag(s_swu_progress,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_swu_progress_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_swu_notes,        LV_OBJ_FLAG_HIDDEN);

    char buf[128];

    switch (st->state) {
    case OTA_STATE_IDLE:
    case OTA_STATE_CHECKING:
        lv_label_set_text(s_swu_icon,     LV_SYMBOL_LOOP);
        lv_obj_set_style_text_color(s_swu_icon, lv_color_make(0x4F, 0xC3, 0xF7), 0);
        lv_label_set_text(s_swu_title,    "Checking for updates");
        snprintf(buf, sizeof(buf), "Current: v%s", OS_VERSION_STR);
        lv_label_set_text(s_swu_subtitle, buf);
        swupdate_set_button(NULL);
        break;

    case OTA_STATE_UP_TO_DATE:
        lv_label_set_text(s_swu_icon,     LV_SYMBOL_OK);
        lv_obj_set_style_text_color(s_swu_icon, lv_color_make(0x32, 0xD7, 0x4D), 0);
        lv_label_set_text(s_swu_title,    "Your system is up to date");
        snprintf(buf, sizeof(buf), "v%s is the latest.", OS_VERSION_STR);
        lv_label_set_text(s_swu_subtitle, buf);
        swupdate_set_button("Tekrar Kontrol Et");
        break;

    case OTA_STATE_UPDATE_AVAILABLE:
        lv_label_set_text(s_swu_icon,     LV_SYMBOL_DOWNLOAD);
        lv_obj_set_style_text_color(s_swu_icon, lv_color_make(0xFF, 0x9E, 0x3A), 0);
        snprintf(buf, sizeof(buf), "v%s available", st->latest_version);
        lv_label_set_text(s_swu_title, buf);
        if (st->download_size > 0) {
            uint32_t mb_x10 = (st->download_size * 10) / (1024 * 1024);
            snprintf(buf, sizeof(buf), "Boyut: %lu.%lu MB",
                     (unsigned long)(mb_x10 / 10), (unsigned long)(mb_x10 % 10));
            lv_label_set_text(s_swu_subtitle, buf);
        } else {
            lv_label_set_text(s_swu_subtitle, "New version available");
        }
        if (st->notes[0]) {
            lv_obj_clear_flag(s_swu_notes, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_swu_notes, st->notes);
        }
        swupdate_set_button("Download and Install");
        break;

    case OTA_STATE_DOWNLOADING:
        lv_label_set_text(s_swu_icon,     LV_SYMBOL_DOWNLOAD);
        lv_obj_set_style_text_color(s_swu_icon, lv_color_make(0xFF, 0x9E, 0x3A), 0);
        lv_label_set_text(s_swu_title,    "Downloading");
        lv_label_set_text(s_swu_subtitle, "Don't turn off, please wait");
        lv_obj_clear_flag(s_swu_progress,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_swu_progress_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_swu_progress, st->progress_pct, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "%%%u", (unsigned)st->progress_pct);
        lv_label_set_text(s_swu_progress_lbl, buf);
        swupdate_set_button(NULL);
        break;

    case OTA_STATE_INSTALLED:
        /* Auto-update ON ise: install_task ZATEN 5 sn sonra esp_restart
         * çağıracak (UI'a bağımlı değil, kullanıcı Software Updates ekranını
         * kapatmış olsa bile reboot olur). UI sadece doğru mesajı gösteriyor.
         *
         * Auto-update OFF ise: kullanıcı manuel "Simdi Baslat" butonuna
         * basmalı; bu butonun click handler'ı kendi 1.5 sn delay'iyle
         * esp_restart eder. */
        if (ota_service_get_auto_update()) {
            lv_label_set_text(s_swu_icon,     LV_SYMBOL_REFRESH);
            lv_obj_set_style_text_color(s_swu_icon, lv_color_make(0x4F, 0xC3, 0xF7), 0);
            lv_label_set_text(s_swu_title,    "Restarting");
            lv_label_set_text(s_swu_subtitle, "Please wait");
            swupdate_set_button(NULL);  /* buton yok, kullanıcı bekliyor */
        } else {
            lv_label_set_text(s_swu_icon,     LV_SYMBOL_OK);
            lv_obj_set_style_text_color(s_swu_icon, lv_color_make(0x32, 0xD7, 0x4D), 0);
            lv_label_set_text(s_swu_title,    "Installation complete");
            lv_label_set_text(s_swu_subtitle, "Please restart");
            swupdate_set_button("Simdi Baslat");
        }
        break;

    case OTA_STATE_ERROR:
        lv_label_set_text(s_swu_icon,     LV_SYMBOL_WARNING);
        lv_obj_set_style_text_color(s_swu_icon, lv_color_make(0xFF, 0x3B, 0x30), 0);
        lv_label_set_text(s_swu_title,    "Error");
        lv_label_set_text(s_swu_subtitle, st->error_msg[0] ? st->error_msg : "Unknown error");
        swupdate_set_button("Tekrar Dene");
        break;
    }
    /* AMOLED chunked partial-flush bazen state geçişlerinde widget'lar
     * görünürlük değiştirdiğinde "sticker peel" / "torn edge" artifact'i
     * üretiyor (eski + yeni render karışıyor). Tüm view'i invalidate
     * ederek temiz tek-frame redraw zorla. */
    if (s_view_swupdate) lv_obj_invalidate(s_view_swupdate);
}

static void swupdate_poll_cb(lv_timer_t* /*t*/)
{
    if (!s_view_swupdate || lv_obj_has_flag(s_view_swupdate, LV_OBJ_FLAG_HIDDEN)) {
        /* View kapalı; timer atıl çalışmasın */
        swupdate_stop_polling();
        return;
    }
    ota_status_t st;
    ota_state_t  cur = ota_service_get_status(&st);
    if (cur != s_swu_last_state) {
        s_swu_last_state = cur;
        swupdate_apply_state(&st);
    } else if (cur == OTA_STATE_DOWNLOADING) {
        /* Aynı state, ama progress sürekli değişiyor — bar'ı update et */
        if (s_swu_progress) lv_bar_set_value(s_swu_progress, st.progress_pct, LV_ANIM_OFF);
        if (s_swu_progress_lbl) {
            char b[16];
            snprintf(b, sizeof(b), "%%%u", (unsigned)st.progress_pct);
            lv_label_set_text(s_swu_progress_lbl, b);
        }
    }
}

static void swupdate_start_polling(void)
{
    if (s_swu_poll_timer) return;
    s_swu_poll_timer = lv_timer_create(swupdate_poll_cb, 500, NULL);
}

static void swupdate_stop_polling(void)
{
    if (s_swu_poll_timer) {
        lv_timer_delete(s_swu_poll_timer);
        s_swu_poll_timer = NULL;
    }
}

static void build_software_update(lv_obj_t* screen)
{
    s_view_swupdate = make_view(screen, &s_swupdate_content);
    make_header(s_view_swupdate, "Software Update", go_general_cb);

    /* Layout: tek-sütun, dikey sıkı yerleşim. SETTINGS_SAFE_W=386 (panel
     * genişliği). Y koordinatları panel-local (header altı pad_top=74'ten
     * sonra başlar). Pozisyonlar UPDATE_AVAILABLE'da notes ve buton
     * çakışmasın diye sıkıştırıldı (önceki: notes y=210, btn y=270 → 3+
     * satırlık notes butonun üzerine biniyordu). */

    /* Üst icon — büyük tek karakter, state'e göre değişir */
    s_swu_icon = lv_label_create(s_swupdate_content);
    lv_obj_set_style_text_font(s_swu_icon, &lv_font_montserrat_46, 0);
    lv_obj_set_style_text_color(s_swu_icon, lv_color_make(0x4F, 0xC3, 0xF7), 0);
    lv_obj_set_style_text_align(s_swu_icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_swu_icon, SETTINGS_SAFE_W);
    lv_obj_set_pos(s_swu_icon, 0, 10);
    lv_label_set_text(s_swu_icon, LV_SYMBOL_LOOP);
    lv_obj_clear_flag(s_swu_icon, LV_OBJ_FLAG_CLICKABLE);

    /* Başlık */
    s_swu_title = lv_label_create(s_swupdate_content);
    lv_obj_set_style_text_color(s_swu_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_swu_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_swu_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_swu_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_swu_title, SETTINGS_SAFE_W);
    lv_obj_set_pos(s_swu_title, 0, 78);
    lv_label_set_text(s_swu_title, "Checking for updates");
    lv_obj_clear_flag(s_swu_title, LV_OBJ_FLAG_CLICKABLE);

    /* Alt başlık (mevcut sürüm / boyut / hata mesajı vb.) */
    s_swu_subtitle = lv_label_create(s_swupdate_content);
    lv_obj_set_style_text_color(s_swu_subtitle, lv_color_make(0x8E, 0x8E, 0x93), 0);
    lv_obj_set_style_text_font(s_swu_subtitle, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_align(s_swu_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_swu_subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_swu_subtitle, SETTINGS_SAFE_W);
    lv_obj_set_pos(s_swu_subtitle, 0, 124);
    lv_label_set_text(s_swu_subtitle, "Please wait");
    lv_obj_clear_flag(s_swu_subtitle, LV_OBJ_FLAG_CLICKABLE);

    /* Release notes (UPDATE_AVAILABLE state'inde gösterilir).
     * Sabit yükseklik (60px ≈ 3 satır font_16) — uzun notlar dots ile
     * kesilir, taşıp butonla çakışmaz. */
    s_swu_notes = lv_label_create(s_swupdate_content);
    lv_obj_set_style_text_color(s_swu_notes, lv_color_make(0xCC, 0xCC, 0xCC), 0);
    lv_obj_set_style_text_font(s_swu_notes, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_swu_notes, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_swu_notes, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_swu_notes, SETTINGS_SAFE_W, 60);
    lv_obj_set_pos(s_swu_notes, 0, 162);
    lv_label_set_text(s_swu_notes, "");
    lv_obj_add_flag(s_swu_notes, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_swu_notes, LV_OBJ_FLAG_CLICKABLE);

    /* sbox (panel'in scrollable parent'i) — bu view sabit içerikli, scroll
     * gerekmiyor. SCROLLABLE açık kalırsa LVGL bazen tap'i scroll-start
     * sanıp click'i yutuyor. Progress bar/label ve buton da sbox'a
     * ekleniyor (panel'e değil) — panel LV_SIZE_CONTENT ile sadece görünür
     * çocuklara göre boyutlanıyor, hidden çocuklar boyut hesabına girmiyor
     * → unhide olduğunda panel'in görsel kutusu dışında kalıp render
     * edilmiyor (kullanıcı raporu 2026-05-10 / 2026-05-11). sbox =
     * full-screen 502px sabit; align BOTTOM_MID ile alt-merkezde. */
    lv_obj_t* sbox = lv_obj_get_parent(s_swupdate_content);
    if (sbox) lv_obj_clear_flag(sbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* prog_parent = sbox ? sbox : s_swupdate_content;

    /* Progress bar + label — sbox parent. Pozisyon: butonun ÜZERİNDE,
     * çakışmayacak şekilde ekran ortasında. Önceki layout'ta progress
     * (BOTTOM_MID -75 → y=419-427) buton bandı (BOTTOM_MID -40 → y=402-462)
     * içinde kalıyordu → DOWNLOADING→INSTALLED geçişinde AMOLED chunked
     * flush "sticker peel" artifact'ine yol açıyordu (kullanıcı raporu
     * 2026-05-11). Şimdi: subtitle (panel-y=124 → screen y~224) ile buton
     * (screen y=402) arasındaki boş alanda merkezde. */
    s_swu_progress = lv_bar_create(prog_parent);
    lv_obj_set_size(s_swu_progress, 280, 8);
    lv_obj_align(s_swu_progress, LV_ALIGN_BOTTOM_MID, 0, -171);  /* y=323-331 */
    lv_bar_set_range(s_swu_progress, 0, 100);
    lv_bar_set_value(s_swu_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_swu_progress, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_swu_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_swu_progress, lv_color_make(0xFF, 0x9E, 0x3A), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_swu_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_swu_progress, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_swu_progress, 4, LV_PART_INDICATOR);
    lv_obj_add_flag(s_swu_progress, LV_OBJ_FLAG_HIDDEN);

    s_swu_progress_lbl = lv_label_create(prog_parent);
    lv_obj_set_style_text_color(s_swu_progress_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_swu_progress_lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_align(s_swu_progress_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_swu_progress_lbl, SETTINGS_SAFE_W);
    lv_obj_align(s_swu_progress_lbl, LV_ALIGN_BOTTOM_MID, 0, -185);  /* lbl bar'ın üstünde */
    lv_label_set_text(s_swu_progress_lbl, "%0");
    lv_obj_add_flag(s_swu_progress_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_swu_progress_lbl, LV_OBJ_FLAG_CLICKABLE);

    /* Aksiyon butonu — lv_button_create yerine lv_obj_create + remove_style_all
     * (alarm_app.cpp pattern): LVGL theme button'a gizli stiller sızdırıyor.
     * Parent: sbox (panel değil) — yukarıda açıklanan auto-size sorunu için. */
    s_swu_button = lv_obj_create(sbox ? sbox : s_swupdate_content);
    lv_obj_remove_style_all(s_swu_button);
#ifdef BOARD_VARIANT_LCD128
    /* Session 38: 320x60 buton 240 ekranda iki yandan taşıyordu (x=-40). */
    lv_obj_set_size(s_swu_button, 168, 42);
    lv_obj_align(s_swu_button, LV_ALIGN_BOTTOM_MID, 0, -18);
#else
    lv_obj_set_size(s_swu_button, 320, 60);
    lv_obj_align(s_swu_button, LV_ALIGN_BOTTOM_MID, 0, -40);
#endif
    lv_obj_set_style_bg_color(s_swu_button, lv_color_make(0xFF, 0x9E, 0x3A), 0);
    lv_obj_set_style_bg_opa(s_swu_button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_swu_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_swu_button, 0, 0);
    lv_obj_set_style_shadow_width(s_swu_button, 0, 0);
    lv_obj_set_style_pad_all(s_swu_button, 0, 0);
    lv_obj_add_flag(s_swu_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_swu_button, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(s_swu_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_swu_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_swu_button, swupdate_button_clicked, LV_EVENT_CLICKED, NULL);
    /* NOT: wear_press_feedback_attach KALDIRILDI. Eklediği opacity dim/
     * restore animasyonu AMOLED chunked partial-flush ile butonu üst-alt
     * iki yarı olarak flush ettiriyor → "ikiye bölünme" artifact (kullanıcı
     * raporu 2026-05-11). City kartlarında / eski PRESSED-bg-flip versiyonunda
     * da görülmüştü. Tap teyidi: state DOWNLOADING'a geçince ekran tamamen
     * değişir (Indiriliyor + progress bar) — o yeterli görsel feedback. */

    s_swu_button_lbl = lv_label_create(s_swu_button);
    lv_obj_set_style_text_color(s_swu_button_lbl, lv_color_black(), 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_style_text_font(s_swu_button_lbl, &lv_font_montserrat_14, 0);
#else
    lv_obj_set_style_text_font(s_swu_button_lbl, &lv_font_montserrat_22, 0);
#endif
    lv_label_set_text(s_swu_button_lbl, "Download and Install");
    lv_obj_center(s_swu_button_lbl);
    /* Label EVENT_BUBBLE — alarm_app.cpp:483 pattern. Label tıklanınca
     * event parent button'a yükselsin, CLICKED handler tetiklensin.
     * Bu olmadan label "Download and Install" yazısının üzerine basınca click
     * yutuluyor; sadece kenarlardan tıklayınca çalışır. */
    lv_obj_add_flag(s_swu_button_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Polling timer'ı view oluştuğu anda başlat — go_swupdate_cb hide_all'dan
     * sonra view'i göster, bu sırada timer 1 sn'de bir state polling yapar. */
    s_swu_last_state = OTA_STATE_IDLE;
    swupdate_start_polling();
}

// ALT EKRAN: Network → WiFi toggle + ağ listesi (Apple Watch tarzı)
static void build_network(lv_obj_t* screen)
{
    s_view_network = make_view(screen, &s_network_content);
    make_header(s_view_network, "WiFi", go_main_cb);
    // make_sep(s_network_content, 0); 

    /* ── WiFi toggle satırı ────────────────────────────────────────────── */
    lv_obj_t* wifi_row = make_row(s_network_content, 8, "WiFi");
    lv_obj_add_flag(wifi_row, LV_OBJ_FLAG_CLICKABLE);

    /* Toggle "kayma" artefaktının ASIL sebebi: wifi_row (parent) CLICKABLE
     * ve lv_obj_create default tema + state-bazlı stiller uyguluyor. Press
     * anında tema, row'a LV_STATE_PRESSED için ufak bir transform_scale
     * (shrink) ve bg flash ekliyor. Row büzüldüğünde ÇOCUK switch de bir
     * frame için içe kayıyor → kullanıcı "toggle kaydı, sonra düzeldi"
     * görüyor. Diğer nav satırlarında (General, WiFi menü kartı, Display…)
     * press feedback İSTENİYOR — bu yüzden make_row'u global olarak
     * değiştirmiyoruz; sadece bu özel satırda press-state stillerini
     * default'a pinliyoruz: bg default=pressed, transform_scale=256 her iki
     * state'te de. Böylece row basıldığında hiç transform/flash yok, switch
     * stabil kalıyor. Fonksiyonel davranış aynı — tıklama hâlâ
     * wifi_row_cb'ı tetikliyor; sadece görsel feedback kapandı. */
    lv_obj_set_style_bg_color(wifi_row, lv_color_make(0x1C, 0x1C, 0x1E),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(wifi_row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(wifi_row, 256, 0);
    lv_obj_set_style_transform_scale(wifi_row, 256, LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(wifi_row, 0, 0);
    lv_obj_set_style_transform_width(wifi_row, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(wifi_row, 0, 0);
    lv_obj_set_style_transform_height(wifi_row, 0, LV_STATE_PRESSED);

    s_wifi_switch = make_toggle(wifi_row, s_wifi_enabled);
    lv_obj_align(s_wifi_switch, LV_ALIGN_RIGHT_MID, -14, 0);
    /* Artık lv_switch VALUE_CHANGED yok — tıklama ROW'dan alınıyor,
     * wifi_row_cb state'i çeviriyor, toggle visual'ını güncelliyor ve
     * ağır DOM işini async'e atıyor. */
    lv_obj_add_event_cb(wifi_row, wifi_row_cb, LV_EVENT_CLICKED, NULL);

    /* ── Bağlı ağ satırı (iOS Watch tarzı) ─────────────────────────────────
     * WiFi toggle ile "NETWORKS" listesi arasında yer alır. Bağlı ağ burada
     * gösterilir ve ana listeye dâhil edilmez. Başlangıçta gizli; bağlantı
     * kurulunca wifi_list_update() doldurur ve gösterir. */
    s_connected_row = lv_obj_create(s_network_content);
    lv_obj_remove_style_all(s_connected_row);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_size(s_connected_row, SETTINGS_SAFE_W, 48);
#else
    lv_obj_set_size(s_connected_row, SETTINGS_SAFE_W, 68);
#endif
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_pos(s_connected_row, 0, 72);
#else
    lv_obj_set_pos(s_connected_row, 0, 90);
#endif
    lv_obj_set_style_bg_color(s_connected_row, lv_color_make(0x1C,0x1C,0x1E), 0);
    lv_obj_set_style_bg_opa(s_connected_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_connected_row, 12, 0);
    lv_obj_set_style_border_width(s_connected_row, 1, 0);
    lv_obj_set_style_border_color(s_connected_row, lv_color_make(0x2C,0x2C,0x2E), 0);
    lv_obj_set_style_pad_all(s_connected_row, 0, 0);
    lv_obj_clear_flag(s_connected_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_connected_row, LV_OBJ_FLAG_HIDDEN);

    /* ── "NETWORKS" bölüm etiketi ──────────────────────────────────────── */
    s_wifi_section_lbl = lv_label_create(s_network_content);
    lv_obj_set_style_text_color(s_wifi_section_lbl,
                                lv_color_make(0x8E, 0x8E, 0x93), 0);
    #ifdef BOARD_VARIANT_LCD128
lv_obj_set_style_text_font(s_wifi_section_lbl,
                               &lv_font_montserrat_12, 0);
#else
lv_obj_set_style_text_font(s_wifi_section_lbl,
                               &lv_font_montserrat_16, 0);
#endif
    lv_obj_set_style_text_letter_space(s_wifi_section_lbl, 2, 0);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_pos(s_wifi_section_lbl, 24, 72);
#else
    lv_obj_set_pos(s_wifi_section_lbl, 20, 110);
#endif
    lv_label_set_text(s_wifi_section_lbl, "NETWORKS");

    /* ── AP listesi container (non-scrollable, LV_SIZE_CONTENT) ────────── */
    s_wifi_list = lv_obj_create(s_network_content);
    lv_obj_remove_style_all(s_wifi_list);
    lv_obj_set_width(s_wifi_list, SETTINGS_SAFE_W);
    lv_obj_set_height(s_wifi_list, LV_SIZE_CONTENT);
#ifdef BOARD_VARIANT_LCD128
    lv_obj_set_pos(s_wifi_list, 0, 92);
#else
    lv_obj_set_pos(s_wifi_list, 0, 132);
#endif
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_wifi_list, 0, 0);
    lv_obj_set_style_border_width(s_wifi_list, 0, 0);
    lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_SCROLLABLE);

    /* Başlangıçta WiFi kapalıysa liste + etiket gizli */
    if (!s_wifi_enabled) {
        lv_obj_add_flag(s_wifi_section_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* WiFi açık → hemen tarama başlat */
        lv_obj_t* scanning = lv_label_create(s_wifi_list);
        lv_obj_set_style_text_color(scanning, lv_color_make(0x8E, 0x8E, 0x93), 0);
        lv_obj_set_style_text_font(scanning, &lv_font_montserrat_20, 0);
        lv_obj_set_pos(scanning, 20, 16);
        lv_label_set_text(scanning, "Scanning...");
        wifi_service_scan_async(wifi_scan_done_cb, NULL);
    }
}

// ALT EKRAN: Display & Brightness → iOS-tarzı büyük parlaklık pill'i
static void build_display(lv_obj_t* screen)
{
    s_view_display = make_view(screen, &s_display_content);
    make_header(s_view_display, "Brightness", go_main_cb);
    make_sep(s_display_content, 0);

    // "BRIGHTNESS" — gri küçük etiket (Apple bölüm başlığı tarzı)
    lv_obj_t* sect = lv_label_create(s_display_content);
    lv_obj_set_style_text_color(sect, lv_color_make(0x8E, 0x8E, 0x93), 0);
    lv_obj_set_style_text_font(sect,  &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(sect, 2, 0);
    lv_obj_align(sect, LV_ALIGN_TOP_LEFT, 20, 30);
    lv_label_set_text(sect, "BRIGHTNESS");

    /* iOS Control Center tarzı büyük parlaklık pill'i.
     *
     * Layout:
     *   - Dış pill (s_br_bar): 349×64 — bütün yatay alan, büyük touch hedefi
     *   - İç fill (s_br_fill): pill'in içinde solden sağa uzar, beyaz
     *   - Sol ucunda küçük güneş (16px) — fill üzerinde, koyu renkli
     *   - Sağ ucunda büyük güneş (24px) — fill dışında kaldığı sürece açık
     *     gri; fill üstüne geldiğinde koyu griye yaklaşıyor (Apple'da sabit
     *     beyaz, bizde görünürlük için mid-gray seçtim).
     *
     * Pill'in her yerine tıklanabilir — wifi_row gibi state-specific
     * press feedback YOK (transform ve bg_color PRESSED state'te de
     * default'a pinlenmiş). Böylece press flash veya kayma oluşmuyor;
     * kullanıcı dokunduğu andan itibaren SADECE fill hareket ediyor. */
    const int32_t BAR_Y = 62;
    const int32_t BAR_H = 64;
    const int32_t BAR_X = 20;
    const int32_t BAR_W = SETTINGS_SAFE_W - 2 * BAR_X;  /* 389-40 = 349 */

    // Track (dış pill, hit-test burada)
    s_br_bar = lv_obj_create(s_display_content);
    lv_obj_remove_style_all(s_br_bar);
    lv_obj_set_size(s_br_bar, BAR_W, BAR_H);
    lv_obj_set_pos(s_br_bar, BAR_X, BAR_Y);
    lv_obj_set_style_bg_color(s_br_bar, lv_color_make(0x2C, 0x2C, 0x2E), 0);
    lv_obj_set_style_bg_opa(s_br_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_br_bar, BAR_H / 2, 0);  /* tam pill */
    lv_obj_set_style_border_width(s_br_bar, 0, 0);
    lv_obj_set_style_outline_width(s_br_bar, 0, 0);
    lv_obj_set_style_shadow_width(s_br_bar, 0, 0);
    lv_obj_set_style_pad_all(s_br_bar, 0, 0);
    /* PRESSED state'i default'a pinle — flash/transform yok */
    lv_obj_set_style_bg_color(s_br_bar, lv_color_make(0x2C, 0x2C, 0x2E),
                              LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(s_br_bar, 256, 0);
    lv_obj_set_style_transform_scale(s_br_bar, 256, LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(s_br_bar, 0, 0);
    lv_obj_set_style_transform_width(s_br_bar, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(s_br_bar, 0, 0);
    lv_obj_set_style_transform_height(s_br_bar, 0, LV_STATE_PRESSED);
    lv_obj_clear_flag(s_br_bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                                 LV_OBJ_FLAG_CLICK_FOCUSABLE |
                                                 LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    lv_obj_add_flag(s_br_bar, LV_OBJ_FLAG_CLICKABLE);

    s_br_bar_w = BAR_W;

    // Fill (iç pill — beyaz, solden genişler)
    s_br_fill = lv_obj_create(s_br_bar);
    lv_obj_remove_style_all(s_br_fill);
    lv_obj_set_size(s_br_fill, BAR_H, BAR_H);  /* başlangıç; hemen update */
    lv_obj_set_pos(s_br_fill, 0, 0);
    lv_obj_set_style_bg_color(s_br_fill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_br_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_br_fill, BAR_H / 2, 0);
    lv_obj_set_style_border_width(s_br_fill, 0, 0);
    lv_obj_set_style_outline_width(s_br_fill, 0, 0);
    lv_obj_set_style_shadow_width(s_br_fill, 0, 0);
    lv_obj_set_style_pad_all(s_br_fill, 0, 0);
    lv_obj_set_style_transform_scale(s_br_fill, 256, 0);
    lv_obj_clear_flag(s_br_fill, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                                  LV_OBJ_FLAG_CLICKABLE |
                                                  LV_OBJ_FLAG_CLICK_FOCUSABLE |
                                                  LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    lv_obj_add_flag(s_br_fill, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Küçük güneş (sol) — fill üstünde her zaman görünür, koyu renk
    lv_obj_t* sun_small = lv_image_create(s_br_bar);
    lv_image_set_src(sun_small, &sun_icon_16);
    lv_image_set_scale(sun_small, 384);  /* 1.5× — daha belirgin */
    lv_obj_align(sun_small, LV_ALIGN_LEFT_MID, 22, 0);
    lv_obj_set_style_image_recolor(sun_small, lv_color_make(0x48, 0x48, 0x4A), 0);
    lv_obj_set_style_image_recolor_opa(sun_small, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sun_small, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                                  LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(sun_small, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Büyük güneş (sağ) — track üstünde çoğu zaman, açık gri
    lv_obj_t* sun_big = lv_image_create(s_br_bar);
    lv_image_set_src(sun_big, &sun_icon_24);
    lv_image_set_scale(sun_big, 384);
    lv_obj_align(sun_big, LV_ALIGN_RIGHT_MID, -22, 0);
    lv_obj_set_style_image_recolor(sun_big, lv_color_make(0x8E, 0x8E, 0x93), 0);
    lv_obj_set_style_image_recolor_opa(sun_big, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sun_big, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE |
                                                LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(sun_big, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Event handler — PRESSED/PRESSING/RELEASED hepsini aynı cb alıyor
    lv_obj_add_event_cb(s_br_bar, br_bar_event_cb, LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(s_br_bar, br_bar_event_cb, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(s_br_bar, br_bar_event_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(s_br_bar, br_bar_event_cb, LV_EVENT_PRESS_LOST, NULL);

    /* Session 34: başlangıç değeri ortak servisten — Control Center'dan
     * değiştirilmiş olabilir. */
    s_brightness = power_service_get_brightness();
    if (s_brightness < BR_MIN) s_brightness = BR_MIN;
    brightness_update_visual(s_brightness);

    set_content_height(s_display_content, BAR_Y + BAR_H + 20);
}

// ALT EKRAN: Uçak Modu — master toggle + Wi-Fi/Bluetooth davranış toggle'ları
static void build_airplane(lv_obj_t* screen)
{
    s_view_airplane = make_view(screen, &s_airplane_content);
    make_header(s_view_airplane, "Airplane Mode", go_main_cb);

    /* ── Master toggle satırı (Ucak Modu) ──────────────────────────────── */
    lv_obj_t* master_row = make_row(s_airplane_content, 8, "Airplane Mode");
    lv_obj_add_flag(master_row, LV_OBJ_FLAG_CLICKABLE);
    /* press feedback'ten kaynaklı toggle kayma artefaktını sıfırla */
    lv_obj_set_style_bg_color(master_row, lv_color_make(0x1C, 0x1C, 0x1E),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(master_row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(master_row, 256, 0);
    lv_obj_set_style_transform_scale(master_row, 256, LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(master_row, 0, 0);
    lv_obj_set_style_transform_width(master_row, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(master_row, 0, 0);
    lv_obj_set_style_transform_height(master_row, 0, LV_STATE_PRESSED);

    s_apm_master_switch = make_toggle_ex(master_row, s_airplane_enabled,
                                         &s_apm_master_knob);
    lv_obj_align(s_apm_master_switch, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_add_event_cb(master_row, apm_master_row_cb, LV_EVENT_CLICKED, NULL);

    /* ── "UÇAK MODU DAVRANIŞI" bölüm başlığı ──────────────────────────── */
    lv_obj_t* sect = lv_label_create(s_airplane_content);
    lv_obj_set_style_text_color(sect, lv_color_make(0x8E, 0x8E, 0x93), 0);
#ifdef BOARD_VARIANT_LCD128
    /* Session 38: font 16 + letter_space ile başlık 255 px sürüyordu (panel
     * 204). Küçük font + kısa metin — anlam aynı, satır sığıyor. */
    lv_obj_set_style_text_font(sect,  &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(sect, 1, 0);
    lv_obj_set_pos(sect, 14, 8 + 90 + 16);
    lv_label_set_text(sect, "AIRPLANE MODE");
#else
    lv_obj_set_style_text_font(sect,  &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_letter_space(sect, 1, 0);
    lv_obj_set_pos(sect, 18, 8 + 90 + 16);
    lv_label_set_text(sect, "AIRPLANE MODE BEHAVIOR");
#endif

    /* ── Wi-Fi davranış satırı ─────────────────────────────────────────── */
    int32_t y_wifi = 8 + 90 + 16 + 28;
    lv_obj_t* wifi_row = make_row(s_airplane_content, y_wifi, "Wi-Fi");
    lv_obj_add_flag(wifi_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(wifi_row, lv_color_make(0x1C, 0x1C, 0x1E),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(wifi_row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(wifi_row, 256, 0);
    lv_obj_set_style_transform_scale(wifi_row, 256, LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(wifi_row, 0, 0);
    lv_obj_set_style_transform_width(wifi_row, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(wifi_row, 0, 0);
    lv_obj_set_style_transform_height(wifi_row, 0, LV_STATE_PRESSED);
    s_apm_wifi_switch = make_toggle_ex(wifi_row, s_airplane_pref_wifi,
                                       &s_apm_wifi_knob);
    lv_obj_align(s_apm_wifi_switch, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_add_event_cb(wifi_row, apm_wifi_row_cb, LV_EVENT_CLICKED, NULL);

    /* ── Bluetooth davranış satırı ─────────────────────────────────────── */
    int32_t y_bt = y_wifi + 98;
    lv_obj_t* bt_row = make_row(s_airplane_content, y_bt, "Bluetooth");
    lv_obj_add_flag(bt_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(bt_row, lv_color_make(0x1C, 0x1C, 0x1E),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(bt_row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(bt_row, 256, 0);
    lv_obj_set_style_transform_scale(bt_row, 256, LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(bt_row, 0, 0);
    lv_obj_set_style_transform_width(bt_row, 0, LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(bt_row, 0, 0);
    lv_obj_set_style_transform_height(bt_row, 0, LV_STATE_PRESSED);
    s_apm_bt_switch = make_toggle_ex(bt_row, s_airplane_pref_bt,
                                     &s_apm_bt_knob);
    lv_obj_align(s_apm_bt_switch, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_add_event_cb(bt_row, apm_bt_row_cb, LV_EVENT_CLICKED, NULL);

    set_content_height(s_airplane_content, y_bt + 90 + 20);
}

// ═══════════════════════════════════════════════════════════════════════════
// v1.1.1 (2026-05-14) — Settings → Güç Rayları (Power Rails)
// ═══════════════════════════════════════════════════════════════════════════
//
// Kullanıcının AXP2101 PMIC rail'lerini runtime'da elle kapatıp açabildiği
// alt sayfa. Deep sleep akışında otomatik kapatılan rail'lere (ALDO1 audio,
// DCDC2 display IO, ALDO2 display panel) manuel kontrol verir + saati
// tamamen kapatan "Soft Power Off" eylemini onay dialog'u arkasında sunar.
//
// DCDC1 (system 3.3V) LİSTELENMEZ — kapatılması ESP32'yi öldürür ve bu
// zaten Soft Power Off'un yaptığı şey. İki ayrı kontrol gereksiz/kafa
// karıştırıcı.
//
// Display rail (DCDC2, ALDO2) toggle OFF olduğunda ekran kararır:
//   - Touch hardware (FT3168 + VCC3V3) etkilenmez, dokunma hala yakalanır
//   - 5 sn auto-wake timer başlatılır
//   - Aynı zamanda invisible full-screen overlay touch event'i yakalar →
//     erken uyandırma
//   - Wake → ilgili rail'ler ON + toggle UI sync
//
// Soft Power Off butonu:
//   - Apple-tarzı onay dialog'u (yarı şeffaf siyah backdrop + ortada panel)
//   - "Kapat" (kırmızı) tıklanırsa axp2101_soft_power_off() çağrılır (DÖNMEZ)
//   - "Iptal" → dialog kapanır, hiçbir şey değişmez
// ═══════════════════════════════════════════════════════════════════════════

/* v1.4.40: AXP I²C çağrısı blocking (~50 ms). LVGL ana thread'inde
 * yapılırsa toggle render frame'i bloke olur → "kayma/bozukluk" artefaktı.
 * Çözüm: async call ile defer et. Async call payload heap'te tutulur,
 * callback işleyince free eder. */
typedef struct {
    axp2101_rail_t rail;
    bool           enable;
} rail_set_async_arg_t;

static void rail_set_async_cb(void* p)
{
    rail_set_async_arg_t* arg = (rail_set_async_arg_t*)p;
    if (!arg) return;
    esp_err_t ret = axp2101_set_rail(arg->rail, arg->enable);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rail_set_async: axp2101_set_rail(%d, %d) → %d",
                 arg->rail, arg->enable, ret);
    }
    free(arg);
}

static void rail_set_async(axp2101_rail_t rail, bool enable)
{
    rail_set_async_arg_t* arg = (rail_set_async_arg_t*)malloc(sizeof(*arg));
    if (!arg) {
        ESP_LOGE(TAG, "rail_set_async: malloc fail, sync fallback");
        axp2101_set_rail(rail, enable);
        return;
    }
    arg->rail   = rail;
    arg->enable = enable;
    lv_async_call(rail_set_async_cb, arg);
}

/* v1.4.41: AXP yazımını N ms gecikmeli yap. Toggle ON tıklamasından sonra
 * "kullanıcı toggle ON olduğunu görsün, SONRA ekran kararsın" görsel
 * feedback için. 300 ms = ~9 frame @ 30fps, yeterli görsel onay süresi. */
static void rail_set_delayed_timer_cb(lv_timer_t* t)
{
    rail_set_async_arg_t* arg = (rail_set_async_arg_t*)lv_timer_get_user_data(t);
    if (arg) {
        esp_err_t ret = axp2101_set_rail(arg->rail, arg->enable);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "rail_set_delayed: axp2101_set_rail(%d, %d) → %d",
                     arg->rail, arg->enable, ret);
        }
        free(arg);
    }
    /* v1.2.2 (2026-05-18): LVGL 9.2'de repeat_count=1 timer'ı SİLMEZ —
     * sadece enabled=false yapar (paused). Struct heap'te kalır → her
     * rail tıklamasında ~64 byte sızıntı + LVGL timer listesi büyür.
     * Manuel delete ile bu önlenir. */
    lv_timer_delete(t);
}

static void rail_set_delayed(axp2101_rail_t rail, bool enable, uint32_t delay_ms)
{
    rail_set_async_arg_t* arg = (rail_set_async_arg_t*)malloc(sizeof(*arg));
    if (!arg) {
        ESP_LOGE(TAG, "rail_set_delayed: malloc fail, sync fallback");
        axp2101_set_rail(rail, enable);
        return;
    }
    arg->rail   = rail;
    arg->enable = enable;
    lv_timer_t* t = lv_timer_create(rail_set_delayed_timer_cb, delay_ms, arg);
    lv_timer_set_repeat_count(t, 1);
}

/* Display rail kapatıldığında ekran karadığı için "geri uyandırma" mekanizması.
 * v1.4.40 düzeltmeleri:
 *   - Timer del çağrısı KALDIRILDI: callback içinden silmek crash/silent
 *     failure. LVGL repeat_count=1 ile zaten otomatik siliyor; sadece
 *     pointer NULL'a çekilir.
 *   - Overlay del → lv_obj_delete_async: event handler içinden direct del
 *     LVGL undefined behavior. async del bir sonraki cycle'da güvenli.
 *   - AXP çağrıları → rail_set_async: blocking I²C UI'yi bloke etmesin.
 *
 * v1.2.2 / v1.4.53 (2026-05-18): Bug #4 fix — worker task pattern.
 *   Önceden rail_wake_now() içinde vTaskDelay(20+120+80=220 ms) LVGL event
 *   handler context'inde çalışıyordu → LVGL mutex tutulurdu → sensor task
 *   UI yazımları bloke + overlay silinmesi 220 ms gecikirdi.
 *
 *   Yeni akış:
 *     1. LVGL task: snapshot al, visual flag'leri + toggle UI'ları + overlay
 *        async-delete'i HEMEN yap (instant feedback).
 *     2. Worker task spawn: AXP rail ON yazımları + datasheet delay'leri
 *        arka planda (UI task serbest).
 *     3. Worker → lv_async_call ile finalize: invalidate + refr_now (clean
 *        push). Worker self-delete.
 *
 *   Hardware delay'leri (CO5300 SLPOUT 120 ms, rail stabilize 20 ms, DMA
 *   push 80 ms) datasheet zorunluluğu — kısaltılamaz, sadece UI task'tan
 *   çıkartılır. Görsel "tap → ekran açıldı" latency'si aynı kalır; LVGL
 *   mutex 220 ms tutulmadığı için arka plan (sensor) task'ları akıcı kalır. */

/* Hangi rail'ler ON edilecek — worker'a heap üzerinden geçirilen iş emri. */
typedef struct {
    bool wake_aldo1;
    bool wake_dcdc2;
    bool wake_aldo2;
    bool wake_aldo3;
    bool wake_aldo4;
    bool wake_dcdc3;
    bool wake_dcdc4;
    bool wake_bldo1;
    bool wake_bldo2;
} rail_wake_work_t;

/* Re-entry guard: 5 sn timer + overlay touch aynı anda tetiklenebilir.
 * Worker zaten çalışıyorsa ikinci spawn'u atla (visual flag'ler zaten
 * temizlenmiş, ek iş yok). */
static volatile bool s_rail_wake_in_progress = false;

/* LVGL task'ında çalışır — worker hardware ops'u bitirince clean buffer
 * push'unu queue eder. Burada vTaskDelay yok, hızlı geri döner. */
static void rail_wake_finalize_async_cb(void* arg)
{
    (void)arg;
    if (s_view_power) {
        lv_obj_invalidate(s_view_power);
    }
    lv_refr_now(NULL);
    s_rail_wake_in_progress = false;
}

/* Worker task — UI task'tan ayrı, vTaskDelay'leri burada LVGL mutex
 * tutmadan yapar. Tek-shot: hardware ops bitince finalize'i async ile
 * UI'a queue eder, sonra kendini siler. */
static void rail_wake_worker_task(void* arg)
{
    rail_wake_work_t* work = (rail_wake_work_t*)arg;

    if (work->wake_aldo1) {
        axp2101_set_rail(AXP_RAIL_LDO1, true);
    }
    /* Display rail wake sırası (v1.4.48):
     *   DCDC2 (gerçek kesim) → önce rail aç + power stabilize
     *   ALDO2 (simülasyon)   → rail dokunulmamış, sadece CO5300 SLPOUT */
    if (work->wake_dcdc2) {
        axp2101_set_rail(AXP_RAIL_DCDC2, true);
        vTaskDelay(pdMS_TO_TICKS(20));         /* power stabilize */
    }
    if (work->wake_aldo2) {
        co5300_sleep(false);                   /* SLPOUT */
        vTaskDelay(pdMS_TO_TICKS(120));        /* min 120 ms datasheet */
        co5300_set_brightness(255);
    }
    if (work->wake_aldo3) {
        axp2101_set_rail(AXP_RAIL_LDO3, true);
    }
    if (work->wake_aldo4) {
        axp2101_set_rail(AXP_RAIL_LDO4, true);
    }
    /* v1.4.47 yeni rail'ler */
    if (work->wake_dcdc3) {
        axp2101_set_rail(AXP_RAIL_DCDC3, true);
    }
    if (work->wake_dcdc4) {
        axp2101_set_rail(AXP_RAIL_DCDC4, true);
    }
    if (work->wake_bldo1) {
        axp2101_set_rail(AXP_RAIL_BLDO1, true);
    }
    if (work->wake_bldo2) {
        axp2101_set_rail(AXP_RAIL_BLDO2, true);
    }

    /* DMA push'un display IC'ye tamamlanması için kısa bekleme — burada
     * worker task içindeyiz, LVGL mutex tutulmuyor → UI bloke olmaz. */
    vTaskDelay(pdMS_TO_TICKS(80));

    free(work);

    /* Finalize render LVGL task'ında yapılmalı → lv_async_call ile queue. */
    lv_async_call(rail_wake_finalize_async_cb, NULL);

    vTaskDelete(NULL);
}

static void rail_wake_now(void)
{
    /* v1.4.42/44: Sadece ON olan rail'leri aç. Display rail'leri (ALDO2/
     * DCDC2) için ek olarak CO5300 sleep'ten uyandırma sırasıyla yapılır:
     * rail aç → power stabilize → SLPOUT → brightness geri yükle. v1.4.53'
     * te hardware ops worker task'a taşındı. */
    ESP_LOGI(TAG, "rail_wake_now: ON toggle'ları temizleniyor (worker)");

    /* Re-entry guard — worker zaten aktifse ikinci spawn'u atla.
     * Visual flag'ler ilk çağrıda temizlenmişti → ikinci çağrıda yapılacak
     * iş zaten yok. */
    if (s_rail_wake_in_progress) {
        ESP_LOGD(TAG, "rail_wake_now: worker aktif, skip");
        /* Overlay/timer cleanup yine de yapılmalı (timer callback'inden
         * geldiyse). Aşağıdaki blok bunu garanti eder. */
    }

    rail_wake_work_t* work = (rail_wake_work_t*)calloc(1, sizeof(*work));
    /* Snapshot al + visual flag/UI'yi LVGL task'ında HEMEN güncelle.
     * Kullanıcı tıkladığında toggle'ın anında OFF göründüğünü hisseder;
     * hardware ON yazımı arkadan worker'da koşar. */
    if (s_rail_aldo1_visual_on) {
        if (work) work->wake_aldo1 = true;
        s_rail_aldo1_visual_on = false;
        if (s_rail_aldo1_switch && s_rail_aldo1_knob) {
            toggle_set_checked(s_rail_aldo1_switch, s_rail_aldo1_knob, false);
        }
    }
    if (s_rail_dcdc2_visual_on) {
        if (work) work->wake_dcdc2 = true;
        s_rail_dcdc2_visual_on = false;
        if (s_rail_dcdc2_switch && s_rail_dcdc2_knob) {
            toggle_set_checked(s_rail_dcdc2_switch, s_rail_dcdc2_knob, false);
        }
    }
    if (s_rail_aldo2_visual_on) {
        if (work) work->wake_aldo2 = true;
        s_rail_aldo2_visual_on = false;
        if (s_rail_aldo2_switch && s_rail_aldo2_knob) {
            toggle_set_checked(s_rail_aldo2_switch, s_rail_aldo2_knob, false);
        }
    }
    if (s_rail_aldo3_visual_on) {
        if (work) work->wake_aldo3 = true;
        s_rail_aldo3_visual_on = false;
        if (s_rail_aldo3_switch && s_rail_aldo3_knob) {
            toggle_set_checked(s_rail_aldo3_switch, s_rail_aldo3_knob, false);
        }
    }
    if (s_rail_aldo4_visual_on) {
        if (work) work->wake_aldo4 = true;
        s_rail_aldo4_visual_on = false;
        if (s_rail_aldo4_switch && s_rail_aldo4_knob) {
            toggle_set_checked(s_rail_aldo4_switch, s_rail_aldo4_knob, false);
        }
    }
    /* v1.4.47 yeni rail'ler */
    if (s_rail_dcdc3_visual_on) {
        if (work) work->wake_dcdc3 = true;
        s_rail_dcdc3_visual_on = false;
        if (s_rail_dcdc3_switch && s_rail_dcdc3_knob) {
            toggle_set_checked(s_rail_dcdc3_switch, s_rail_dcdc3_knob, false);
        }
    }
    if (s_rail_dcdc4_visual_on) {
        if (work) work->wake_dcdc4 = true;
        s_rail_dcdc4_visual_on = false;
        if (s_rail_dcdc4_switch && s_rail_dcdc4_knob) {
            toggle_set_checked(s_rail_dcdc4_switch, s_rail_dcdc4_knob, false);
        }
    }
    if (s_rail_bldo1_visual_on) {
        if (work) work->wake_bldo1 = true;
        s_rail_bldo1_visual_on = false;
        if (s_rail_bldo1_switch && s_rail_bldo1_knob) {
            toggle_set_checked(s_rail_bldo1_switch, s_rail_bldo1_knob, false);
        }
    }
    if (s_rail_bldo2_visual_on) {
        if (work) work->wake_bldo2 = true;
        s_rail_bldo2_visual_on = false;
        if (s_rail_bldo2_switch && s_rail_bldo2_knob) {
            toggle_set_checked(s_rail_bldo2_switch, s_rail_bldo2_knob, false);
        }
    }

    /* v1.4.40: timer ZATEN LVGL tarafından siliniyor (repeat_count=1
     * auto-delete) eğer buraya timer callback'ten geldiysek. Manuel del =
     * double-free. Sadece pointer'ı NULL'a çek. */
    s_rail_wake_timer = NULL;

    /* Overlay del → async. Event handler içinden direct del LVGL UB.
     * lv_obj_delete_async bir sonraki LVGL cycle'da güvenli silme yapar. */
    if (s_rail_wake_overlay) {
        lv_obj_delete_async(s_rail_wake_overlay);
        s_rail_wake_overlay = NULL;
    }

    /* Halihazırda worker aktifse veya malloc başarısızsa spawn etme. */
    if (!work) {
        ESP_LOGE(TAG, "rail_wake_now: calloc fail, sync fallback render");
        /* Worker yok → en azından invalidate + refr yap, ekran content
         * geri gelsin. Hardware ops'u kaçırdık ama visual flag'ler false
         * olduğu için kullanıcı OFF durumunu görür. */
        if (s_view_power) lv_obj_invalidate(s_view_power);
        lv_refr_now(NULL);
        return;
    }
    if (s_rail_wake_in_progress) {
        /* Önceki worker hala koşuyor; visual zaten temizdi, yeni iş yok. */
        free(work);
        return;
    }

    s_rail_wake_in_progress = true;
    BaseType_t ok = xTaskCreate(rail_wake_worker_task,
                                "rail_wake",
                                4096,                    /* stack */
                                work,
                                SENSOR_TASK_PRIORITY,    /* UI prio'dan düşük */
                                NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "rail_wake_now: worker spawn fail, sync fallback");
        /* Worker spawn olmadı → eski yöntemle UI'da sync yap.
         * Beklenmedik durum; bu noktada UI task'ında olduğumuz için
         * vTaskDelay'lere maruz kalacak ama korelasyon hata case'i. */
        if (work->wake_aldo1) axp2101_set_rail(AXP_RAIL_LDO1, true);
        if (work->wake_dcdc2) { axp2101_set_rail(AXP_RAIL_DCDC2, true); vTaskDelay(pdMS_TO_TICKS(20)); }
        if (work->wake_aldo2) { co5300_sleep(false); vTaskDelay(pdMS_TO_TICKS(120)); co5300_set_brightness(255); }
        if (work->wake_aldo3) axp2101_set_rail(AXP_RAIL_LDO3, true);
        if (work->wake_aldo4) axp2101_set_rail(AXP_RAIL_LDO4, true);
        if (work->wake_dcdc3) axp2101_set_rail(AXP_RAIL_DCDC3, true);
        if (work->wake_dcdc4) axp2101_set_rail(AXP_RAIL_DCDC4, true);
        if (work->wake_bldo1) axp2101_set_rail(AXP_RAIL_BLDO1, true);
        if (work->wake_bldo2) axp2101_set_rail(AXP_RAIL_BLDO2, true);
        if (s_view_power) lv_obj_invalidate(s_view_power);
        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(80));
        free(work);
        s_rail_wake_in_progress = false;
    }
}

static void rail_wake_timer_cb(lv_timer_t* t)
{
    (void)t;
    rail_wake_now();
}

static void rail_wake_overlay_cb(lv_event_t* e)
{
    (void)e;
    /* Touch yakalanır yakalanmaz uyandır. Kullanıcı 5 sn beklemek zorunda
     * kalmaz — ekran kararıktan hemen sonra dokunarak geri açabilir. */
    rail_wake_now();
}

/* Rail kapatıldığında auto-wake mekanizmasını kur:
 *   - 5sn timer (her durumda)
 *   - Touch overlay (sadece with_touch_overlay=true ise; display rail'leri
 *     için anlamlı çünkü ekran kararık + touch IC ALDO1 hala besleniyor)
 *
 * v1.4.39 düzeltme:
 *   - Önceki overlay screen'in normal child'ı + LV_OPA_TRANSP idi; LVGL
 *     hit-test bazı durumlarda transparent objeleri atlıyordu.
 *   - Yeni: lv_layer_top()'a koy (kalıcı foreground) + opak siyah bg.
 *     Display rail kapatıldığında ekran zaten kararık olduğu için opak
 *     siyah overlay görsel olarak fark yaratmaz, ama LVGL kesin hit-test
 *     yapabilir → touch event'i yakalanır.
 *
 * ALDO1 (touch IC besleyici) kapatıldığında overlay yok çünkü:
 *   - Touch IC besleme yok → hardware touch event üretemez
 *   - LVGL'e event hiç gelmez → overlay tetiklenmez
 *   - Görünür siyah overlay UI'yi karartır (ALDO2 açık olduğu için ekran
 *     yanıyor) → kötü deneyim
 *   - Sadece 5sn timer ile recovery. */
static void rail_schedule_wake(bool with_touch_overlay)
{
    /* Pending varsa temizle. rail_row_clicked_cb ana thread'den çağırıyor —
     * event handler context'inde değiliz, direct delete güvenli + immediate
     * (async-delete race condition'ı önle: pending delete + yeni overlay
     * iki overlay yaratıyordu, eski tıklamayı yutuyor handler'ı yoktu). */
    if (s_rail_wake_timer) {
        lv_timer_delete(s_rail_wake_timer);
        s_rail_wake_timer = NULL;
    }
    if (s_rail_wake_overlay) {
        lv_obj_delete(s_rail_wake_overlay);
        s_rail_wake_overlay = NULL;
    }

    /* 5sn timer her zaman kurulur */
    s_rail_wake_timer = lv_timer_create(rail_wake_timer_cb, 5000, NULL);
    lv_timer_set_repeat_count(s_rail_wake_timer, 1);
    ESP_LOGI(TAG, "rail_wake: 5sn timer kuruldu (overlay=%d)", with_touch_overlay);

    if (!with_touch_overlay) return;

    /* v1.4.41: Power view'ın kendisine ekle (layer_top yerine). Layer_top'ta
     * bazı LVGL 9 build'lerinde touch event propagation eksikti. Power view
     * zaten power sayfası açıkken foreground'da, child overlay onun üstüne
     * binebilir. lv_obj_move_foreground ile son child olarak garantile. */
    s_rail_wake_overlay = lv_obj_create(s_view_power);
    lv_obj_remove_style_all(s_rail_wake_overlay);
    lv_obj_set_size(s_rail_wake_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_rail_wake_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_rail_wake_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_rail_wake_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_rail_wake_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_rail_wake_overlay, 0, 0);
    lv_obj_add_flag(s_rail_wake_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_rail_wake_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_rail_wake_overlay);
    /* LV_EVENT_CLICKED daha güvenli (press+release sonrası).
     * PRESSED bazen LVGL'in scroll/swipe gesture algoritmasına takılıyor. */
    lv_obj_add_event_cb(s_rail_wake_overlay, rail_wake_overlay_cb,
                        LV_EVENT_CLICKED, NULL);
}

/* Rail toggle row tıklama callback'i. user_data olarak axp2101_rail_t
 * değerini taşır (uintptr_t cast).
 *
 * v1.4.38 INVERSE SEMANTIC:
 *   - Toggle visual ON  = rail KAPATILDI (kapatma aksiyonu uygulandı)
 *   - Toggle visual OFF = rail AÇIK  (default state, kapatma yok)
 *
 * Yani toggle "rail kapat" aksiyon switch'i. Default kapalı (OFF) görüntüsü
 * kullanıcının "isterse kapatsın" mental modelini karşılar. Tıklama:
 *   visual OFF → visual ON  → axp2101_set_rail(rail, false)   ← KAPAT
 *   visual ON  → visual OFF → axp2101_set_rail(rail, true)    ← AÇ
 *
 * Display rail (DCDC2/ALDO2) visual ON yapıldıysa ekran kararır → 5sn timer
 * + touch overlay kurulur. Visual OFF yapılırsa wake mekanizması iptal. */
static void rail_row_clicked_cb(lv_event_t* e)
{
    axp2101_rail_t rail = (axp2101_rail_t)(uintptr_t)lv_event_get_user_data(e);

    /* Rail'e karşılık gelen visual flag + toggle widget pointer'larını bul.
     * v1.4.47: 9 rail (4 ALDO + 3 DCDC + 2 BLDO). */
    bool*       visual_flag = NULL;
    lv_obj_t*   sw          = NULL;
    lv_obj_t*   knob        = NULL;
    switch (rail) {
        case AXP_RAIL_LDO1:
            visual_flag = &s_rail_aldo1_visual_on;
            sw = s_rail_aldo1_switch; knob = s_rail_aldo1_knob; break;
        case AXP_RAIL_LDO2:
            visual_flag = &s_rail_aldo2_visual_on;
            sw = s_rail_aldo2_switch; knob = s_rail_aldo2_knob; break;
        case AXP_RAIL_LDO3:
            visual_flag = &s_rail_aldo3_visual_on;
            sw = s_rail_aldo3_switch; knob = s_rail_aldo3_knob; break;
        case AXP_RAIL_LDO4:
            visual_flag = &s_rail_aldo4_visual_on;
            sw = s_rail_aldo4_switch; knob = s_rail_aldo4_knob; break;
        case AXP_RAIL_DCDC2:
            visual_flag = &s_rail_dcdc2_visual_on;
            sw = s_rail_dcdc2_switch; knob = s_rail_dcdc2_knob; break;
        case AXP_RAIL_DCDC3:
            visual_flag = &s_rail_dcdc3_visual_on;
            sw = s_rail_dcdc3_switch; knob = s_rail_dcdc3_knob; break;
        case AXP_RAIL_DCDC4:
            visual_flag = &s_rail_dcdc4_visual_on;
            sw = s_rail_dcdc4_switch; knob = s_rail_dcdc4_knob; break;
        case AXP_RAIL_BLDO1:
            visual_flag = &s_rail_bldo1_visual_on;
            sw = s_rail_bldo1_switch; knob = s_rail_bldo1_knob; break;
        case AXP_RAIL_BLDO2:
            visual_flag = &s_rail_bldo2_visual_on;
            sw = s_rail_bldo2_switch; knob = s_rail_bldo2_knob; break;
        default:
            ESP_LOGW(TAG, "rail_row_clicked: unknown rail %d", rail);
            return;
    }
    if (!visual_flag || !sw || !knob) return;

    bool new_visual_on   = !*visual_flag;
    bool new_rail_enable = !new_visual_on;   /* inverse: visual ON = rail OFF */

    /* v1.4.42 — yeni sıralama (önceki: delay/async timing race):
     *   1) Visual state güncelle
     *   2) sw + knob + row invalidate (yarım render artefakt önle)
     *   3) lv_refr_now() ile LVGL'i ZORLA render et → display IC'ye push.
     *      Toggle yeşil görüntüsü display panel'inde GARANTİLİ görünür.
     *   4) AXP yazımı SYNC (blocking 50ms, ama display zaten güncel).
     *   5) Schedule wake (kapatma durumunda)
     *
     * Önceki rail_set_delayed(300ms) yaklaşımı: timer fire'ı 700ms'ye
     * uzayabiliyordu (LVGL timer queue + I²C blocking), bu sürede de
     * display IC'ye toggle değişimi push edilmemiş olabiliyordu →
     * kullanıcı toggle yeşilini hiç göremiyordu. lv_refr_now() ile bu
     * tamamen deterministik. */
    *visual_flag = new_visual_on;
    toggle_set_checked(sw, knob, new_visual_on);
    lv_obj_invalidate(sw);
    lv_obj_invalidate(knob);
    lv_obj_t* row = lv_obj_get_parent(sw);
    if (row) lv_obj_invalidate(row);

    /* Force LVGL render frame.
     * v1.4.43 KRİTİK FİX — Display SPI DMA-based async (co5300_driver.cpp
     * DMA completion ISR'da lv_display_flush_ready çağırıyor). lv_refr_now()
     * render bitince döner ama frame henüz DMA queue'da, display IC'de
     * değil. vTaskDelay ile DMA push'a zaman ver. */
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(80));

    /* v1.4.48 — Test sonuçlarına göre rail başına farklı strateji:
     *
     *   ALDO2 → SİMÜLASYON: kesilince display IC içsel reset'e giriyor
     *           (v1.4.45 testi), full re-init gerekiyor → orantısız iş,
     *           pragmatik olarak CO5300 sleep + brightness 0 yapıyoruz.
     *           Akım ölçerde fark yok, görsel deneyim aynı.
     *
     *   DCDC2 → GERÇEK KESİM güvenli (v1.4.47 testi): kesim ekranı
     *           karartıyor ama LVGL hang olmuyor, touch overlay düzgün
     *           çalışıyor. Wake mekanizması güvenilir, rail dokunulabilir.
     *
     *   Diğer 7 rail (ALDO1/3/4, DCDC3/4, BLDO1/2) → GERÇEK KESİM. */
    if (new_visual_on && rail == AXP_RAIL_LDO2) {
        /* ALDO2 sadece simülasyon */
        co5300_set_brightness(0);
        co5300_sleep(true);
        vTaskDelay(pdMS_TO_TICKS(30));
        ESP_LOGI(TAG, "ALDO2 simülasyon — display sleep, rail dokunulmaz");
    } else if (!new_visual_on && rail == AXP_RAIL_LDO2) {
        co5300_sleep(false);
        vTaskDelay(pdMS_TO_TICKS(120));
        co5300_set_brightness(255);
        lv_obj_invalidate(s_view_power);
    } else {
        /* Diğer tüm rail'ler için gerçek AXP yazımı (DCDC2 dahil — test
         * ile güvenli olduğu doğrulandı). */
        axp2101_set_rail(rail, new_rail_enable);
    }

    if (new_visual_on) {
        /* Rail kapatıldı → 5sn timer (tüm rail'ler için) + touch overlay
         * (sadece display rail için, ALDO1 touch IC'yi öldürür). */
        bool is_display_rail = (rail == AXP_RAIL_DCDC2 || rail == AXP_RAIL_LDO2);
        rail_schedule_wake(is_display_rail);
    } else {
        /* Manuel AÇ yapıldı → pending wake'i iptal et */
        if (s_rail_wake_timer) {
            lv_timer_delete(s_rail_wake_timer);
            s_rail_wake_timer = NULL;
        }
        if (s_rail_wake_overlay) {
            lv_obj_delete(s_rail_wake_overlay);
            s_rail_wake_overlay = NULL;
        }
    }
}

/* Soft Power Off onay dialog'u — Cancel callback (overlay user_data). */
static void soft_poweroff_cancel_cb(lv_event_t* e)
{
    lv_obj_t* overlay = (lv_obj_t*)lv_event_get_user_data(e);
    if (overlay) lv_obj_del(overlay);
}

/* Soft Power Off onay dialog'u — Confirm callback. axp2101_soft_power_off()
 * DÖNMEZ — saat ölür. Wake için USB unplug+plug gerekir (R11 pull-up
 * hardware limitasyonu). */
static void soft_poweroff_confirm_cb(lv_event_t* e)
{
    (void)e;
    ESP_LOGW(TAG, "User confirmed soft power off");
    vTaskDelay(pdMS_TO_TICKS(50));   /* log drain için */
    axp2101_soft_power_off();        /* DÖNMEZ */
}

/* Soft Power Off butonu → onay dialog'u açar. Apple-tarzı: yarı şeffaf
 * siyah backdrop + ortada panel (title + açıklama + Iptal/Kapat butonu). */
static void soft_poweroff_btn_cb(lv_event_t* e)
{
    (void)e;
    lv_obj_t* scr = lv_screen_active();
    if (!scr) return;

    /* Full-screen yarı şeffaf overlay (backdrop) */
    lv_obj_t* overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);   /* arkadaki UI'ya tıklamayı engelle */
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(overlay);

    /* Dialog panel — koyu gri, yuvarlak köşe, ortada */
    lv_obj_t* panel = lv_obj_create(overlay);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 340, 260);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_make(0x1C, 0x1C, 0x1E), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 24, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_make(0x3A, 0x3A, 0x3C), 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Başlık */
    lv_obj_t* title = lv_label_create(panel);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_text(title, "Turn Off Watch");

    /* Açıklama */
    lv_obj_t* desc = lv_label_create(panel);
    lv_obj_set_style_text_color(desc, lv_color_make(0xBE, 0xBE, 0xC2), 0);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, 290);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, 44);
    lv_label_set_text(desc,
        "Are you sure you want to\n"
        "completely turn off the watch?\n\n"
        "You'll need to unplug and replug\n"
        "the USB cable to turn it back on.");

    /* Iptal butonu (sol, gri) */
    lv_obj_t* cancel = lv_button_create(panel);
    lv_obj_set_size(cancel, 130, 50);
    lv_obj_set_style_bg_color(cancel, lv_color_make(0x3A, 0x3A, 0x3C), 0);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cancel, 14, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(cancel, soft_poweroff_cancel_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t* cancel_lbl = lv_label_create(cancel);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);

    /* Kapat butonu (sağ, iOS kırmızısı) */
    lv_obj_t* confirm = lv_button_create(panel);
    lv_obj_set_size(confirm, 130, 50);
    lv_obj_set_style_bg_color(confirm, lv_color_make(0xFF, 0x3B, 0x30), 0);
    lv_obj_set_style_bg_opa(confirm, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(confirm, 14, 0);
    lv_obj_set_style_shadow_width(confirm, 0, 0);
    lv_obj_set_style_border_width(confirm, 0, 0);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(confirm, soft_poweroff_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* confirm_lbl = lv_label_create(confirm);
    lv_obj_set_style_text_color(confirm_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(confirm_lbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(confirm_lbl, "Turn Off");
    lv_obj_center(confirm_lbl);
}

/* Tek bir rail row'u inşa eder: kart + label sol + toggle sağ.
 *
 * v1.4.41 değişikliği — sadece TOGGLE widget'ı tıklanabilir, ROW DEĞİL.
 * Kullanıcı isteği: "row'a dokununca işlem olmasın, sadece toggle'a
 * dokunulduğunda ekran kararsın". Row clickable kaldırıldı + click cb
 * toggle widget'a taşındı + toggle ext_click_area ile alanı 20px her
 * yöne genişletildi (parmak büyük, tam toggle'a değmek zor olmasın). */
static void make_rail_toggle_row(lv_obj_t* parent, int32_t y, const char* text,
                                  axp2101_rail_t rail,
                                  lv_obj_t** out_switch, lv_obj_t** out_knob)
{
    lv_obj_t* row = make_row(parent, y, NULL);
    /* row CLICKABLE değil — make_row default'ta zaten clickable değil */

    /* Label */
    lv_obj_t* lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
#ifdef BOARD_VARIANT_LCD128
    /* Session 38: font 22 + 290 px genişlik AMOLED içindi; 204 px'lik panelde
     * "DCDC2 (0.9V Display)" (tw=236) toggle'ın altına giriyordu. Etiket
     * toggle'ın soluna sığacak kadar (130 px) ve küçük fontla. */
    /* Toggle 44 px ve sağdan 14 px içeride → etiket için 204-58-12 = 134 px
     * kalıyor. Font 14'te "DCDC2 (0.9V Display)" 153 px sürüp kırpılıyordu. */
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lbl, 134);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);
#else
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_width(lbl, 290);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 18, 0);
#endif
    lv_label_set_text(lbl, text);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    /* v1.4.38 Inverse semantic: toggle başlangıç state'i SABIT OFF.
     * Kullanıcı isteği "başlangıçta hepsi kapalı gelsin". AXP register
     * state'inden bağımsız — toggle "rail kapat" aksiyon switch'i olarak
     * çalışır, default'ta hiçbir kapatma uygulanmamış olduğu için OFF. */
    *out_switch = make_toggle_ex(row, false, out_knob);
    lv_obj_align(*out_switch, LV_ALIGN_RIGHT_MID, -14, 0);

    /* v1.4.41: Toggle widget'ı tıklanabilir yap (make_toggle_ex CLICKABLE'ı
     * varsayılan olarak kaldırıyor — burada geri ekle). Ext click area
     * ile 64x34 toggle'ın etkin tıklama alanını ~104x74'e genişlet, parmak
     * ile rahat hedef olsun. */
    lv_obj_add_flag(*out_switch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(*out_switch, LV_OBJ_FLAG_EVENT_BUBBLE);  /* event row'a yükselmesin */
    lv_obj_set_ext_click_area(*out_switch, 20);
    lv_obj_add_event_cb(*out_switch, rail_row_clicked_cb, LV_EVENT_CLICKED,
                        (void*)(uintptr_t)rail);
}

/* ALT EKRAN: Settings → Güç Rayları
 *
 * v1.4.47 düzeni: 9 rail (AXP2101'in kapatılabilir tüm regulator çıkışları,
 * DCDC1=sistem ve CPUSLDO=ESP32 internal hariç). Schematic'e göre doğru
 * label'lar — net adı + voltaj + bilinen hedef (varsa). Bilinmeyenler "?"
 * ile işaretli, kullanıcı test ile keşfeder.
 *
 * Sıra: ALDO grubu önce (1-4), sonra DCDC grubu (2-4), sonra BLDO (1-2). */
static void build_power(lv_obj_t* screen)
{
    s_view_power = make_view(screen, &s_power_content);
    make_header(s_view_power, "Power Rails", go_main_cb);
    make_sep(s_power_content, 0);

#ifdef BOARD_VARIANT_LCD128
    const int32_t Y0   = 8;
    const int32_t STEP = 50;    /* rail row 44 + 6 boşluk */
#else
    const int32_t Y0   = 8;
    const int32_t STEP = 108;
#endif
    int idx = 0;

    /* v1.4.48 (2026-05-15): Label'lar saatte fiziksel test sonuçlarına
     * göre güncellendi. Her toggle'ın gerçek hedefi denenip kanıtlandı.
     * Önceki tahminlerden farklı çıkanlar: ALDO1 audio değil sensors,
     * DCDC2 0.9V "belirsiz" değil display etkili, ALDO4+DCDC3/4+BLDO1/2
     * board'da bağlı değil. */

    /* ALDO1 — Touch + IMU + RTC sensorleri. Test: ON → touch öldü,
     * 5sn timer ile recovery. */
    make_rail_toggle_row(s_power_content, Y0 + STEP * idx,
                         "ALDO1 (3.3V Sensors)", AXP_RAIL_LDO1,
                         &s_rail_aldo1_switch, &s_rail_aldo1_knob);
    idx++;

    /* ALDO2 — Display panel (DSI_PWR_EN). Test: ekran kararıyor.
     * Simülasyon kullanılıyor (gerçek kesim display IC reset eder). */
    make_rail_toggle_row(s_power_content, Y0 + STEP * idx,
                         "ALDO2 (3.3V Display)", AXP_RAIL_LDO2,
                         &s_rail_aldo2_switch, &s_rail_aldo2_knob);
    idx++;

    /* ALDO3 — Motor + sistem paralel. Test: görsel etki yok (motor
     * zaten dönmüyor; saat çalışmaya devam, DCDC1 sistem rail'i hala
     * besliyor). */
    make_rail_toggle_row(s_power_content, Y0 + STEP * idx,
                         "ALDO3 (3.3V Motor)", AXP_RAIL_LDO3,
                         &s_rail_aldo3_switch, &s_rail_aldo3_knob);
    idx++;

    /* ALDO4 — Test: hiçbir etki, board'da bağlı değil. */
    make_rail_toggle_row(s_power_content, Y0 + STEP * idx,
                         "ALDO4 (1.8V Bos)", AXP_RAIL_LDO4,
                         &s_rail_aldo4_switch, &s_rail_aldo4_knob);
    idx++;

    /* DCDC2 — Display etkili (sürpriz bulgu). Test: ON → ekran kararıyor
     * AMA LVGL hang olmuyor (ALDO2'nin aksine), touch overlay düzgün
     * çalışıyor. Gerçek kesim güvenli. */
    make_rail_toggle_row(s_power_content, Y0 + STEP * idx,
                         "DCDC2 (0.9V Display)", AXP_RAIL_DCDC2,
                         &s_rail_dcdc2_switch, &s_rail_dcdc2_knob);
    idx++;

    /* DCDC3 — Test: hiçbir etki, board'da bağlı değil. */
    make_rail_toggle_row(s_power_content, Y0 + STEP * idx,
                         "DCDC3 (1.2V Bos)", AXP_RAIL_DCDC3,
                         &s_rail_dcdc3_switch, &s_rail_dcdc3_knob);
    idx++;

    /* DCDC4 — Test: hiçbir etki, board'da bağlı değil. */
    make_rail_toggle_row(s_power_content, Y0 + STEP * idx,
                         "DCDC4 (1.8V Bos)", AXP_RAIL_DCDC4,
                         &s_rail_dcdc4_switch, &s_rail_dcdc4_knob);
    idx++;

    /* BLDO1 — Test: hiçbir etki, board'da bağlı değil. */
    make_rail_toggle_row(s_power_content, Y0 + STEP * idx,
                         "BLDO1 (1.2V Bos)", AXP_RAIL_BLDO1,
                         &s_rail_bldo1_switch, &s_rail_bldo1_knob);
    idx++;

    /* BLDO2 — Test: hiçbir etki, board'da bağlı değil. */
    make_rail_toggle_row(s_power_content, Y0 + STEP * idx,
                         "BLDO2 (2.8V Bos)", AXP_RAIL_BLDO2,
                         &s_rail_bldo2_switch, &s_rail_bldo2_knob);
    idx++;

    /* Soft Power Off butonu — kırmızı text, koyu kırmızı bg, alt onay dialog'u */
    int32_t btn_y = Y0 + STEP * idx + 24;   /* extra breathing room */
    lv_obj_t* btn_row = make_row(s_power_content, btn_y, NULL);
    lv_obj_add_flag(btn_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(btn_row, lv_color_make(0x3A, 0x10, 0x14), 0);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_row, lv_color_make(0x68, 0x20, 0x28), 0);
    lv_obj_set_style_transform_scale(btn_row, 256, 0);
    lv_obj_set_style_transform_scale(btn_row, 256, LV_STATE_PRESSED);

    lv_obj_t* btn_lbl = lv_label_create(btn_row);
    lv_obj_set_style_text_color(btn_lbl, lv_color_make(0xFF, 0x3B, 0x30), 0);
#ifdef BOARD_VARIANT_LCD128
    /* Session 38: tam metin 355 px sürüyordu (panel 204) → iki yandan
     * kırpılıyordu. Kısa metin + küçük font. */
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(btn_lbl, "Turn Off Watch");
#else
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_22, 0);
    lv_label_set_text(btn_lbl, "Turn Off Watch (Soft Power Off)");
#endif
    lv_obj_center(btn_lbl);
    lv_obj_clear_flag(btn_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(btn_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_add_event_cb(btn_row, soft_poweroff_btn_cb, LV_EVENT_CLICKED, NULL);

#ifdef BOARD_VARIANT_LCD128
    set_content_height(s_power_content, btn_y + 52 + 20);   /* make_row LCD yüksekliği */
#else
    set_content_height(s_power_content, btn_y + 90 + 20);
#endif
}

// ─── App lifecycle ────────────────────────────────────────────────────────────
static void on_start(lv_obj_t* screen)
{
    ESP_LOGI(TAG, "Settings starting");
    g_swipe_dir = 0;  // launcher tıkından kalan swipe'ı temizle
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* Session 13 (C): WiFi yığını artık Settings açılışında DEĞİL, kullanıcı
     * WiFi'yi açınca (wifi_enable_task / scan / connect kendi içinde init eder)
     * kurulur — eager init ~20 KB heap'i kalıcı alıyordu ve WiFi'ye hiç
     * dokunmayan akışlarda bile bu maliyeti ödetiyordu. */
    s_wifi_enabled = wifi_service_is_connected();

    // Session 13 (B): yalnız ana menü kurulur; alt sayfalar ilk girişte (ensure_view)
    s_screen = screen;
    build_main(screen);

    // Başlangıçta sadece ana ekran görünür (state reset)
    reset_to_root();
}

static void on_stop(void)
{
    ESP_LOGI(TAG, "Settings stopping");
    wifi_service_scan_cancel();
    /* Timer sil (LVGL nesnesinden bağımsız) */
    if (s_close_timer)           { lv_timer_del(s_close_timer);           s_close_timer = NULL; }
    if (s_pw_hide_timer)         { lv_timer_del(s_pw_hide_timer);         s_pw_hide_timer = NULL; }
    if (s_commit_timer)          { lv_timer_del(s_commit_timer);          s_commit_timer = NULL; }
    s_pw_len = 0;
    secure_zero(s_pw_real, sizeof(s_pw_real));   /* audit #3: BSS plaintext leak fix */
    /* Tüm widget pointer'larını sıfırla; LVGL nesneleri screen silinince zaten yok olur */
    s_wifi_switch        = NULL;
    s_wifi_knob          = NULL;
    s_br_bar             = NULL;
    s_br_fill            = NULL;
    s_br_bar_w           = 0;
    s_wifi_section_lbl   = NULL;
    s_wifi_list          = NULL;
    s_connected_row      = NULL;
    s_conn_overlay       = NULL;
    s_conn_status_lbl    = NULL;
    s_pw_textarea        = NULL;
    s_pw_scribble        = NULL;
    s_pw_hint_lbl        = NULL;
    s_stroke_n           = 0;
    s_stroke_count       = 0;
    s_stroke_live        = false;
    s_last_ap_count      = 0;
    s_view_main = s_view_general = s_view_about = s_view_network = s_view_display = NULL;
    s_view_auto_update = NULL;   /* Session 13: tembel inşa — HEPSİ sıfırlanmalı */
    s_screen           = NULL;
    s_view_storage     = NULL;   /* v1.2.3 */
    s_storage_content  = NULL;
    s_view_airplane     = NULL;
    s_airplane_content  = NULL;
    s_apm_master_switch = NULL;
    s_apm_master_knob   = NULL;
    s_apm_wifi_switch   = NULL;
    s_apm_wifi_knob     = NULL;
    s_apm_bt_switch     = NULL;
    s_apm_bt_knob       = NULL;
    /* v1.1.1+ (v1.4.38): Güç Rayları — timer + overlay temizliği + visual
     * flag reset. View objesi screen ile silinir, timer LVGL nesnesinden
     * bağımsız (manuel del). Visual flag'ler bir sonraki sayfa açılışında
     * tüm toggle'lar tekrar OFF görünmesi için false'a çekilir. */
    if (s_rail_wake_timer) {
        lv_timer_del(s_rail_wake_timer);
        s_rail_wake_timer = NULL;
    }
    s_rail_wake_overlay  = NULL;   /* screen ile silindi, pointer'ı sıfırla */
    s_view_power         = NULL;
    s_power_content      = NULL;
    s_rail_aldo1_switch  = NULL;  s_rail_aldo1_knob = NULL;
    s_rail_aldo2_switch  = NULL;  s_rail_aldo2_knob = NULL;
    s_rail_aldo3_switch  = NULL;  s_rail_aldo3_knob = NULL;
    s_rail_aldo4_switch  = NULL;  s_rail_aldo4_knob = NULL;
    s_rail_dcdc2_switch  = NULL;  s_rail_dcdc2_knob = NULL;
    s_rail_dcdc3_switch  = NULL;  s_rail_dcdc3_knob = NULL;
    s_rail_dcdc4_switch  = NULL;  s_rail_dcdc4_knob = NULL;
    s_rail_bldo1_switch  = NULL;  s_rail_bldo1_knob = NULL;
    s_rail_bldo2_switch  = NULL;  s_rail_bldo2_knob = NULL;
    s_rail_aldo1_visual_on = false;
    s_rail_aldo2_visual_on = false;
    s_rail_aldo3_visual_on = false;
    s_rail_aldo4_visual_on = false;
    s_rail_dcdc2_visual_on = false;
    s_rail_dcdc3_visual_on = false;
    s_rail_dcdc4_visual_on = false;
    s_rail_bldo1_visual_on = false;
    s_rail_bldo2_visual_on = false;

    /* Software Update state — view objeleri screen ile silinir */
    swupdate_stop_polling();
    s_view_swupdate     = NULL;
    s_swupdate_content  = NULL;
    s_swu_icon          = NULL;
    s_swu_title         = NULL;
    s_swu_subtitle      = NULL;
    s_swu_notes         = NULL;
    s_swu_progress      = NULL;
    s_swu_progress_lbl  = NULL;
    s_swu_button        = NULL;
    s_swu_button_lbl    = NULL;
    s_swu_last_state    = OTA_STATE_IDLE;
}
static void on_resume(void)
{
    reset_to_root();
}
static void on_pause(void)  {}
static bool on_event(const os_event_t* e) { (void)e; return false; }

// ─── Interface export ─────────────────────────────────────────────────────────
static const app_interface_t s_iface = {
    .on_start  = on_start,
    .on_resume = on_resume,
    .on_pause  = on_pause,
    .on_stop   = on_stop,
    .on_event  = on_event,
    .name      = "Settings",
};

const app_interface_t* settings_app_get_interface(void) { return &s_iface; }
