/**
 * @file sensor_service.cpp
 * @brief IMU verilerini işler ve event_bus'a adım/jest/APEX event'leri yayar.
 *
 * Step count: Her 20 polling'de bir (=1 saniye) donanım adım sayacı + aktivite sınıfı okunur;
 * adım değiştiyse EVENT_STEP_COUNT, aktivite sınıfı değiştiyse EVENT_ACTIVITY_CLASS yayımlanır.
 * APEX kesmeleri (2026-11-04): ISR bayrağı her tick'te kontrol edilir; INT_STATUS2/3 görev
 * bağlamında okunur (latched INT1 böylece kalkar) ve olaylara ayrılır. Uyanışta sürücünün
 * yakaladığı olaylar init'te aynı yoldan yayımlanır.
 * NVS: Sayaç güç kesiminde sıfırlandığından NVS'te offset saklanır;
 *      gerçek adım = nvs_offset + hw_steps.
 */

#include "sensor_service.h"
#include "../drivers/imu/icm42670_driver.h"
#include "../core/event_bus.h"

#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char* TAG      = "SensorSvc";
static const char* NVS_NS   = "sensor";
static const char* NVS_KEY  = "steps_base";

static uint32_t s_steps_base   = 0;   // NVS'ten yüklenen offset
static uint32_t s_steps_last   = 0;   // Önceki hw step değeri
static uint8_t  s_poll_counter = 0;   // 20Hz → 1Hz için sayaç
static uint8_t  s_activity     = 0xFF; // son yayımlanan APEX aktivite sınıfı
static uint32_t s_ev_count     = 0;   // toplam APEX kesmesi (teşhis)

// ─── APEX olaylarını event_bus'a dağıt ───────────────────────────────────────

static void publish_apex_events(const imu_apex_events_t* ev, const char* src)
{
    /* 2026-11-06: WOM, eşik aşılan HER örnekte (50 Hz) kesme üretir; her birini yayınlamak
     * event_bus kuyruğunu doldurup ("event dropped, queue full") adım/eğim olaylarını da
     * düşürüyordu (NAR kart 3 testi). WOM yayını 1 s'de bire sınırlı; log yalnız yayınlananlar için. */
    static int64_t s_last_wom_us = 0;
    bool wom_pub = false;
    if (ev->wom) {
        int64_t now = esp_timer_get_time();
        if (now - s_last_wom_us >= 1000000) { s_last_wom_us = now; wom_pub = true; }
    }
    s_ev_count++;
    if (!ev->wom || wom_pub || ev->step || ev->step_ovf || ev->tilt || ev->smd || ev->ff || ev->lowg)
        ESP_LOGI(TAG, "[imu] %s: %s%s%s%s%s%s%s(ff_dur=%u)", src,
                 ev->wom ? "WOM " : "", ev->step ? "ADIM " : "", ev->step_ovf ? "ADIM-TASMA " : "",
                 ev->tilt ? "TILT " : "", ev->smd ? "SMD " : "", ev->ff ? "DUSME " : "",
                 ev->lowg ? "LOW-G " : "", (unsigned)ev->ff_dur);
    if (wom_pub)      event_bus_publish(EVENT_GESTURE_WRIST_UP, ev->wom_axes);  // eski davranış korunur
    if (ev->step)     event_bus_publish(EVENT_STEP_DETECT, 0);
    if (ev->tilt)     event_bus_publish(EVENT_IMU_TILT, 0);
    if (ev->smd)      event_bus_publish(EVENT_IMU_SMD, 0);
    if (ev->ff)       event_bus_publish(EVENT_IMU_FREEFALL, ev->ff_dur);
    if (ev->lowg)     event_bus_publish(EVENT_IMU_LOWG, 0);
    if (ev->step_ovf) s_poll_counter = 19;   // bir sonraki tick'te sayaç taşması işlensin
}

// ─── sensor_service_init ─────────────────────────────────────────────────────

void sensor_service_init(void)
{
    // NVS'ten step offset yükle
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_KEY, &s_steps_base);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Step base loaded from NVS: %lu", (unsigned long)s_steps_base);

    /* Derin uykudan APEX ile uyanıldıysa uyandıran olayı yayımla. */
    imu_apex_events_t wev;
    if (icm42670_wake_events(&wev)) publish_apex_events(&wev, "uyandiran olay");
}

// ─── sensor_service_update ───────────────────────────────────────────────────

void sensor_service_update(void)
{
    /* APEX kesmesi bekliyor mu? (her tick, 20 Hz) */
    if (icm42670_int_take()) {
        imu_apex_events_t ev;
        if (icm42670_apex_events(&ev) == ESP_OK) publish_apex_events(&ev, "kesme");
    }

    // Her 20 çağrıda bir (20Hz × 20 = 1s) adım sayacı + aktivite sınıfını oku
    if (++s_poll_counter < 20) return;
    s_poll_counter = 0;

    imu_apex_data_t d;
    if (icm42670_apex_read(&d) != ESP_OK) return;
    uint32_t hw_steps = d.steps;

    if (d.activity != s_activity) {
        s_activity = d.activity;
        event_bus_publish(EVENT_ACTIVITY_CLASS, d.activity);
    }

    // Donanım sayacı taştı mı? (16-bit max = 65535)
    if (hw_steps < s_steps_last) {
        // Taşma: offset'e ekle
        s_steps_base += 65536;
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            esp_err_t e = nvs_set_u32(h, NVS_KEY, s_steps_base);
            if (e == ESP_OK) {
                e = nvs_commit(h);
                if (e != ESP_OK) ESP_LOGW("Sensor", "NVS commit fail (overflow save): 0x%x", e);
            } else {
                ESP_LOGW("Sensor", "NVS set fail (overflow save): 0x%x", e);
            }
            nvs_close(h);
        }
    }
    s_steps_last = hw_steps;

    uint32_t total = s_steps_base + hw_steps;
    event_bus_publish(EVENT_STEP_COUNT, total);
}

// ─── sensor_service_get_steps ────────────────────────────────────────────────

uint32_t sensor_service_get_steps(void)
{
    return s_steps_base + s_steps_last;
}

uint8_t sensor_service_get_activity(void)
{
    return s_activity == 0xFF ? 0 : s_activity;
}

uint32_t sensor_service_apex_event_count(void)
{
    return s_ev_count;
}
