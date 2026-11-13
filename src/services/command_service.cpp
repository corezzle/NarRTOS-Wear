/**
 * @file command_service.cpp
 * @brief Metin komutlarıyla cihazı sürme — taşıyıcıdan bağımsız çekirdek
 *        + USB seri taşıyıcısı (bkz. header). BLE taşıyıcısı: ble_cmd_service.
 *
 * Tasarım notları:
 *  - UART0 RX: log çıkan konsol hattının okuma yönü. uart_driver_install
 *    yalnızca RX buffer'ı kurar; log/printf TX yolu (VFS polling) DEĞİŞMEZ.
 *  - Uygulama açma: doğrudan app_manager_launch ÇAĞRILMAZ (LVGL task işi).
 *    alarm_service ile aynı kalıp: event_bus_publish(EVENT_APP_SWITCH, id)
 *    → app_manager kendi dispatch timer'ında (LVGL task) launch eder.
 *  - Geri: lv_async_call ile LVGL task'ına atlanır (wifi callback kalıbı).
 *  - Jest: g_swipe_dir tek byte volatile — başka task'tan yazmak güvenli;
 *    app_manager'ın 50 ms'lik check_swipe'ı tüketir. Yön eşlemesi kullanıcı
 *    algısıyla verilir (bkz. app_manager.cpp: -1 = sağa, +1 = sola).
 *  - Yanıtlar reply-sink üzerinden akar: USB için printf, BLE için notify.
 *    exec çağrıları mutex ile serileştirilir (USB task + NimBLE host task
 *    aynı anda komut çalıştırabilir).
 */

#ifdef TRACKER_FEATURES

#include "command_service.h"
#include "ui_service.h"
#include "control_center.h"
#include "wifi_service.h"
#include "ble_service.h"
#include "power_service.h"
#include "telemetry_service.h"
#include "gps_service.h"
#include "activity_service.h"
#include "vibration_service.h"
#include "can_service.h"
#include "mic_service.h"
#include "sensor_service.h"
#include "../drivers/imu/icm42670_driver.h"
#include "../drivers/can/twai_driver.h"
#include <math.h>
#include "../core/os_config.h"
#include "../core/app_manager.h"
#include "../core/event_bus.h"
#include "../drivers/display/co5300_driver.h"
#include "../drivers/power/axp2101_driver.h"

#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

static const char* TAG = "CmdSvc";

#define CMD_UART        UART_NUM_0
#include <esp_partition.h>   /* flashtest komutu (TRACKER_FEATURES derlemelerinde) */
#ifdef BOARD_VARIANT_NAR
/* NAR: UART0 (GPIO43/44) nRF9151 modeme gider; konsol USB-Serial-JTAG. */
#include <driver/usb_serial_jtag.h>
#define CMD_USE_USB_JTAG 1
#endif
#define CMD_LINE_MAX    128
#define CMD_REPLY_MAX   384   /* piezo capture satiri (20 ornek + 5 ivme ucgeni) ~210 karakter */

/* ── Yanıt sink'i (exec süresince aktif taşıyıcıya işaret eder) ─────────── */
static SemaphoreHandle_t s_exec_mutex = NULL;
static cmd_reply_fn_t    s_reply      = NULL;
static void*             s_reply_ctx  = NULL;

/* printf tarzı formatla + aktif sink'e teslim et */
static void R(const char* fmt, ...)
{
    char buf[CMD_REPLY_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_reply) s_reply(buf, s_reply_ctx);
}

static void serial_reply(const char* line, void* ctx)
{
    (void)ctx;
    printf("%s\n", line);
}

/* ── İsim → APP_ID eşlemesi ─────────────────────────────────────────────── */
typedef struct { const char* name; app_id_t id; } app_name_t;
static const app_name_t s_apps[] = {
    { "watchface",     APP_ID_WATCHFACE    },
    { "steps",         APP_ID_STEP_COUNTER },
    { "settings",      APP_ID_SETTINGS     },
    { "launcher",      APP_ID_LAUNCHER     },
    { "heartrate",     APP_ID_HEARTRATE    },
    { "weather",       APP_ID_WEATHER      },
    { "timer",         APP_ID_TIMER        },
    { "notifications", APP_ID_NOTIFY       },
    { "gallery",       APP_ID_GALLERY      },
    { "alarm",         APP_ID_ALARM        },
    { "store",         APP_ID_STORE        },
    { "hqmsg",         APP_ID_HQMSG        },   /* v1.2.9 */
};
#define APP_COUNT (sizeof(s_apps) / sizeof(s_apps[0]))

static const char* app_name_by_id(app_id_t id)
{
    for (size_t i = 0; i < APP_COUNT; i++)
        if (s_apps[i].id == id) return s_apps[i].name;
    return "?";
}

static int app_id_by_name(const char* s)
{
    for (size_t i = 0; i < APP_COUNT; i++)
        if (strcasecmp(s_apps[i].name, s) == 0) return (int)s_apps[i].id;
    if (isdigit((unsigned char)s[0])) {
        int id = atoi(s);
        if (id >= 0 && id < APP_ID_COUNT) return id;
    }
    return -1;
}

/* ── LVGL task köprüleri ────────────────────────────────────────────────── */
static void async_back(void* arg)
{
    (void)arg;
    app_manager_back();
}

/* Session 34: Control Center'ı komutla aç/kapat. Panel üst kenardan aşağı
 * sürüklemeyle açılıyor; bu jest touch_read_cb'nin GERÇEK dokunuş dalında
 * işlendiği için sentetik `drag` ile tetiklenemiyor → otonom testte CC'ye
 * ulaşmanın yolu yoktu. */
static void async_cc_open(void* arg)  { (void)arg; control_center_open(); }
static void async_cc_close(void* arg) { (void)arg; control_center_close(); }

/* ── Komut işleyiciler ──────────────────────────────────────────────────── */
static void cmd_help(void)
{
    R("[cmd] commands:");
    R("[cmd]   help | info | reboot | wake");
    R("[cmd]   app launch <name|id> | app back | app current | app list");
    R("[cmd]   swipe left|right|up|down");
    R("[cmd]   tap <x> <y> [hold_ms]   (sentetik dokunuş)  |  ui dump  (LVGL ağacı)");
    R("[cmd]   drag <x0> <y0> <x1> <y1> [dur_ms]   (sentetik sürükleme / scroll)");
    R("[cmd]   cc open|close   (Control Center)");
    R("[cmd]   brightness <0-255>");
    R("[cmd]   set wifi|bt on|off  |  get wifi|bt  |  toggle wifi|bt");
    R("[cmd]   set rail <aldo1-4|dcdc2-4|bldo1-2> on|off  |  get rails  |  get temp  (PMIC'li board)");
    R("[cmd]   set wifi ssid \"<ag adi>\" \"<sifre>\"   (ekransiz kartta WiFi'ye baglan, kalici)");
    R("[cmd]   set backend http://<ip>:3001  |  get backend   (sunucu adresi, kalici)");
    R("[cmd]   set telemetry period <10-3600 sn>  |  get telemetry  |  telemetry send");
    R("[cmd]   get gps  |  gps sim <lat> <lng> [hdop]  |  gps sim off   (GPS testi)");
    R("[cmd]   set telemetry rest_period <sn> | set telemetry rest_after <sn>  (dinlenme modu)");
    R("[cmd]   get tracker | tracker mode auto|active|rest | get activity | activity sim rest|walk|run|off");
    R("[cmd]   get piezo   (titresim sensoru teshis: pin seviyesi + darbe sayaci)");
    R("[cmd]   piezo capture [sn] [hz]   (A modu: ham analog dalga -> nabiz/solunum deneyi)");
    R("[cmd]   can inject 0xID,b0,..,b7 | can stats | can dump [n] | can clear | can json   (ham CAN; cihaz cozmez)");
    R("[cmd]   mic capture [sn=10] [hz=1000] [kanal=1] [varyant=0]   (ES7210 ham ses; varyant 1/2 = esp-bsp kodek dizisi)");
    R("[cmd]   mic raw [sn=10] [kanal=3] [bit=24] [varyant=0]   (48 kHz ikili akis; varyant 3 = HPF kapali; tools/mic_raw.py okur)");
    R("[cmd]   mic sdrec [sn=10] [kanal=3] [bit=24] [varyant=0]   (yalniz SD karta WAV: /sdcard/KAYIT/K######.WAV; mic raw da kart varsa SD'ye kopyalar)");
    R("[cmd]   mic sdls   (SD karttaki kayitlari listele)");
    R("[cmd]   mic nabiz [sn] [kanal=3] [bit=24] [varyant=0] [insan|kopek|kedi] [usb] [nosd]   (kartta nabiz + solunum, alg A2 (kedide mirlama, kopekte soluma tespiti); ham WAV SD'ye; usb = akis bilgisayara: tools/nar_olcum.py)");
    R("[cmd]   mic etiket <metin>   (kayit etiketi: tur/hayvan/yer/durum; LISTE.TXT ve sonuc satirina yazilir)");
    R("[cmd]   mem | mem peek <hex> [n=8] | mem bit <hex> [tur=1000] | mem psram [bekle_sn=2]   (bellek teshisi: butunluk, hucre yaz/oku, PSRAM tarama)");
    R("[cmd]   imu status | imu events [sn=10] | imu apex <ped|tilt|smd|ff|lowg|wom> on|off");
    R("[cmd]   imu wom <mg> | imu tilt <0-3:0/2/4/6 s> | imu smd <0-4> | imu ff <min 0-15> <max 0-15> | imu lowg <peak 0-31> <time 0-7>");
    R("[cmd]   set can bitrate 125|250|500|1000 | get can   (TWAI icin ENABLE_CAN=1 + transceiver gerekir)");
}

