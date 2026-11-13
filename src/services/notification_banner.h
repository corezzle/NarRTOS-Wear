#pragma once
/**
 * @file notification_banner.h
 * @brief Apple Watch-style notification banner overlay.
 *
 * Yeni bildirim geldiğinde ekran üstünde slide-down overlay gösterir.
 *
 * UX davranışı:
 *   1. Bildirim arrival → ekran wake (power_service)
 *   2. lv_layer_top()'a banner çiz (her app üstünde görünür)
 *   3. App icon + sender + preview, Apple Watch tarzı kompakt format
 *   4. 5 saniye sonra otomatik dismiss (slide-up animasyonu)
 *   5. Tap → Notifications app aç + detail göster
 *   6. Yeni bildirim sırasında varsa eskisini değiştir (replace)
 *
 * Thread safety: notification_store listener BLE task'tan tetiklenir,
 * lv_async_call ile UI thread'e geçer.
 */

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Init — boot'ta bir kez. notification_store_init'ten SONRA çağrılmalı.
 * Banner servisi store'a listener olarak register olur. */
esp_err_t notification_banner_init(void);

/* Şu an gösterilen banner var mı (test/debug için). */
bool notification_banner_is_showing(void);

/* Force dismiss (mesela kullanıcı bir app açtığında). */
void notification_banner_dismiss(void);

#ifdef __cplusplus
}
#endif
