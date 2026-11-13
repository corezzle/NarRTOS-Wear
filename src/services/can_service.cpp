/**
 * @file can_service.cpp
 * @brief Kimlik başına en son frame tablosu (bkz. header). NarRTOS-Drive'dan taşındı.
 *
 * Tasarım notları:
 *  - Doğrusal arama: tablo 96 girdi, tipik araç 30-80 kimlik yayınlar; hash
 *    tablosu bu boyutta kendini amorti etmiyor.
 *  - Mutex: TWAI görevi (çekirdek 1, öncelik 6) ile komut/telemetry görevleri
 *    aynı anda erişebilir.
 *  - Tablo dolunca YENİ kimlik atılır, mevcutlar korunur; en eskiyi atmak nadir ama
 *    önemli bir PGN'i (arıza kodu) sürekli yayın yapan bir PGN uğruna düşürürdü.
 *    Atılan sayılır (`can stats` → dropped); artıyorsa CAN_ID_TABLE_SIZE büyütülür.
 */
#ifdef TRACKER_FEATURES
#include "can_service.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "CanSvc";
#define CAN_NVS_NS   "can"
#define CAN_NVS_KBPS "kbps"

static can_frame_t       s_tbl[CAN_ID_TABLE_SIZE];
static uint16_t          s_used  = 0;
static uint32_t          s_rx    = 0;
static uint32_t          s_drop  = 0;
static uint32_t          s_kbps  = CAN_BITRATE_DEFAULT_KBPS;
static SemaphoreHandle_t s_mutex = NULL;

esp_err_t can_service_init(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    memset(s_tbl, 0, sizeof(s_tbl));
    s_used = 0; s_rx = 0; s_drop = 0;

    nvs_handle_t h;
    if (nvs_open(CAN_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v = 0;
        if (nvs_get_u32(h, CAN_NVS_KBPS, &v) == ESP_OK &&
            (v == 125 || v == 250 || v == 500 || v == 1000)) s_kbps = v;
        nvs_close(h);
    }
    ESP_LOGI(TAG, "init OK (tablo %d kimlik, kimlik basina son frame, %lu kbit/s, TWAI=%s)",
             CAN_ID_TABLE_SIZE, (unsigned long)s_kbps, ENABLE_CAN ? "derli" : "yok - `can inject`");
    return ESP_OK;
}

void can_service_ingest(uint32_t id, bool ext, const uint8_t* data, uint8_t dlc)
{
    if (dlc > 8) dlc = 8;
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_rx++;
    can_frame_t* slot = NULL;
    for (uint16_t i = 0; i < s_used; i++)
        if (s_tbl[i].id == id && s_tbl[i].ext == ext) { slot = &s_tbl[i]; break; }
    if (!slot) {
        if (s_used >= CAN_ID_TABLE_SIZE) { s_drop++; xSemaphoreGive(s_mutex); return; }
        slot = &s_tbl[s_used++];
        slot->id = id; slot->ext = ext; slot->count = 0;
    }
    if (dlc && data) memcpy(slot->data, data, dlc);
    slot->dlc     = dlc;
    slot->last_us = esp_timer_get_time();
    slot->count++;
    xSemaphoreGive(s_mutex);
}

size_t can_service_snapshot(can_frame_t* out, size_t max)
{
    if (!out || !max || !s_mutex) return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = (s_used < max) ? s_used : max;
    bool taken[CAN_ID_TABLE_SIZE] = {false};
    for (size_t k = 0; k < n; k++) {           /* en taze kimlikten başla */
        int best = -1;
        for (uint16_t i = 0; i < s_used; i++) {
            if (taken[i]) continue;
            if (best < 0 || s_tbl[i].last_us > s_tbl[best].last_us) best = i;
        }
        if (best < 0) { n = k; break; }
        taken[best] = true; out[k] = s_tbl[best];
    }
    xSemaphoreGive(s_mutex);
    return n;
}

void can_service_stats(can_stats_t* out)
{
    if (!out) return;
    if (!s_mutex) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out->rx_total = s_rx; out->dropped_full = s_drop; out->distinct = s_used; out->capacity = CAN_ID_TABLE_SIZE;
    xSemaphoreGive(s_mutex);
}

void can_service_clear(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_tbl, 0, sizeof(s_tbl)); s_used = 0; s_rx = 0; s_drop = 0;
    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "tablo bosaltildi");
}

size_t can_service_format_frame(const can_frame_t* f, char* out, size_t cap)
{
    if (!f || !out || cap < 12) return 0;
    /* Kimlik HER ZAMAN 8 hane: sunucu sabit konumdan keser, hane sayısı değişirse sessizce yanlış eşleşir. */
    int n = snprintf(out, cap, "0x%08lX", (unsigned long)f->id);
    if (n <= 0 || (size_t)n >= cap) return 0;
    for (uint8_t i = 0; i < f->dlc && i < 8; i++) {
        int m = snprintf(out + n, cap - n, ",%02X", f->data[i]);
        if (m <= 0 || (size_t)m >= cap - n) return 0;
        n += m;
    }
    return (size_t)n;
}

size_t can_service_json(char* out, size_t cap)
{
    static can_frame_t snap[CAN_FRAMES_PER_REPORT];   /* telemetri görevi yığınını şişirmesin */
    can_stats_t st; can_service_stats(&st);
    if (st.distinct == 0 || !out || cap < 64) return 0;
    size_t n = can_service_snapshot(snap, CAN_FRAMES_PER_REPORT);
    int w = snprintf(out, cap, "\"can\":{\"rx_total\":%lu,\"distinct\":%u,\"dropped\":%lu,\"frames\":[",
                     (unsigned long)st.rx_total, (unsigned)st.distinct, (unsigned long)st.dropped_full);
    if (w <= 0 || (size_t)w >= cap) return 0;
    for (size_t i = 0; i < n; i++) {
        if (cap - w < 48) break;                       /* sığmıyorsa en taze olanlar gitti, gerisi kesilir */
        if (i) out[w++] = ',';
        out[w++] = '"';
        size_t m = can_service_format_frame(&snap[i], out + w, cap - w);
        if (!m) { w -= (i ? 2 : 1); break; }
        w += (int)m; out[w++] = '"';
    }
    if (cap - w < 3) return 0;
    out[w++] = ']'; out[w++] = '}'; out[w] = 0;
    return (size_t)w;
}

uint32_t can_service_bitrate(void) { return s_kbps; }

esp_err_t can_service_set_bitrate(uint32_t kbps)
{
    if (kbps != 125 && kbps != 250 && kbps != 500 && kbps != 1000) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    if (nvs_open(CAN_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t e = nvs_set_u32(h, CAN_NVS_KBPS, kbps);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) s_kbps = kbps;
    return e;
}
#endif /* TRACKER_FEATURES */