static void cmd_info(void)
{
    R("[cmd] {\"version\":\"%s\",\"uptime_s\":%lld,\"heap_free\":%u,"
      "\"wifi\":%s,\"app\":\"%s\",\"power_state\":%d}",
      OS_VERSION_STR,
      (long long)(esp_timer_get_time() / 1000000),
      (unsigned)esp_get_free_heap_size(),
      wifi_service_is_connected() ? "true" : "false",
      app_name_by_id(app_manager_current_id()),
      (int)power_service_get_state());
}

static void cmd_app(char* sub, char* arg)
{
    if (!sub) { R("[cmd] ERR: app launch|back|current|list"); return; }

    if (strcasecmp(sub, "launch") == 0) {
        if (!arg) { R("[cmd] ERR: app launch <name|id>"); return; }
        int id = app_id_by_name(arg);
        if (id < 0) { R("[cmd] ERR: unknown app '%s' (bkz. app list)", arg); return; }
        /* Ekran uykudaysa önce uyandır — geçiş animasyonu ekran kapalıyken
         * tamamlanamıyor. */
        power_service_user_activity(POWER_WAKE_SRC_BTN_TOP);
        /* Thread-safe yol: alarm_service kalıbı — app_manager LVGL task'ında açar */
        event_bus_publish(EVENT_APP_SWITCH, (uint32_t)id);
        R("[cmd] OK: launch %s (id=%d)", app_name_by_id((app_id_t)id), id);
    } else if (strcasecmp(sub, "back") == 0) {
        power_service_user_activity(POWER_WAKE_SRC_BTN_TOP);
        lv_async_call(async_back, NULL);
        R("[cmd] OK: back");
    } else if (strcasecmp(sub, "current") == 0) {
        app_id_t id = app_manager_current_id();
        R("[cmd] OK: current=%s (id=%d)", app_name_by_id(id), (int)id);
    } else if (strcasecmp(sub, "list") == 0) {
        for (size_t i = 0; i < APP_COUNT; i++)
            R("[cmd]   %-13s id=%d", s_apps[i].name, (int)s_apps[i].id);
    } else {
        R("[cmd] ERR: app launch|back|current|list");
    }
}

static void cmd_swipe(const char* dir)
{
    /* Kullanıcı algısı eşlemesi (app_manager.cpp check_swipe ile aynı):
     * sağa=-1 sola=+1 yukarı=-2 aşağı=+2. Ekran uykudaysa önce uyandır. */
    int8_t v = 0;
    if      (!dir)                          { R("[cmd] ERR: swipe left|right|up|down"); return; }
    else if (strcasecmp(dir, "right") == 0) v = -1;
    else if (strcasecmp(dir, "left")  == 0) v = +1;
    else if (strcasecmp(dir, "up")    == 0) v = -2;
    else if (strcasecmp(dir, "down")  == 0) v = +2;
    else { R("[cmd] ERR: swipe left|right|up|down"); return; }

    power_service_user_activity(POWER_WAKE_SRC_TOUCH);
    g_swipe_dir = v;
    R("[cmd] OK: swipe %s (dir=%d)", dir, (int)v);
}

/* ── Otonom test: sentetik tap/drag + UI ağaç dökümü (C6 Session 9 portu) ── */
static void cmd_tap(const char* xs, const char* ys, const char* hs)
{
    if (!xs || !ys) { R("[cmd] ERR: tap <x> <y> [hold_ms]"); return; }
    int x = atoi(xs), y = atoi(ys);
    int hold = hs ? atoi(hs) : 80;
    if (hold < 20)   hold = 20;
    if (hold > 5000) hold = 5000;
    power_service_user_activity(POWER_WAKE_SRC_TOUCH);
    ui_service_inject_tap(x, y, (uint32_t)hold);
    R("[cmd] OK: tap %d,%d hold=%dms", x, y, hold);
}

static void cmd_drag(char** save)
{
    const char* a[5] = {0};
    int n = 0;
    for (; n < 5; n++) { a[n] = strtok_r(NULL, " \t", save); if (!a[n]) break; }
    if (n < 4) { R("[cmd] ERR: drag <x0> <y0> <x1> <y1> [dur_ms]"); return; }
    int dur = a[4] ? atoi(a[4]) : 300;
    if (dur < 50)   dur = 50;
    if (dur > 5000) dur = 5000;
    power_service_user_activity(POWER_WAKE_SRC_TOUCH);
    ui_service_inject_drag(atoi(a[0]), atoi(a[1]), atoi(a[2]), atoi(a[3]), (uint32_t)dur);
    R("[cmd] OK: drag %s,%s -> %s,%s dur=%dms", a[0], a[1], a[2], a[3], dur);
}

/* Döküm satırları ÖNCE belleğe toplanır, LVGL kilidi bırakıldıktan SONRA
 * seri porta yazılır. Doğrudan yazma, kilidi 100+ ms tutup UI task'ını
 * blokluyordu → ölçüm aracının kendisi sahte [stall] üretiyordu. */
typedef struct { char* buf; size_t cap; size_t len; } dump_sink_t;

static void dump_line_cb(const char* line, void* ctx)
{
    dump_sink_t* d = (dump_sink_t*)ctx;
    if (!d || !d->buf) return;
    size_t n = strlen(line);
    if (d->len + n + 2 >= d->cap) return;      /* tampon doldu — sessizce kes */
    memcpy(d->buf + d->len, line, n);
    d->len += n;
    d->buf[d->len++] = '\n';
    d->buf[d->len]   = '\0';
}

static void cmd_ui(const char* sub)
{
    if (!sub || strcasecmp(sub, "dump") != 0) { R("[cmd] ERR: ui dump"); return; }

    dump_sink_t d = { NULL, 48 * 1024, 0 };
    d.buf = (char*)heap_caps_malloc(d.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d.buf) d.buf = (char*)malloc(d.cap = 12 * 1024);
    if (!d.buf) { R("[cmd] ERR: ui dump (bellek yok)"); return; }
    d.buf[0] = '\0';

    ui_service_dump_tree(dump_line_cb, &d);      /* kilit yalnız gezinti boyunca */

    char* p = d.buf;
    while (p && *p) {
        char* nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        R("[ui] %s", p);
        vTaskDelay(pdMS_TO_TICKS(2));            /* host FIFO'sunu boşaltsın */
        if (!nl) break;
        p = nl + 1;
    }
    free(d.buf);
    R("[cmd] OK: ui dump");
}

static void cmd_cc(const char* sub)
{
    if (sub && strcasecmp(sub, "open") == 0) {
        lv_async_call(async_cc_open, NULL);
        R("[cmd] OK: cc open");
    } else if (sub && strcasecmp(sub, "close") == 0) {
        lv_async_call(async_cc_close, NULL);
        R("[cmd] OK: cc close");
    } else {
        R("[cmd] ERR: cc open|close");
    }
}

static void cmd_brightness(const char* arg)
{
    if (!arg || !isdigit((unsigned char)arg[0])) {
        R("[cmd] ERR: brightness <0-255>");
        return;
    }
    int v = atoi(arg);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    esp_err_t err = co5300_set_brightness((uint8_t)v);
    if (err == ESP_OK) R("[cmd] OK: brightness=%d", v);
    else               R("[cmd] ERR: brightness failed (%s)", esp_err_to_name(err));
}

