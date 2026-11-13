#pragma once
/**
 * @file notifications_app.h
 * @brief Notifications app — telefon bildirimleri (ANCS via BLE).
 *
 * UI:
 *   - Liste view: son N bildirim (yeniden eskiye), her satır app icon + sender + preview
 *   - Detail view: tam mesaj, dismiss butonu
 *   - Boş state: "No new notifications" + BLE durum
 *
 * Notification store listener'ı LVGL'e lv_async_call ile rebuild tetikler.
 */

#include "../../core/app_manager.h"

const app_interface_t* notifications_app_get_interface(void);
