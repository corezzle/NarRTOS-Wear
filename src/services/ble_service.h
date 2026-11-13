#pragma once
/**
 * @file ble_service.h
 * @brief NimBLE peripheral lifecycle (init, advertise, pair, bond).
 *
 * Sorumluluklar:
 *   - NimBLE stack başlat (host task + controller)
 *   - Device name + IRK + GAP advertise config
 *   - LE Secure Connections pairing + bonding (NVS-backed)
 *   - Bağlantı state machine: ADV → CONNECTED → BONDED → IDLE
 *   - Connection lost → reconnect (advertise again)
 *
 * ANCS service tarafından kullanılır — bu modül sadece transport.
 *
 * Power: BLE tipik tüketim ~5-10 mA. Connection interval optimize edilmeli
 * (ANCS için 1000ms slow interval yeterli — bildirim event-driven).
 */

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_STATE_OFF        = 0,
    BLE_STATE_INIT       = 1,
    BLE_STATE_ADVERTISING= 2,
    BLE_STATE_CONNECTED  = 3,   /* link up, pairing/bonding pending */
    BLE_STATE_BONDED     = 4,   /* fully ready, ANCS subscriptions yapılabilir */
    BLE_STATE_ERROR      = 5,
} ble_state_t;

/* State değişim callback'i — UI Settings/ControlCenter için */
typedef void (*ble_state_cb_t)(ble_state_t new_state, uint16_t conn_handle, void* user_data);

/* GATT discovery / characteristic ready callback — ANCS service init için */
typedef void (*ble_connected_cb_t)(uint16_t conn_handle, void* user_data);
typedef void (*ble_disconnected_cb_t)(uint16_t conn_handle, void* user_data);

/* ─── Lifecycle ─────────────────────────────────────────────────────────────── */

/* Init NimBLE stack + start host task. Boot'ta bir kez çağrılır.
 * Returns ESP_OK on success. */
esp_err_t ble_service_init(void);

/* Advertise başlat (eşleştirilebilir olarak görün). Telefon scan ettiğinde
 * "NarRTOS Watch" görünür. */
esp_err_t ble_service_start_advertising(void);

/* Advertise durdur (idle mode / power save). */
esp_err_t ble_service_stop_advertising(void);

/* Bağlı cihazı zorla disconnect et. */
esp_err_t ble_service_disconnect(void);

/* Tüm bond'ları sil (factory reset için). */
esp_err_t ble_service_clear_bonds(void);

/* ─── State + callbacks ────────────────────────────────────────────────────── */

ble_state_t ble_service_get_state(void);
uint16_t    ble_service_get_conn_handle(void);   /* 0xFFFF = no connection */

void ble_service_set_state_cb(ble_state_cb_t cb, void* user_data);
void ble_service_set_connection_cb(ble_connected_cb_t   on_connect,
                                    ble_disconnected_cb_t on_disconnect,
                                    void* user_data);

/* Bağlı cihazın görünür adı (eğer pairing sırasında okunduysa).
 * NULL = bilinmiyor. Pointer kısa ömürlü — UI hemen kopyalasın. */
const char* ble_service_get_peer_name(void);

#ifdef __cplusplus
}
#endif