/* ── Parametre kontrolü: set / get / toggle ─────────────────────────────────
 * Gerçek backend etkisi olan anahtarlar. (Control Center'daki bazı toggle'lar
 * yalnızca görseldir; burada YALNIZCA gerçekten çalışan API'ler sunulur.)
 *   wifi : kullanıcı tercihi (NVS, kalıcı) + bağlan/kopar
 *   bt   : BLE advertise başlat / bağlantıyı kes + advertise durdur
 */
static void wifi_connect_worker(void* arg)
{
    (void)arg;
    /* Bloklayıcı bağlanma komut task'ında/NimBLE host'unda ÇALIŞTIRILMAZ —
     * (BLE host'u bloklamak bağlantıyı düşürür). Kısa ömürlü işçi görev. */
    wifi_service_connect(15000);
    vTaskDelete(NULL);
}

static bool parse_onoff(const char* v, bool* out)
{
    if (!v) return false;
    if (strcasecmp(v, "on") == 0 || strcmp(v, "1") == 0)  { *out = true;  return true; }
    if (strcasecmp(v, "off") == 0 || strcmp(v, "0") == 0) { *out = false; return true; }
    return false;
}

static void set_wifi(bool on)
{
    wifi_service_set_user_preference(on);   /* NVS'e kalıcı yazılır */
    if (on) {
        xTaskCreate(wifi_connect_worker, "wifi_conn", 4096, NULL, 3, NULL);
        R("[cmd] OK: wifi=on (connecting async — 'get wifi' ile kontrol et)");
    } else {
        wifi_service_disconnect();
        R("[cmd] OK: wifi=off");
    }
}

static void set_bt(bool on)
{
    if (on) {
        ble_service_start_advertising();
        R("[cmd] OK: bt=on (advertising)");
    } else {
        /* DİKKAT: komut BLE'den geldiyse bu, telefonla bağlantıyı da keser. */
        R("[cmd] OK: bt=off (bağlıysa bağlantı kesilecek)");
        ble_service_disconnect();
        ble_service_stop_advertising();
    }
}

/* ── Güç rayları (AXP2101 PMIC — yalnız AMOLED board'da mevcut) ──────────
 * Settings → Güç Rayları sayfasının komut karşılığı. PMIC olmayan board'da
 * (LCD128) sürücü hata döner; komut bunu dürüstçe raporlar. */
typedef struct { const char* name; axp2101_rail_t rail; } rail_name_t;
static const rail_name_t s_rails[] = {
    { "dcdc2", AXP_RAIL_DCDC2 }, { "dcdc3", AXP_RAIL_DCDC3 },
    { "dcdc4", AXP_RAIL_DCDC4 },
    { "aldo1", AXP_RAIL_LDO1  }, { "aldo2", AXP_RAIL_LDO2  },
    { "aldo3", AXP_RAIL_LDO3  }, { "aldo4", AXP_RAIL_LDO4  },
    { "bldo1", AXP_RAIL_BLDO1 }, { "bldo2", AXP_RAIL_BLDO2 },
    /* DCDC1 bilerek listede YOK: sistem rayı (MCU/Flash) — kapatmak cihazı
     * öldürür; Settings sayfası da sunmuyor. */
};
#define RAIL_COUNT (sizeof(s_rails) / sizeof(s_rails[0]))

static int rail_by_name(const char* s)
{
    for (size_t i = 0; i < RAIL_COUNT; i++)
        if (strcasecmp(s_rails[i].name, s) == 0) return (int)i;
    return -1;
}

static void set_rail(const char* name, bool on)
{
    int i = rail_by_name(name);
    if (i < 0) {
        R("[cmd] ERR: unknown rail '%s' (aldo1-4|dcdc2-4|bldo1-2)", name);
        return;
    }
    /* ALDO2/DCDC2 = display rayları: kapatınca ekran kararır (Settings
     * sayfasındaki davranışın aynısı) — komut yine de izin verir, uyarır. */
    esp_err_t err = axp2101_set_rail(s_rails[i].rail, on);
    if (err == ESP_OK) {
        R("[cmd] OK: rail %s=%s%s", s_rails[i].name, on ? "on" : "off",
          (!on && (s_rails[i].rail == AXP_RAIL_LDO2 ||
                   s_rails[i].rail == AXP_RAIL_DCDC2))
              ? " (display rayı — ekran kararabilir)" : "");
    } else {
        R("[cmd] ERR: rail %s failed (%s — bu board'da PMIC olmayabilir)",
          s_rails[i].name, esp_err_to_name(err));
    }
}

static void get_rails(void)
{
    for (size_t i = 0; i < RAIL_COUNT; i++) {
        bool en = false;
        esp_err_t err = axp2101_get_rail_state(s_rails[i].rail, &en);
        if (err == ESP_OK) R("[cmd]   %-6s %s", s_rails[i].name, en ? "on" : "off");
        else { R("[cmd] ERR: PMIC okunamadı (%s) — bu board'da yok olabilir",
                 esp_err_to_name(err)); return; }
    }
}

/* Tırnaklı argüman ayrıştırıcı: "abc def" → abc def; tırnaksızsa boşluğa kadar */
static bool parse_quoted(char** rest, char* out, size_t n)
{
    char* p = *rest; while (*p == ' ' || *p == '\t') p++;
    if (!*p) return false;
    size_t i = 0;
    if (*p == '"') { p++; while (*p && *p != '"' && i < n - 1) out[i++] = *p++; if (*p == '"') p++; }
    else { while (*p && *p != ' ' && *p != '\t' && i < n - 1) out[i++] = *p++; }
    out[i] = '\0'; *rest = p; return i > 0 || true;
}
static void wifi_cmd_result_cb(bool connected, void* ud)
{
    (void)ud;
    printf("[cmd] %s: wifi %s\n", connected ? "OK" : "ERR", connected ? "baglandi" : "baglanamadi (ag adi/sifre?)");
}

static void cmd_get(const char* param);   /* ileri bildirim: can stats → get can */

/* ── can inject|stats|dump|clear|json ────────────────────────────────────────
 * inject: "0xID,b0,..,b7" — transceiver olmadan boru hattını (tablo → telemetri →
 * sunucu) sürmek için. Enjekte edilen frame tabloda gerçek frame gibi durur. */
static void cmd_can_inject(char* arg)
{
    if (!arg) { R("[cmd] ERR: can inject 0xID,b0,..,b7"); return; }
    char* save = NULL;
    char* tok = strtok_r(arg, ",", &save);
    if (!tok) { R("[cmd] ERR: kimlik yok"); return; }
    char* end = NULL;
    unsigned long id = strtoul(tok, &end, 0);     /* 0x ile hex, yoksa ondalik */
    if (end == tok || id > 0x1FFFFFFFUL) { R("[cmd] ERR: kimlik gecersiz '%s'", tok); return; }
    uint8_t data[8]; uint8_t dlc = 0;
    while (dlc < 8 && (tok = strtok_r(NULL, ",", &save)) != NULL) {
        unsigned long b = strtoul(tok, &end, 16);
        if (end == tok || b > 0xFF) { R("[cmd] ERR: bayt gecersiz '%s' (hex 00-FF)", tok); return; }
        data[dlc++] = (uint8_t)b;
    }
    bool ext = id > 0x7FF;
    can_service_ingest((uint32_t)id, ext, data, dlc);
    R("[cmd] OK: can inject id=0x%08lX ext=%d dlc=%u", id, ext ? 1 : 0, (unsigned)dlc);
}

static void cmd_can_dump(const char* ns)
{
    static can_frame_t snap[CAN_ID_TABLE_SIZE];
    size_t max = ns ? (size_t)strtoul(ns, NULL, 10) : 16;
    if (max == 0 || max > CAN_ID_TABLE_SIZE) max = CAN_ID_TABLE_SIZE;
    size_t n = can_service_snapshot(snap, max);
    if (!n) { R("[cmd] can: tablo bos"); return; }
    int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < n; i++) {
        char line[64]; can_service_format_frame(&snap[i], line, sizeof(line));
        R("[cmd]   %-36s  n=%lu  %lld ms once", line, (unsigned long)snap[i].count,
          (long long)((now - snap[i].last_us) / 1000));
    }
    R("[cmd] OK: %u kimlik listelendi (en taze ustte)", (unsigned)n);
}

