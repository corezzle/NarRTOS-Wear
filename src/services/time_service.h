#pragma once

#include <stdint.h>
#include <stdbool.h>
void time_service_init(void);
void time_service_update(void);  // Periyodik RTC → sistem saati senkronizasyonu
bool time_service_sync_from_ntp(uint32_t timeout_ms);

/* Start (or restart) the SNTP background daemon. Returns immediately;
 * the daemon keeps polling and fires the internal sync callback — which
 * persists the corrected time to the RTC — whenever it first succeeds
 * (e.g. after the user connects WiFi later). Call this once on boot,
 * regardless of whether initial WiFi connect succeeded. */
void time_service_ntp_autosync_start(void);

/* Has NTP successfully synced at least once since boot? */
bool time_service_is_ntp_synced(void);

/* Mevcut sistem saatini NVS'e zorla kaydet. power_service deep sleep'e
 * girmeden hemen önce çağırır → uyku boyunca olası bir ray kesintisinde
 * (ör. AXP2101 sleep mode + ALDO1 kesilmesi senaryosunda) bile en taze
 * epoch korunur; uyandığında time_service_init NVS'ten kurtarır.
 * Yıl < 2025 ise (compile fallback'i) NVS'i kirletmemek için kaydetmez. */
void time_service_save_to_nvs(void);
