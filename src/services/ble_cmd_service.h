#pragma once
/**
 * @brief BLE komut taşıyıcısı — telefondan metin komutlarıyla cihazı sürme.
 *
 * command_service'in (parser çekirdeği) BLE GATT taşıyıcısı. Özel servis:
 *
 *   Service UUID : 4E52-4354 tabanlı 128-bit (aşağıda, "NarRTOS CMD")
 *   RX  char     : WRITE / WRITE_NO_RSP — telefon komut satırını buraya yazar
 *                  (örn. "app launch weather"). Her yazma = bir komut.
 *   TX  char     : NOTIFY — "[cmd] ..." yanıt satırları telefona bildirilir
 *                  (abone olmak opsiyonel; USB loguna da her zaman düşer).
 *   RES char     : READ (…0004) — son ölçüm özeti (ör. "nabiz 79/dk guven 0.85 (yuksek) 14 sn 2026-11-09 16:51");
 *                  command_service_set_result ile güncellenir, tek okuma ile görünür (2026-11-09).
 *
 * Kullanım (özel uygulama gerekmez): nRF Connect → "NarRTOS Watch"a bağlan
 * → NarRTOS CMD servisi → RX'e UTF-8 text yaz → TX notification'larını izle.
 *
 * Kayıt, NimBLE host başlamadan ÖNCE yapılmalı: ble_service_init içinden
 * ble_cmd_gatt_register() çağrılır. LCD128 ve NAR varyantlarında (2026-11-09).
 */

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** GATT servisini kaydet — ble_svc_gatt_init() SONRASI, host task ÖNCESİ. */
esp_err_t ble_cmd_gatt_register(void);

#ifdef __cplusplus
}
#endif