static void cmd_can(char** save)
{
    const char* sub = strtok_r(NULL, " \t", save);
    if (!sub) { R("[cmd] ERR: can inject|stats|dump|clear|json"); return; }
    if (strcasecmp(sub, "inject") == 0) { cmd_can_inject(strtok_r(NULL, " \t", save)); return; }
    if (strcasecmp(sub, "dump") == 0)   { cmd_can_dump(strtok_r(NULL, " \t", save)); return; }
    if (strcasecmp(sub, "clear") == 0)  { can_service_clear(); R("[cmd] OK: can tablo bosaltildi"); return; }
    if (strcasecmp(sub, "stats") == 0)  { cmd_get("can"); return; }
    if (strcasecmp(sub, "json") == 0) {
        /* Telemetriye gidecek parça, birebir */
        static char js[CAN_FRAMES_PER_REPORT * 40 + 96];
        size_t n = can_service_json(js, sizeof(js));
        if (!n) { R("[cmd] can: tablo bos, telemetriye 'can' alani gitmez"); return; }
        for (size_t i = 0; i < n; i += 150) R("[cmd] %.150s", js + i);   /* satir sinirina bol */
        R("[cmd] OK: %u karakter", (unsigned)n);
        return;
    }
    R("[cmd] ERR: can inject|stats|dump|clear|json");
}

static void cmd_set(const char* param, const char* val, char** save)
{
    /* "set wifi ssid "<ssid>" "<pass>"" — tırnaklı argümanlar; sifre bos olabilir */
    if (param && strcasecmp(param, "wifi") == 0 && val && strcasecmp(val, "ssid") == 0) {
        char* rest = *save;                     /* kalan ham metin */
        char ssid[64] = {0}, pass[64] = {0};
        if (!rest || !parse_quoted(&rest, ssid, sizeof(ssid))) { R("[cmd] ERR: set wifi ssid \"<ag>\" \"<sifre>\""); return; }
        parse_quoted(&rest, pass, sizeof(pass)); /* yoksa acik ag */
        R("[cmd] .. wifi: '%s' agina baglaniliyor (15 sn'ye kadar)", ssid);
        wifi_service_set_user_preference(true);
        wifi_service_connect_to_ap(ssid, pass, wifi_cmd_result_cb, NULL);
        return;
    }
    /* "set backend http://host:port" */
    if (param && strcasecmp(param, "backend") == 0) {
        if (!val || strncmp(val, "http", 4) != 0) { R("[cmd] ERR: set backend http://<ip>:3001"); return; }
        if (telemetry_service_set_backend(val) == ESP_OK) R("[cmd] OK: backend=%s (kalici)", telemetry_service_get_backend());
        else R("[cmd] ERR: backend kaydedilemedi");
        return;
    }
    /* "set rail <isim> on|off" — üç argümanlı özel biçim */
    if (param && strcasecmp(param, "rail") == 0) {
        const char* rname = val;
        const char* v     = strtok_r(NULL, " \t", save);
        bool on;
        if (!rname || !parse_onoff(v, &on)) { R("[cmd] ERR: set rail <isim> on|off"); return; }
        set_rail(rname, on);
        return;
    }
    /* "set can bitrate <kbps>" — NVS, reboot gerekir */
    if (param && strcasecmp(param, "can") == 0) {
        const char* v = strtok_r(NULL, " \t", save);
        if (!val || strcasecmp(val, "bitrate") != 0 || !v) { R("[cmd] ERR: set can bitrate 125|250|500|1000"); return; }
        long k = strtol(v, NULL, 10);
        if (can_service_set_bitrate((uint32_t)k) == ESP_OK) R("[cmd] OK: can bitrate=%ld kbps (kalici, reboot gerekli)", k);
        else R("[cmd] ERR: desteklenen hizlar 125/250/500/1000");
        return;
    }
    /* "set telemetry period <sn>" */
    if (param && strcasecmp(param, "telemetry") == 0) {
        const char* v = strtok_r(NULL, " \t", save);   /* val = "period", v = sayı */
        if (!val || !v) { R("[cmd] ERR: set telemetry period|rest_period|rest_after <sn>"); return; }
        long sec = strtol(v, NULL, 10);
        esp_err_t e;
        if      (strcasecmp(val, "period") == 0)      e = telemetry_service_set_period((uint32_t)sec);
        else if (strcasecmp(val, "rest_period") == 0) e = telemetry_service_set_rest_period((uint32_t)sec);
        else if (strcasecmp(val, "rest_after") == 0)  e = telemetry_service_set_rest_after((uint32_t)sec);
        else { R("[cmd] ERR: set telemetry period|rest_period|rest_after <sn>"); return; }
        if (e == ESP_OK)
            R("[cmd] OK: telemetry period=%u rest_period=%u rest_after=%u (kalici)",
              (unsigned)telemetry_service_get_period(), (unsigned)telemetry_service_get_rest_period(),
              (unsigned)telemetry_service_get_rest_after());
        else R("[cmd] ERR: NVS yazilamadi");
        return;
    }

    bool on;
    if (!param || !parse_onoff(val, &on)) {
        R("[cmd] ERR: set wifi|bt on|off  |  set rail <isim> on|off");
        return;
    }
    if      (strcasecmp(param, "wifi") == 0) set_wifi(on);
    else if (strcasecmp(param, "bt")   == 0) set_bt(on);
    else R("[cmd] ERR: unknown param '%s' (wifi|bt|rail|telemetry)", param);
}

static void cmd_get(const char* param)
{
    if (!param) { R("[cmd] ERR: get wifi|bt|rails|telemetry|gps"); return; }
    if (strcasecmp(param, "rails") == 0) { get_rails(); return; }
    if (strcasecmp(param, "temp") == 0) {
        float c = 0;
        esp_err_t e = axp2101_get_die_temp(&c);
        if (e == ESP_OK) R("[cmd] PMIC cip sicakligi: %.1f C", c);
        else R("[cmd] ERR: PMIC sicakligi okunamadi (%s)", esp_err_to_name(e));
        return;
    }
    if (strcasecmp(param, "telemetry") == 0) {
        R("[cmd] OK: telemetry period=%u s sent=%u last_http=%d wifi=%s",
          (unsigned)telemetry_service_get_period(), (unsigned)telemetry_service_sent_count(),
          telemetry_service_last_http(), wifi_service_is_connected() ? "yes" : "no");
        return;
    }
    if (strcasecmp(param, "backend") == 0) { R("[cmd] OK: backend=%s", telemetry_service_get_backend()); return; }
    if (strcasecmp(param, "can") == 0) {
        can_stats_t st; can_service_stats(&st);
        static const char* bs[] = { "stopped", "running", "bus-off", "recovering" };
        int b = can_twai_bus_state();
        R("[cmd] OK: can bitrate=%lu kbps twai=%s rx_total=%lu distinct=%u/%u dropped=%lu",
          (unsigned long)can_service_bitrate(), b < 0 ? (ENABLE_CAN ? "yok" : "derlenmedi (ENABLE_CAN=0)") : bs[b],
          (unsigned long)st.rx_total, (unsigned)st.distinct, (unsigned)st.capacity, (unsigned long)st.dropped_full);
        return;
    }
    if (strcasecmp(param, "tracker") == 0) {
        int fc = telemetry_service_get_force();
        R("[cmd] OK: tracker mode=%s force=%s period_active=%u rest_period=%u rest_after=%u gps=%s",
          telemetry_service_get_mode() == TRK_MODE_ACTIVE ? "active" : "rest",
          fc < 0 ? "auto" : (fc ? "active" : "rest"),
          (unsigned)telemetry_service_get_period(), (unsigned)telemetry_service_get_rest_period(),
          (unsigned)telemetry_service_get_rest_after(), gps_service_is_enabled() ? "on" : "off");
        return;
    }
    if (strcasecmp(param, "activity") == 0) {
        activity_t a; activity_service_get(&a);
        R("[cmd] OK: activity state=%s%s steps=%u spm=%u taps/min=%u intensity=%u since_motion=%us",
          activity_state_name(a.state), a.simulated ? "(SIM)" : "", (unsigned)a.steps_total,
          (unsigned)a.steps_per_min, (unsigned)a.taps_per_min, (unsigned)a.intensity, (unsigned)a.since_motion_s);
        return;
    }
    if (strcasecmp(param, "piezo") == 0) {
        /* Teshis: level=1 surekli ise esik/besleme sorunu (sinyal hep HIGH, kenar yok);
         * level=0 ve total artmiyorsa modul tetiklenmiyor (potansiyometre/anahtar D). */
        uint32_t sl = vibration_service_since_last_s();
        R("[cmd] OK: piezo present=%s level=%d total=%u taps/min=%u since_last=%s",
          vibration_service_present() ? "yes" : "no", vibration_service_level(),
          (unsigned)vibration_service_taps_total(), (unsigned)vibration_service_taps_per_min(),
          sl == UINT32_MAX ? "-" : "");
        if (sl != UINT32_MAX) R("[cmd]   since_last=%us", (unsigned)sl);
        return;
    }
    if (strcasecmp(param, "gps") == 0) {
        gps_fix_t f;
        bool ok = gps_service_get_fix(&f);
        R("[cmd] OK: gps present=%s sentences=%u fix=%s%s lat=%.6f lng=%.6f hdop=%.1f sats=%u age=%us",
          gps_service_is_present() ? "yes" : "no", (unsigned)gps_service_sentences(),
          ok ? "yes" : "no", (ok && f.simulated) ? "(SIM)" : "",
          ok ? f.lat : 0.0, ok ? f.lng : 0.0, f.hdop, f.sats, (unsigned)f.age_s);
        return;
    }
    if (strcasecmp(param, "wifi") == 0) {
        R("[cmd] OK: wifi connected=%s pref=%s",
          wifi_service_is_connected()      ? "yes" : "no",
          wifi_service_get_user_preference() ? "on" : "off");
    } else if (strcasecmp(param, "bt") == 0) {
        static const char* names[] = { "off", "init", "advertising",
                                       "connected", "bonded", "error" };
        int st = (int)ble_service_get_state();
        R("[cmd] OK: bt state=%s", (st >= 0 && st <= 5) ? names[st] : "?");
    } else {
        R("[cmd] ERR: unknown param '%s' (wifi|bt|rails|telemetry|gps|tracker|activity)", param);
    }
}

