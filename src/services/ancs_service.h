#pragma once
/**
 * @file ancs_service.h
 * @brief Apple Notification Center Service (ANCS) client.
 *
 * Apple spec: https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/
 *
 * Pipeline:
 *   1. ble_service connected + bonded → ancs_service_on_connected çağrılır
 *   2. ancs_service iPhone'daki ANCS service'i discover eder (peripheral GATT discovery)
 *   3. Notification Source + Data Source characteristic'lerine subscribe olur
 *   4. iPhone'a bildirim gelince Notification Source'tan 8-byte event'i alır
 *   5. Önemli ise (Social/Email/Call) Control Point'e GetNotificationAttributes
 *      command yazarak title, message, app_id gibi detayları çeker
 *   6. Data Source'tan multi-packet response gelir → parse → notification_store
 *
 * UUIDs:
 *   Service:           7905F431-B5CE-4E99-A40F-4B1E122D00D0
 *   NotificationSource: 9FBF120D-6301-42D9-8C58-25E699A21DBD  (notify)
 *   ControlPoint:       69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9  (write)
 *   DataSource:         22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFD  (notify)
 */

#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Init — boot'ta bir kez. ble_service init'ten SONRA çağrılmalı. */
esp_err_t ancs_service_init(void);

/* ble_service'ten gelen callback'ler — ANCS discovery'yi bunlar tetikler. */
void ancs_service_on_connected(uint16_t conn_handle);
void ancs_service_on_disconnected(uint16_t conn_handle);

/* ble_service GAP NOTIFY_RX event'i bunu çağırır (raw notification data
 * forwarding). Hangi karakter olduğunu handle ile ayırt ederiz. */
void ancs_service_on_notify_rx(uint16_t conn_handle, uint16_t attr_handle,
                                const uint8_t* data, uint16_t len);

/* Saatta kullanıcı bir notification'ı dismiss etti — iPhone'a bildir.
 * Control Point'e "PerformNotificationAction(NotificationUID, NegativeAction)"
 * yazılır. iPhone bildirim listesinden kaldırır. */
esp_err_t ancs_service_dismiss(uint32_t notification_uid);

#ifdef __cplusplus
}
#endif
