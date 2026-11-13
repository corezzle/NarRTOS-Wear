#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

void wifi_service_init(void);
esp_err_t wifi_service_connect(uint32_t timeout_ms);
void wifi_service_disconnect(void);
bool wifi_service_is_connected(void);

/* ── Async scan ─────────────────────────────────────────────────────────── */
#define WIFI_SCAN_MAX_APS 8

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secured;
    bool   connected;
} wifi_ap_t;

typedef void (*wifi_scan_cb_t)(const wifi_ap_t* aps, uint8_t count, void* user_data);

/**
 * Start a non-blocking WiFi scan.  Results are delivered via `cb` which is
 * called from a FreeRTOS task (NOT the LVGL task) — use lv_async_call to
 * touch LVGL objects inside the callback.
 * Calling this again while a scan is running replaces the callback.
 */
void wifi_service_scan_async(wifi_scan_cb_t cb, void* user_data);

/** Request the running scan task to abort (callback will NOT be called). */
void wifi_service_scan_cancel(void);

/* ── Synchronous BSSID scan (WiFi geolocation) ──────────────────────────────
 * Telemetri/konum servisi, GPS'siz cihazı konumlandırmak için yakındaki
 * AP'lerin BSSID (MAC) + RSSI listesini ister (backend WiFi geolocation ile
 * lat/lon çözer). Async scan (yukarıda) UI için SSID döndürür; bu BSSID verir.
 * BLOKLAYICI (tarama süresince ~1-3 sn) — yalnızca kendi FreeRTOS task'ından
 * çağır, LVGL task'ından DEĞİL. Async bir scan çalışıyorsa ya da WiFi init
 * değilse ESP_ERR_INVALID_STATE döner; *out_count taranan AP sayısıdır. */
typedef struct {
    uint8_t bssid[6];   /* AP MAC */
    int8_t  rssi;       /* dBm */
    uint8_t channel;
} wifi_bssid_ap_t;

esp_err_t wifi_service_scan_bssids(wifi_bssid_ap_t* out, uint8_t max, uint8_t* out_count);

/* ── Connect to a specific AP ───────────────────────────────────────────── */

/**
 * Result callback.  Called from a FreeRTOS task — use lv_async_call to
 * touch LVGL objects inside.
 */
typedef void (*wifi_connect_result_cb_t)(bool connected, void* user_data);

/**
 * Disconnect any current connection and connect to the given SSID/password.
 * Pass password="" for open networks.
 * cb is called (from a task) with the result after up to 15 s.
 */
void wifi_service_connect_to_ap(const char* ssid, const char* password,
                                 wifi_connect_result_cb_t cb, void* user_data);

/* ── User preference persistence ────────────────────────────────────────────
 * Kullanıcının "WiFi açık/kapalı" tercihi NVS'te kalıcı tutulur. Boot
 * sırasında main.cpp bu tercihi okuyup auto-connect'i yapacağına karar
 * verir; OFF tercihinde reboot/deep-sleep sonrası WiFi auto-on olmaz.
 *
 * Settings WiFi toggle'ı set_user_preference(true/false) çağırır.
 * Default (NVS key yoksa) = true (ilk boot'ta kullanıcı WiFi'nin çalışmasını
 * bekler). */
bool wifi_service_get_user_preference(void);
void wifi_service_set_user_preference(bool enabled);