/* ── imu: ICM-42670-P APEX masa testi (2026-11-04) ─────────────────────────── */
static const struct { const char* n; uint32_t bit; } k_apex[] = {
    { "ped", IMU_APEX_PED }, { "tilt", IMU_APEX_TILT }, { "smd", IMU_APEX_SMD },
    { "ff", IMU_APEX_FF },   { "lowg", IMU_APEX_LOWG }, { "wom", IMU_APEX_WOM },
};

static void imu_print_status(void)
{
    uint32_t m = icm42670_apex_mask();
    imu_apex_data_t d = {};
    esp_err_t e = icm42670_apex_read(&d);
    R("[cmd] OK: imu apex=%s%s%s%s%s%s int1=GPIO%d seviye=%d (0=kesme bekliyor) kesme_sayisi=%u",
      (m & IMU_APEX_PED) ? "ped " : "", (m & IMU_APEX_TILT) ? "tilt " : "", (m & IMU_APEX_SMD) ? "smd " : "",
      (m & IMU_APEX_FF) ? "ff " : "", (m & IMU_APEX_LOWG) ? "lowg " : "", (m & IMU_APEX_WOM) ? "wom " : "",
      PIN_IMU_INT1, icm42670_int_level(), (unsigned)sensor_service_apex_event_count());
    if (e == ESP_OK)
        R("[cmd]   adim=%u (toplam %u) kadans=%u.%02u ornek aktivite=%s dmp=%s",
          d.steps, (unsigned)sensor_service_get_steps(), d.cadence_u62 >> 2, (d.cadence_u62 & 3) * 25,
          d.activity == 1 ? "YURUME" : d.activity == 2 ? "KOSU" : "bilinmiyor", d.dmp_idle ? "bosta" : "calisiyor");
    else R("[cmd]   apex okunamadi: %s", esp_err_to_name(e));
}

