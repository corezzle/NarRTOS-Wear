#pragma once
/**
 * @brief Lightweight pub/sub event bus for inter-task communication.
 *
 * Tasks post events (non-blocking from ISR-safe contexts).
 * Subscribers register callbacks that are invoked on the receiving task's context.
 *
 * Design: FreeRTOS queue-based. Each subscriber owns a queue.
 * The bus routes published events to all registered queues.
 */

#include <stdint.h>
#include <freertos/FreeRTOS.h>

// ─── Event Types ─────────────────────────────────────────────────────────────

typedef enum {
    // Power events
    EVENT_BATTERY_LEVEL     = 0x01,  // data: uint8_t percent
    EVENT_CHARGING_STATE    = 0x02,  // data: uint8_t (0=discharging, 1=charging)
    EVENT_LOW_BATTERY       = 0x03,  // data: none → triggers sleep flow

    // Sensor events
    EVENT_STEP_COUNT        = 0x10,  // data: uint32_t total steps
    EVENT_GESTURE_WRIST_UP  = 0x11,  // data: none → wake display
    EVENT_SHAKE             = 0x12,
    // ICM-42670-P APEX olayları (2026-11-04). ISR'den yalnız EVENT_IMU_INT; gerisi sensor_service'ten.
    EVENT_IMU_INT           = 0x13,  // data: none (INT1 kenarı; status görev bağlamında okunur)
    EVENT_IMU_TILT          = 0x14,  // data: none (35° eğim, TILT_WAIT süre)
    EVENT_IMU_SMD           = 0x15,  // data: none (significant motion)
    EVENT_IMU_FREEFALL      = 0x16,  // data: FF_DUR (DMP örnek sayısı)
    EVENT_IMU_LOWG          = 0x17,  // data: none
    EVENT_ACTIVITY_CLASS    = 0x18,  // data: 0 bilinmiyor, 1 yürüme, 2 koşu (APEX_DATA3)
    EVENT_STEP_DETECT       = 0x19,  // data: none (donanım adım kesmesi)

    // UI events
    EVENT_TOUCH_DOWN        = 0x20,  // data: touch_point_t
    EVENT_TOUCH_UP          = 0x21,
    EVENT_BTN_TOP           = 0x22,  // data: uint8_t (press/long-press)
    EVENT_BTN_BOTTOM        = 0x23,

    // System events
    EVENT_TIME_UPDATED      = 0x30,  // data: none
    EVENT_APP_SWITCH        = 0x31,  // data: app_id_t
    EVENT_WEATHER_UPDATE    = 0x32,  // data: uint8_t weather_condition_t

    EVENT_MAX
} event_type_t;

typedef struct {
    event_type_t type;
    uint32_t     data;
    uint32_t     timestamp_ms;
} os_event_t;

// ─── API ──────────────────────────────────────────────────────────────────────

void  event_bus_init(void);
void  event_bus_publish(event_type_t type, uint32_t data);
void  event_bus_publish_from_isr(event_type_t type, uint32_t data);

/** Subscribe: returns a queue handle the caller polls via event_bus_receive() */
QueueHandle_t event_bus_subscribe(event_type_t type);

/** Blocking receive with timeout. Returns false on timeout. */
bool  event_bus_receive(QueueHandle_t queue, os_event_t* out_event, uint32_t timeout_ms);

/** v1.4.55 audit #2c: queue overflow telemetry. Pointer'lar NULL geçilebilir.
 *  ISR drop sayısı normal log basamayacağımız için kritik; publish drop log
 *  üretiyor ama health-check dump için yine de okunabilir. */
void  event_bus_get_drop_stats(uint32_t* out_isr_drops, uint32_t* out_publish_drops);
