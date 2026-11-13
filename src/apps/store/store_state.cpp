/**
 * @file store_state.cpp
 * @brief Store catalog data + NVS install state — v1.2.6 (2026-05-21).
 */

#include "store_state.h"
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>
#include <stdio.h>

static const char* TAG = "StoreState";

#define STORE_NVS_NS  "store"
#define STORE_KEY_FMT "inst_%d"   /* inst_<app_id> — uint8_t (0/1) */

// ─── Catalog ─────────────────────────────────────────────────────────────────
//
// Tek static array. System app'ler (is_system=true) her zaman installed
// kabul edilir, NVS yazımı yapılmaz. Downloadable'lar (is_system=false) NVS
// flag ile yönetilir.
//
// v1.2.6 ilk downloadable: Step Counter. Binary saatte compile edilmiş
// (APP_ID_STEP_COUNTER zaten enum'da + app_manager_register aktif), sadece
// kullanıcı için "yüklü" gösterilip gösterilmemesi NVS'te.

static const store_app_entry_t s_catalog[] = {
    /* ─── System (silinemez) — 4 app ──────────────────────────────────── */
    { APP_ID_ALARM,        "Alarm",         "Audio & vibration alerts",
      "Set alarms with sound and haptic feedback. Snooze and stop "
      "functionality. Multiple alarm slots supported.",
      LV_SYMBOL_BELL,     0xFF, 0x9F, 0x0A, /*is_system=*/true },

    { APP_ID_WEATHER,      "Weather",       "Multi-city forecast",
      "Current conditions and forecast for multiple cities. Updates "
      "automatically when WiFi is available.",
      LV_SYMBOL_HOME,     0x4F, 0xC3, 0xF7, /*is_system=*/true },

    { APP_ID_GALLERY,      "Gallery",       "Photo browser & faces",
      "Browse uploaded photos and assign them as custom watchfaces. "
      "Photos are stored in the device's internal flash.",
      LV_SYMBOL_IMAGE,    0x4F, 0xE0, 0x9D, /*is_system=*/true },

    { APP_ID_NOTIFY,       "Notifications", "Alerts & messages",
      "View incoming notifications from paired devices and system "
      "alerts. Tap to expand for full message.",
      LV_SYMBOL_ENVELOPE, 0xAF, 0x52, 0xDE, /*is_system=*/true },

    /* ─── Downloadable (mağazadan indirilir gibi) — 3 app ────────────── */
    { APP_ID_STEP_COUNTER, "Step Counter",  "Daily activity tracking",
      "Daily step count and activity history. Powered by the built-in "
      "IMU sensor (ICM42670).",
      LV_SYMBOL_LIST,     0x32, 0xD7, 0x4D, /*is_system=*/false },

    /* v1.4.62 (2026-06-07): Backend integration ile geri eklendi. Backend
     * "heart_rate" entry'sini listeliyor + APP_ID_HEARTRATE binary'si var.
     * Compile-time catalog'da olmazsa click handler "Requires firmware update"
     * modal'ına düşüyordu (badge="Installed" ile çelişki). PPG sensor yok →
     * placeholder UI; gerçek ölçüm v1.3.0+ (hardware upgrade). Açıklama metni
     * demo karakterini açıkça belirtiyor. */
    { APP_ID_HEARTRATE,    "Heart Rate",    "Monitor your pulse",
      "Heart rate tracking demo. NOTE: this watch board has no optical PPG "
      "sensor — readings are simulated for UI demonstration only. Real "
      "measurement requires a hardware upgrade.",
      LV_SYMBOL_GPS,      0xFF, 0x45, 0x5C, /*is_system=*/false },

    { APP_ID_TIMER,        "Timer",         "Countdown timer",
      "Customizable countdown timer with sound and vibration alert "
      "when finished.",
      LV_SYMBOL_LOOP,     0xFF, 0x6B, 0x35, /*is_system=*/false },
};

#define CATALOG_COUNT (sizeof(s_catalog) / sizeof(s_catalog[0]))

// ─── Catalog accessors ──────────────────────────────────────────────────────

size_t store_catalog_count(void)
{
    return CATALOG_COUNT;
}

const store_app_entry_t* store_catalog_at(size_t idx)
{
    if (idx >= CATALOG_COUNT) return NULL;
    return &s_catalog[idx];
}

// ─── NVS-backed install state ───────────────────────────────────────────────

static bool find_entry_is_system(app_id_t id)
{
    for (size_t i = 0; i < CATALOG_COUNT; i++) {
        if (s_catalog[i].id == id) return s_catalog[i].is_system;
    }
    /* Catalog'da olmayan app_id → muhtemelen tamamen sistem (Watchface,
     * Launcher, Settings, Store) — silinemez, hep installed. */
    return true;
}

bool store_is_installed(app_id_t id)
{
    /* System app'ler her zaman installed — NVS okuma yok */
    if (find_entry_is_system(id)) return true;

    /* Downloadable — NVS'ten flag oku. Default: yüklü değil (kullanıcı
     * mağazadan install etmemiş olarak başlasın). */
    nvs_handle_t h;
    if (nvs_open(STORE_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;   /* NVS bozuk veya namespace yok → değil */
    }

    char key[16];
    snprintf(key, sizeof(key), STORE_KEY_FMT, (int)id);

    uint8_t val = 0;
    esp_err_t err = nvs_get_u8(h, key, &val);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_get_u8(%s) → %s", key, esp_err_to_name(err));
        return false;
    }
    return val != 0;
}