static void cmd_imu(char** save)
{
    const char* sub = strtok_r(NULL, " \t", save);
    const char* a1  = strtok_r(NULL, " \t", save);
    const char* a2  = strtok_r(NULL, " \t", save);
    if (!sub || strcasecmp(sub, "status") == 0) { imu_print_status(); return; }
    if (strcasecmp(sub, "events") == 0) {
        /* N sn boyunca kesmeleri canlı yaz. sensor_service da aynı bayrağı tüketir; burada
         * doğrudan status okunur, böylece hangi olayın geldiği komut çıktısında görünür. */
        int sn = a1 ? atoi(a1) : 10; if (sn < 1) sn = 1; if (sn > 120) sn = 120;
        uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000), n = 0, t_cls = 0;
        uint8_t  last_act = 0xFF; bool last_idle = true;
        R("[cmd] imu events: %d sn dinleniyor (yuru / egdir / dusur / vur)...", sn);
        while ((uint32_t)(esp_timer_get_time() / 1000) - t0 < (uint32_t)sn * 1000) {
            /* 2026-11-06: aktivite sınıfı/DMP durumu hareket SIRASINDA görünsün (bitişte okununca
             * çip çoktan "bilinmiyor"a dönmüş oluyordu — T2 koşu testi). 500 ms'de bir, değişince yaz. */
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (now - t_cls >= 500) {
                t_cls = now;
                imu_apex_data_t d;
                if (icm42670_apex_read(&d) == ESP_OK && (d.activity != last_act || d.dmp_idle != last_idle)) {
                    last_act = d.activity; last_idle = d.dmp_idle;
                    R("[imu] +%5u ms aktivite=%s adim=%u kadans=%u.%02u dmp=%s", (unsigned)(now - t0),
                      d.activity == 1 ? "YURUME" : d.activity == 2 ? "KOSU" : "bilinmiyor",
                      (unsigned)d.steps, d.cadence_u62 >> 2, (d.cadence_u62 & 3) * 25, d.dmp_idle ? "bosta" : "calisiyor");
                }
            }
            if (icm42670_int_take() || icm42670_int_level() == 0) {
                imu_apex_events_t ev;
                if (icm42670_apex_events(&ev) == ESP_OK && (ev.wom || ev.step || ev.tilt || ev.smd || ev.ff || ev.lowg || ev.step_ovf)) {
                    n++;
                    R("[imu] +%5u ms %s%s%s%s%s%s%s", (unsigned)((uint32_t)(esp_timer_get_time() / 1000) - t0),
                      ev.wom ? "WOM " : "", ev.step ? "ADIM " : "", ev.step_ovf ? "ADIM-TASMA " : "",
                      ev.tilt ? "TILT " : "", ev.smd ? "SMD " : "", ev.ff ? "DUSME " : "", ev.lowg ? "LOW-G " : "");
                    if (ev.ff) R("[imu]   dusme suresi %u ornek", ev.ff_dur);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        R("[cmd] OK: imu events bitti, %u olay", (unsigned)n);
        imu_print_status();
        return;
    }
    if (strcasecmp(sub, "apex") == 0 && a1 && a2) {
        uint32_t bit = 0;
        for (size_t i = 0; i < sizeof(k_apex) / sizeof(k_apex[0]); i++)
            if (strcasecmp(a1, k_apex[i].n) == 0) bit = k_apex[i].bit;
        if (!bit) { R("[cmd] ERR: imu apex ped|tilt|smd|ff|lowg|wom on|off"); return; }
        uint32_t m = icm42670_apex_mask();
        m = (strcasecmp(a2, "on") == 0) ? (m | bit) : (m & ~bit);
        esp_err_t e = icm42670_apex_enable(m, true);
        R("[cmd] %s: imu apex %s %s", e == ESP_OK ? "OK" : esp_err_to_name(e), a1, a2);
        return;
    }
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if      (strcasecmp(sub, "wom")  == 0 && a1) e = icm42670_enable_wom((uint16_t)atoi(a1));
    else if (strcasecmp(sub, "tilt") == 0 && a1) e = icm42670_set_tilt_wait((uint8_t)atoi(a1));
    else if (strcasecmp(sub, "smd")  == 0 && a1) e = icm42670_set_smd_sensitivity((uint8_t)atoi(a1));
    else if (strcasecmp(sub, "ff")   == 0 && a1 && a2) e = icm42670_set_ff_duration((uint8_t)atoi(a1), (uint8_t)atoi(a2));
    else if (strcasecmp(sub, "lowg") == 0 && a1 && a2) e = icm42670_set_lowg((uint8_t)atoi(a1), (uint8_t)atoi(a2));
    else { R("[cmd] ERR: imu status|events [sn]|apex <f> on|off|wom <mg>|tilt <0-3>|smd <0-4>|ff <min> <max>|lowg <peak> <time>"); return; }
    R("[cmd] %s: imu %s", e == ESP_OK ? "OK" : esp_err_to_name(e), sub);
}

static void cmd_toggle(const char* param)
{
    if (!param) { R("[cmd] ERR: toggle wifi|bt"); return; }
    if      (strcasecmp(param, "wifi") == 0) set_wifi(!wifi_service_is_connected());
    else if (strcasecmp(param, "bt")   == 0) {
        ble_state_t st = ble_service_get_state();
        set_bt(!(st == BLE_STATE_ADVERTISING || st == BLE_STATE_CONNECTED ||
                 st == BLE_STATE_BONDED));
    }
    else R("[cmd] ERR: unknown param '%s' (wifi|bt)", param);
}

/* ── Parser çekirdeği (taşıyıcıdan bağımsız) ────────────────────────────── */
static void exec_locked(char* line)
{
    /* baş/son boşluk kırp */
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == '\r' || line[n-1] == '\n' ||
                     line[n-1] == ' '  || line[n-1] == '\t')) line[--n] = '\0';
    if (n == 0) return;

    char* save = NULL;
    char* tok  = strtok_r(line, " \t", &save);
    if (!tok) return;

    if      (strcasecmp(tok, "help") == 0)       cmd_help();
    else if (strcasecmp(tok, "info") == 0)       cmd_info();
    else if (strcasecmp(tok, "app")  == 0)       cmd_app(strtok_r(NULL, " \t", &save),
                                                         strtok_r(NULL, " \t", &save));
    else if (strcasecmp(tok, "swipe") == 0)      cmd_swipe(strtok_r(NULL, " \t", &save));
    else if (strcasecmp(tok, "tap") == 0) {
        char* xs = strtok_r(NULL, " \t", &save);
        char* ys = strtok_r(NULL, " \t", &save);
        char* hs = strtok_r(NULL, " \t", &save);
        cmd_tap(xs, ys, hs);
    }
    else if (strcasecmp(tok, "drag") == 0)       cmd_drag(&save);
    else if (strcasecmp(tok, "ui") == 0)         cmd_ui(strtok_r(NULL, " \t", &save));
    else if (strcasecmp(tok, "cc") == 0)         cmd_cc(strtok_r(NULL, " \t", &save));
    else if (strcasecmp(tok, "brightness") == 0) cmd_brightness(strtok_r(NULL, " \t", &save));
    else if (strcasecmp(tok, "set") == 0)        cmd_set(strtok_r(NULL, " \t", &save),
                                                         strtok_r(NULL, " \t", &save), &save);
    else if (strcasecmp(tok, "get") == 0)        cmd_get(strtok_r(NULL, " \t", &save));
    else if (strcasecmp(tok, "can") == 0)        cmd_can(&save);
    else if (strcasecmp(tok, "pad") == 0) {
        const char* sub = strtok_r(NULL, " \t", &save);
        int pins[8], n = 0; const char* a;
        while (n < 8 && (a = strtok_r(NULL, " \t", &save)) != NULL) pins[n++] = atoi(a);
        if (sub && strcasecmp(sub, "test") == 0 && n > 0) {
            esp_err_t e = mic_service_pad_test(pins, n, [](const char* l) { R("%s", l); });
            R("[cmd] %s: pad test", e == ESP_OK ? "OK" : esp_err_to_name(e));
        } else R("[cmd] ERR: pad test <gpio> [gpio...]  (en fazla 8; ornek: pad test 41 40 15 16)");
    }
    else if (strcasecmp(tok, "mem") == 0) {
        /* 2026-11-09 bellek teşhisi: PSRAM 0x3c2f86a0'da her açılışta aynı tek bit düşüyor (heap zehirleme yakaladı). */
        const char* sub = strtok_r(NULL, " \t", &save);
        const char* a1  = strtok_r(NULL, " \t", &save);
        const char* a2  = strtok_r(NULL, " \t", &save);
        auto in_ram = [](uintptr_t a) { return esp_ptr_external_ram((void*)a) || esp_ptr_internal((void*)a); };
        if (!sub || strcasecmp(sub, "info") == 0) {
            bool ok = heap_caps_check_integrity_all(true);
            R("[cmd] mem: dahili serbest %u (en buyuk %u), PSRAM serbest %u (en buyuk %u), butunluk %s",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), ok ? "OK" : "BOZUK (CORRUPT HEAP satirina bak)");
        } else if (strcasecmp(sub, "peek") == 0 && a1) {
            uintptr_t a = (uintptr_t)strtoul(a1, NULL, 16) & ~3u; int n = a2 ? atoi(a2) : 8; if (n < 1) n = 1; if (n > 32) n = 32;
            if (!in_ram(a) || !in_ram(a + n * 4 - 4)) { R("[cmd] ERR: adres RAM disinda"); goto mem_done; }
            char line[160]; int off = snprintf(line, sizeof line, "[cmd] %08x:", (unsigned)a);
            for (int i = 0; i < n; i++) { off += snprintf(line + off, sizeof line - off, " %08x", (unsigned)*(volatile uint32_t*)(a + 4 * i)); if (off > 140 || i == n - 1 || (i % 8) == 7) { R("%s", line); off = snprintf(line, sizeof line, "[cmd] %08x:", (unsigned)(a + 4 * (i + 1))); } }
        } else if (strcasecmp(sub, "bit") == 0 && a1) {
            uintptr_t a = (uintptr_t)strtoul(a1, NULL, 16) & ~3u; int tur = a2 ? atoi(a2) : 1000; if (tur < 1) tur = 1; if (tur > 100000) tur = 100000;
            if (!in_ram(a)) { R("[cmd] ERR: adres RAM disinda"); goto mem_done; }
            volatile uint32_t* p = (volatile uint32_t*)a; uint32_t orig = *p;
            const uint32_t pat[4] = { 0xFFFFFFFFu, 0x00000000u, 0xAAAAAAAAu, 0x55555555u }; int bad[4] = {0, 0, 0, 0}; uint32_t ilk[4] = {0, 0, 0, 0};
            for (int t = 0; t < tur; t++) for (int k = 0; k < 4; k++) { *p = pat[k]; uint32_t g = *p; if (g != pat[k]) { if (!bad[k]) ilk[k] = g; bad[k]++; } }
            /* bekletmeli okuma: 200 ms sonra hala tutuyor mu (tutma/sizinti) */
            int bekle_bad = 0; uint32_t bekle_got = 0;
            for (int k = 0; k < 4; k++) { *p = pat[k]; vTaskDelay(pdMS_TO_TICKS(200)); uint32_t g = *p; if (g != pat[k]) { bekle_bad++; bekle_got = g; } }
            *p = orig;
            R("[cmd] mem bit %08x (%s): ilk deger %08x; %d turda hata FF:%d 00:%d AA:%d 55:%d%s%s", (unsigned)a, esp_ptr_external_ram((void*)a) ? "PSRAM" : "dahili", (unsigned)orig, tur,
              bad[0], bad[1], bad[2], bad[3], bekle_bad ? "; 200 ms bekletmede HATA, okunan " : "; 200 ms bekletme OK", bekle_bad ? "" : "");
            if (bekle_bad) R("[cmd]   bekletme sonrasi okunan: %08x", (unsigned)bekle_got);
            for (int k = 0; k < 4; k++) if (bad[k]) R("[cmd]   desen %08x -> ilk hatali okuma %08x", (unsigned)pat[k], (unsigned)ilk[k]);
        } else if (strcasecmp(sub, "psram") == 0) {
            int bekle = a1 ? atoi(a1) : 2; if (bekle < 0) bekle = 0; if (bekle > 60) bekle = 60;
            /* PSRAM'in boş kısmını büyük bloklar hâlinde al, adrese bağlı desen yaz, bekle, doğrula; ters desenle tekrar. */
            enum { MAXB = 64 }; uint32_t* blk[MAXB]; size_t len[MAXB]; int nb = 0; size_t toplam = 0;
            while (nb < MAXB) { size_t lf = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); if (lf < 65536) break; size_t want = lf - 4096; uint32_t* b = (uint32_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM); if (!b) break; blk[nb] = b; len[nb] = want / 4; toplam += want; nb++; }
            R("[cmd] mem psram: %d blok, %u KB test ediliyor, bekleme %d sn (deger-adres deseni, sonra tersi)", nb, (unsigned)(toplam / 1024), bekle);
            uint32_t hata = 0; char ilk[5][48]; int nilk = 0; int64_t t0 = esp_timer_get_time();
            for (int pass = 0; pass < 2; pass++) {
                for (int i = 0; i < nb; i++) for (size_t j = 0; j < len[i]; j++) { uint32_t v = ((uint32_t)(uintptr_t)&blk[i][j]) ^ 0xA5A5A5A5u; blk[i][j] = pass ? ~v : v; }
                if (bekle) vTaskDelay(pdMS_TO_TICKS(bekle * 1000));
                for (int i = 0; i < nb; i++) for (size_t j = 0; j < len[i]; j++) {
                    uint32_t v = ((uint32_t)(uintptr_t)&blk[i][j]) ^ 0xA5A5A5A5u; if (pass) v = ~v; uint32_t g = blk[i][j];
                    if (g != v) { hata++; if (nilk < 5) { snprintf(ilk[nilk], sizeof ilk[nilk], "%08x bekl %08x oku %08x", (unsigned)(uintptr_t)&blk[i][j], (unsigned)v, (unsigned)g); nilk++; } }
                }
            }
            for (int i = 0; i < nb; i++) free(blk[i]);
            R("[cmd] mem psram: %lu hatali kelime / %u KB, %lld ms%s", (unsigned long)hata, (unsigned)(toplam / 1024), (long long)((esp_timer_get_time() - t0) / 1000), hata ? "" : " - TEMIZ");
            for (int i = 0; i < nilk; i++) R("[cmd]   %s", ilk[i]);
        } else R("[cmd] ERR: mem | mem peek <hex> [n] | mem bit <hex> [tur] | mem psram [bekle_sn]");
        mem_done: ;
    }
    else if (strcasecmp(tok, "flashtest") == 0) {
        /* assets bölümünün son sektöründe yaz/oku sınaması (NAR). Sektör sonunda silinir. */
        const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "assets");
        static uint8_t w[256], r[256];
        if (!p) R("[cmd] ERR: flashtest assets yok");
        else {
            const uint32_t off = p->size - 4096;
            for (int i = 0; i < 256; i++) w[i] = (uint8_t)(i * 7 + 0x5A);
            esp_err_t e1 = esp_partition_erase_range(p, off, 4096);
            esp_err_t e2 = e1 == ESP_OK ? esp_partition_write(p, off, w, sizeof w) : e1;
            esp_err_t e3 = esp_partition_read(p, off, r, sizeof r);
            bool ok = e2 == ESP_OK && e3 == ESP_OK && memcmp(w, r, sizeof w) == 0;
            esp_partition_erase_range(p, off, 4096);
            R("[cmd] flashtest assets=0x%lx/%lu KB yazma=%s geri_okuma=%s", (unsigned long)p->address, (unsigned long)(p->size / 1024), esp_err_to_name(e2), ok ? "OK" : "HATA");
        }
    }
    else if (strcasecmp(tok, "mic") == 0) {
        const char* sub = strtok_r(NULL, " \t", &save);
        /* `mic etiket <metin>`: satırın kalanı boşluklarıyla birlikte etiket olur (argümanlara bölünmez) */
        const bool  etk = sub && strcasecmp(sub, "etiket") == 0;
        const char* etk_metin = etk ? strtok_r(NULL, "", &save) : NULL;
        const char* a1  = etk ? NULL : strtok_r(NULL, " \t", &save);
        const char* a2  = etk ? NULL : strtok_r(NULL, " \t", &save);
        const char* a3  = etk ? NULL : strtok_r(NULL, " \t", &save);
        const char* a4  = etk ? NULL : strtok_r(NULL, " \t", &save);   /* kodek varyantı 0|1|2 (teşhis) */
        if (etk) {
            mic_service_set_label(etk_metin);
            R("[cmd] OK: mic etiket '%s'", mic_service_get_label());
        } else if (sub && strcasecmp(sub, "clocks") == 0) {
            esp_err_t e = mic_service_clock_test([](const char* l) { R("%s", l); });
            R("[cmd] %s: mic clocks", e == ESP_OK ? "OK" : esp_err_to_name(e));
        } else if (sub && strcasecmp(sub, "capture") == 0) {
            int sec = a1 ? atoi(a1) : 10, hz = a2 ? atoi(a2) : 1000, ch = a3 ? atoi(a3) : 1;
            mic_service_set_variant(a4 ? atoi(a4) : 0);
            R("[cmd] mic capture basliyor: %d sn, %d Hz, kanal %d", sec, hz, ch);
            esp_err_t e = mic_service_capture(sec, hz, ch, [](const char* l) { R("%s", l); });
            if (e == ESP_OK) R("[cmd] OK: mic capture bitti");
            else R("[cmd] ERR: mic capture %s", esp_err_to_name(e));
        } else if (sub && strcasecmp(sub, "raw") == 0) {
            esp_err_t e = mic_service_raw(a1 ? atoi(a1) : 10, a2 ? atoi(a2) : 3, a3 ? atoi(a3) : 24, a4 ? atoi(a4) : 0, [](const char* l) { R("%s", l); });
            if (e != ESP_OK) R("[cmd] ERR: mic raw %s", esp_err_to_name(e));
        } else if (sub && strcasecmp(sub, "sdrec") == 0) {
            esp_err_t e = mic_service_sdrec(a1 ? atoi(a1) : 10, a2 ? atoi(a2) : 3, a3 ? atoi(a3) : 24, a4 ? atoi(a4) : 0, [](const char* l) { R("%s", l); });
            if (e == ESP_OK) R("[cmd] OK: mic sdrec bitti");
            else R("[cmd] ERR: mic sdrec %s", e == ESP_ERR_NOT_FOUND ? "SD kart yok ya da FAT degil" : esp_err_to_name(e));
        } else if (sub && strcasecmp(sub, "nabiz") == 0) {
            /* mic nabiz [sn] [kanal=3] [bit=24] [varyant=0] [insan|kopek|kedi] [usb] [nosd]
             * Sayısal argümanlar sırayla, sözcükler herhangi bir yerde. sn verilmezse profilin süresi (insan 15, hayvan 30).
             * Ham WAV varsayılan olarak SD'ye yazılır (kart yoksa ölçüm sürer); "nosd" kapatır, "sd" eski sözdizimi (etkisiz). */
            const char* a5 = strtok_r(NULL, " \t", &save);
            const char* a6 = strtok_r(NULL, " \t", &save);
            const char* a7 = strtok_r(NULL, " \t", &save);
            const char* args[7] = { a1, a2, a3, a4, a5, a6, a7 };
            bool sd = true, usb = false; int prof = 0; int num[4] = { 0, 3, 24, 0 }; int ni = 0; const char* bilinmeyen = NULL;
            for (int i = 0; i < 7 && args[i]; i++) {
                int pi = mic_service_profil_bul(args[i]);
                if (pi >= 0) prof = pi;
                else if (strcasecmp(args[i], "sd") == 0) sd = true;
                else if (strcasecmp(args[i], "nosd") == 0) sd = false;
                else if (strcasecmp(args[i], "usb") == 0) usb = true;
                else if ((args[i][0] >= '0' && args[i][0] <= '9') && ni < 4) num[ni++] = atoi(args[i]);
                else bilinmeyen = args[i];
            }
            if (bilinmeyen) { R("[cmd] ERR: mic nabiz: bilinmeyen secenek '%s' (insan|kopek|kedi|usb|nosd)", bilinmeyen); }
            else {
                int sec = num[0] > 0 ? num[0] : mic_service_profil_sure(prof), ch = num[1] > 0 ? num[1] : 3, bits = num[2] == 16 ? 16 : 24, var = num[3];
                R("[cmd] mic nabiz: %d sn dinleniyor (profil %s, kanal %d, %d bit, varyant %d%s%s)...", sec, mic_service_profil_ad(prof), ch, bits, var, sd ? ", SD" : "", usb ? ", USB akisi" : "");
                esp_err_t e = mic_service_nabiz(sec, ch, bits, var, sd, usb, prof, [](const char* l) { R("%s", l); });
                if (e == ESP_OK) R("[cmd] OK: mic nabiz bitti");
                else R("[cmd] ERR: mic nabiz %s", esp_err_to_name(e));
            }
        } else if (sub && strcasecmp(sub, "sdls") == 0) {
            esp_err_t e = mic_service_sd_list([](const char* l) { R("%s", l); });
            R("[cmd] %s: mic sdls", e == ESP_OK ? "OK" : "ERR");
        } else R("[cmd] ERR: mic capture [sn] [hz] [kanal] | mic raw|sdrec [sn] [kanal] [16|24] [varyant] | mic nabiz [sn] [kanal] [bit] [varyant] [insan|kopek|kedi] [usb] [nosd] | mic etiket <metin> | mic sdls | mic clocks");
    }
    else if (strcasecmp(tok, "imu") == 0)        cmd_imu(&save);
    else if (strcasecmp(tok, "telemetry") == 0) {
        const char* sub = strtok_r(NULL, " \t", &save);
        if (sub && strcasecmp(sub, "send") == 0) {
            esp_err_t e = telemetry_service_send_now();
            if (e == ESP_OK) R("[cmd] OK: telemetry sent (http=%d)", telemetry_service_last_http());
            else R("[cmd] ERR: telemetry send failed (%s — WiFi bagli mi?)", esp_err_to_name(e));
        } else R("[cmd] ERR: telemetry send");
    }
    else if (strcasecmp(tok, "tracker") == 0) {
        const char* sub = strtok_r(NULL, " \t", &save);
        const char* m   = strtok_r(NULL, " \t", &save);
        if (sub && strcasecmp(sub, "mode") == 0 && m) {
            if      (strcasecmp(m, "auto") == 0)   telemetry_service_force_mode(-1);
            else if (strcasecmp(m, "active") == 0) telemetry_service_force_mode(1);
            else if (strcasecmp(m, "rest") == 0)   telemetry_service_force_mode(0);
            else { R("[cmd] ERR: tracker mode auto|active|rest"); goto done; }
            R("[cmd] OK: tracker mode %s", m);
        } else R("[cmd] ERR: tracker mode auto|active|rest");
        done: ;
    }
    else if (strcasecmp(tok, "activity") == 0) {
        const char* sub = strtok_r(NULL, " \t", &save);
        const char* st  = strtok_r(NULL, " \t", &save);
        if (sub && strcasecmp(sub, "sim") == 0 && st) {
            if      (strcasecmp(st, "off") == 0)  { activity_service_simulate_off(); R("[cmd] OK: activity sim off"); }
            else if (strcasecmp(st, "rest") == 0) { activity_service_simulate(ACT_REST); R("[cmd] OK: activity sim rest"); }
            else if (strcasecmp(st, "walk") == 0) { activity_service_simulate(ACT_WALK); R("[cmd] OK: activity sim walk"); }
            else if (strcasecmp(st, "run") == 0)  { activity_service_simulate(ACT_RUN);  R("[cmd] OK: activity sim run"); }
            else R("[cmd] ERR: activity sim rest|walk|run|off");
        } else R("[cmd] ERR: activity sim rest|walk|run|off");
    }
    else if (strcasecmp(tok, "piezo") == 0) {
        const char* sub = strtok_r(NULL, " \t", &save);
        const char* a1  = strtok_r(NULL, " \t", &save);
        const char* a2  = strtok_r(NULL, " \t", &save);
        const char* a3  = strtok_r(NULL, " \t", &save);
        if (sub && strcasecmp(sub, "capture") == 0) {
            int sec = a1 ? atoi(a1) : 10, hz = a2 ? atoi(a2) : 200;
            int pull = a3 ? (strcasecmp(a3, "pd") == 0 ? -1 : strcasecmp(a3, "pu") == 0 ? 1 : 0) : 0;
            R("[cmd] piezo capture basliyor: %d sn @ %d Hz pull=%s (modul anahtari A olmali)", sec, hz,
              pull < 0 ? "down" : pull > 0 ? "up" : "yok");
            esp_err_t e = vibration_service_capture(sec, hz, [](const char* l) { R("%s", l); }, pull);
            if (e == ESP_OK) R("[cmd] OK: piezo capture bitti, dijital darbe moduna donuldu");
            else R("[cmd] ERR: piezo capture %s%s", esp_err_to_name(e),
                   e == ESP_ERR_INVALID_STATE ? " (pin henuz hazir degil - acilistan ~10 sn sonra tekrar dene)" : "");
        } else R("[cmd] ERR: piezo capture [sn=10] [hz=200] [pd|pu]");
    }
    else if (strcasecmp(tok, "gps") == 0) {
        const char* sub = strtok_r(NULL, " \t", &save);
        if (sub && strcasecmp(sub, "sim") == 0) {
            const char* a = strtok_r(NULL, " \t", &save);
            if (a && strcasecmp(a, "off") == 0) { gps_service_simulate_off(); R("[cmd] OK: gps sim off"); }
            else {
                const char* b = strtok_r(NULL, " \t", &save);
                const char* h = strtok_r(NULL, " \t", &save);
                if (!a || !b) { R("[cmd] ERR: gps sim <lat> <lng> [hdop] | gps sim off"); }
                else {
                    double lat = atof(a), lng = atof(b);
                    if (fabs(lat) > 90 || fabs(lng) > 180) { R("[cmd] ERR: lat/lng araligi"); }
                    else { gps_service_simulate(lat, lng, h ? (float)atof(h) : 1.0f);
                           R("[cmd] OK: gps sim %.6f,%.6f (telemetry send ile gonder)", lat, lng); }
                }
            }
        } else R("[cmd] ERR: gps sim <lat> <lng> [hdop] | gps sim off");
    }
    else if (strcasecmp(tok, "toggle") == 0)     cmd_toggle(strtok_r(NULL, " \t", &save));
    else if (strcasecmp(tok, "wake") == 0) {
        power_service_user_activity(POWER_WAKE_SRC_BTN_TOP);
        R("[cmd] OK: wake");
    }
    else if (strcasecmp(tok, "reboot") == 0) {
        R("[cmd] OK: rebooting...");
        vTaskDelay(pdMS_TO_TICKS(200));   /* yanıt kanaldan çıksın */
        esp_restart();
    }
    else R("[cmd] ERR: unknown command '%s' (help)", tok);
}