void store_set_installed(app_id_t id, bool installed)
{
    /* System app'ler için no-op (default true, yazılmaz) */
    if (find_entry_is_system(id)) {
        ESP_LOGW(TAG, "store_set_installed: id=%d is system, ignored", (int)id);
        return;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(STORE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(%s,RW) → %s", STORE_NVS_NS, esp_err_to_name(err));
        return;
    }

    char key[16];
    snprintf(key, sizeof(key), STORE_KEY_FMT, (int)id);

    err = nvs_set_u8(h, key, installed ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set/commit(%s) → %s", key, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "set installed: id=%d → %s", (int)id, installed ? "true" : "false");
    }
    nvs_close(h);
}

// ─── v1.2.7 (2026-05-22): Backend integration helpers ───────────────────────

/* Backend `app_id` string'leri → APP_ID_* enum mapping. Saatte compile edilmiş
 * binary olmayan string'ler için APP_ID_COUNT döner (UI "Requires firmware
 * update" badge gösterir). Yeni binary firmware'e eklenirse buraya yeni entry
 * ekle. */
typedef struct {
    const char* str;
    app_id_t    id;
} appid_str_map_t;

static const appid_str_map_t s_appid_map[] = {
    /* v1.2.6 sonu — downloadable app'ler */
    { "step_counter",  APP_ID_STEP_COUNTER },
    { "heart_rate",    APP_ID_HEARTRATE    },
    { "timer",         APP_ID_TIMER        },
    /* System app'ler — backend listelemez ama defansif olarak haritada */
    { "alarm",         APP_ID_ALARM        },
    { "weather",       APP_ID_WEATHER      },
    { "gallery",       APP_ID_GALLERY      },
    { "notifications", APP_ID_NOTIFY       },
    { "notify",        APP_ID_NOTIFY       },
};

app_id_t store_appid_from_string(const char* app_id_str)
{
    if (!app_id_str) return APP_ID_COUNT;
    for (size_t i = 0; i < sizeof(s_appid_map)/sizeof(s_appid_map[0]); i++) {
        if (strcmp(s_appid_map[i].str, app_id_str) == 0) {
            return s_appid_map[i].id;
        }
    }
    return APP_ID_COUNT;   /* mapping yok — saatte binary yok */
}

/* LVGL LV_SYMBOL_* macro'ları runtime'da UTF-8 string. Backend "LV_SYMBOL_LIST"
 * gibi macro adı yayınlar; map ile gerçek glyph string'ine çeviririz. */
typedef struct {
    const char* str;
    const char* glyph;
} icon_str_map_t;

static const icon_str_map_t s_icon_map[] = {
    { "LV_SYMBOL_BELL",       LV_SYMBOL_BELL      },
    { "LV_SYMBOL_LOOP",       LV_SYMBOL_LOOP      },
    { "LV_SYMBOL_HOME",       LV_SYMBOL_HOME      },
    { "LV_SYMBOL_LIST",       LV_SYMBOL_LIST      },
    { "LV_SYMBOL_PLUS",       LV_SYMBOL_PLUS      },
    { "LV_SYMBOL_IMAGE",      LV_SYMBOL_IMAGE     },
    { "LV_SYMBOL_ENVELOPE",   LV_SYMBOL_ENVELOPE  },
    { "LV_SYMBOL_AUDIO",      LV_SYMBOL_AUDIO     },
    { "LV_SYMBOL_GPS",        LV_SYMBOL_GPS       },
    { "LV_SYMBOL_DOWNLOAD",   LV_SYMBOL_DOWNLOAD  },
    { "LV_SYMBOL_REFRESH",    LV_SYMBOL_REFRESH   },
    { "LV_SYMBOL_SETTINGS",   LV_SYMBOL_SETTINGS  },
    { "LV_SYMBOL_OK",         LV_SYMBOL_OK        },
    { "LV_SYMBOL_WARNING",    LV_SYMBOL_WARNING   },
    { "LV_SYMBOL_WIFI",       LV_SYMBOL_WIFI      },
    { "LV_SYMBOL_BATTERY_FULL", LV_SYMBOL_BATTERY_FULL },
    { "LV_SYMBOL_CHARGE",     LV_SYMBOL_CHARGE    },
    { "LV_SYMBOL_FILE",       LV_SYMBOL_FILE      },
    { "LV_SYMBOL_KEYBOARD",   LV_SYMBOL_KEYBOARD  },
    { "LV_SYMBOL_TRASH",      LV_SYMBOL_TRASH     },
    { "LV_SYMBOL_EDIT",       LV_SYMBOL_EDIT      },
};

const char* store_icon_from_string(const char* icon_symbol_str)
{
    if (!icon_symbol_str) return "?";
    for (size_t i = 0; i < sizeof(s_icon_map)/sizeof(s_icon_map[0]); i++) {
        if (strcmp(s_icon_map[i].str, icon_symbol_str) == 0) {
            return s_icon_map[i].glyph;
        }
    }
    return LV_SYMBOL_FILE;   /* fallback glyph */
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

bool store_color_from_hex(const char* hex, uint8_t* r, uint8_t* g, uint8_t* b)
{
    if (!hex || !r || !g || !b) return false;
    if (hex[0] == '#') hex++;
    if (strlen(hex) < 6) return false;

    int rh = hex_digit(hex[0]), rl = hex_digit(hex[1]);
    int gh = hex_digit(hex[2]), gl = hex_digit(hex[3]);
    int bh = hex_digit(hex[4]), bl = hex_digit(hex[5]);
    if (rh < 0 || rl < 0 || gh < 0 || gl < 0 || bh < 0 || bl < 0) return false;

    *r = (uint8_t)((rh << 4) | rl);
    *g = (uint8_t)((gh << 4) | gl);
    *b = (uint8_t)((bh << 4) | bl);
    return true;
}