static char s_last_result[160] = "-";
void command_service_set_result(const char* text)
{
    if (!text) { strlcpy(s_last_result, "-", sizeof s_last_result); return; }
    strlcpy(s_last_result, text, sizeof s_last_result);
}
const char* command_service_get_result(void) { return s_last_result; }

void command_service_exec_ex(char* line, cmd_reply_fn_t reply, void* ctx)
{
    if (!line) return;
    if (!s_exec_mutex) s_exec_mutex = xSemaphoreCreateMutex();
    if (s_exec_mutex) xSemaphoreTake(s_exec_mutex, portMAX_DELAY);

    s_reply     = reply;
    s_reply_ctx = ctx;
    exec_locked(line);
    s_reply     = NULL;
    s_reply_ctx = NULL;

    if (s_exec_mutex) xSemaphoreGive(s_exec_mutex);
}

void command_service_exec(char* line)
{
    command_service_exec_ex(line, serial_reply, NULL);
}

/* ── UART okuyucu görevi ────────────────────────────────────────────────── */
static void command_task(void* arg)
{
    (void)arg;
    static char line[CMD_LINE_MAX];
    size_t pos = 0;

    for (;;) {
        uint8_t ch;
#ifdef CMD_USE_USB_JTAG
        int r = usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY);
#else
        int r = uart_read_bytes(CMD_UART, &ch, 1, portMAX_DELAY);
#endif
        if (r <= 0) continue;

        if (ch == '\n' || ch == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                command_service_exec(line);
                pos = 0;
            }
        } else if (pos < CMD_LINE_MAX - 1) {
            line[pos++] = (char)ch;
        } else {
            pos = 0;   /* taşan satırı at */
            printf("[cmd] ERR: line too long\n");
        }
    }
}

esp_err_t command_service_init(void)
{
    if (!s_exec_mutex) s_exec_mutex = xSemaphoreCreateMutex();

    /* RX icin surucu kur — TX buffer 0: log/printf yolu degismez.
     * Konsol UART'ı boot'ta zaten konfigure (115200); yeniden ayarlamiyoruz. */
#ifdef CMD_USE_USB_JTAG
    usb_serial_jtag_driver_config_t jcfg = { .tx_buffer_size = 4096, .rx_buffer_size = 512 };   /* mic raw: 144 kB/s */
    esp_err_t err = usb_serial_jtag_driver_install(&jcfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
#else
    esp_err_t err = uart_driver_install(CMD_UART, 512, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
#endif

    BaseType_t r = xTaskCreatePinnedToCore(
        command_task, "cmd_svc",
        8192 /* 2026-11-09: SD'ye WAV yazımı (FATFS) + nabız analizi bu görevde; 4 KB dardı */, NULL, 3, NULL, 1 /* Core 1 — UI'yi bloklamaz */);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "task spawn failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "init OK (USB serial, satır tabanlı; 'help' yazın)");
    return ESP_OK;
}

#endif /* TRACKER_FEATURES */
