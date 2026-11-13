#pragma once
/**
 * selftest_ext — ek donanım testleri (LVGL'den ve checklist tablosundan bağımsız).
 *
 * Her test kısa bir ASCII açıklama doldurur ve stx_res_t döner; çağıran taraf
 * bunu kendi tablosuna yazar. Aynı dosya hem narrtos-board-selftest'te hem
 * embedded-wear-os checklist'inde kullanılır (yalnız i2c_bus HAL'ine bağımlı).
 *
 * NAR-ESP32S3'e özgü testler (motor, GSM modülü) kart NAR değilse ATLA döner.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { STX_PASS = 0, STX_FAIL = 1, STX_SKIP = 2, STX_WARN = 3 } stx_res_t;

/** Ayrıntı satırları için yazıcı (printf biçimli). NULL → ayrıntı basılmaz. */
typedef void (*stx_log_fn)(const char* fmt, ...);
void stx_set_logger(stx_log_fn fn);

/** NAR-ESP32S3 kartı mı? (16 MB PSRAM = ESP32-S3R16V SiP) */
bool stx_is_nar(void);

/** UART0 RX baytları GSM modülü testi tarafından tüketilirken true — komut okuyucu yoksaymalı. */
extern volatile bool g_stx_uart_busy;

/** SDA/SCL boşta seviye (ADC). 3.0 V üstü sağlıklı; ~2.0 V = beslemesiz çip hattı kırpıyor. */
stx_res_t stx_i2c_level(char* d, size_t n);

/** ES8311 → NS4150B → (geri besleme) ES7210 MIC3 + akustik MIC1/MIC2.
 *  Tek ölçümde iki satır üretir: hoparlör yolu ve mikrofonlar. ~1 sn, kısa bip sesi çıkar. */
stx_res_t stx_audio(char* d_spk, size_t n_spk, stx_res_t* mic_res, char* d_mic, size_t n_mic);

/** Wi-Fi pasif tarama (verici kapalı). wifi_busy=true → OS Wi-Fi'yi kullanıyor, ATLA. */
stx_res_t stx_wifi(char* d, size_t n, bool wifi_busy);

/** BLE pasif tarama, 3 sn. ble_busy=true → yığın OS'ta çalışıyor, ATLA. */
stx_res_t stx_ble(char* d, size_t n, bool ble_busy);

/** Titreşim motoru (GPIO18) çalıştırılır, ICM42670 ile titreşim ölçülür. Yalnız NAR. */
stx_res_t stx_motor(char* d, size_t n);

/** GSM modülü (şemada NAR-IOT-LTE): ENABLE (GPIO48) + UART0 üzerinden "AT". Yalnız NAR. */
stx_res_t stx_lte(char* d, size_t n);

/** Panel TE darbesi (GPIO13) sayımı, 300 ms. Dönüş: darbe sayısı (TE kapalıysa 0). */
int stx_te_pulses(void);

/** Bilgi satırı: MAC, çip revizyonu, flash voltajı (eFuse), PMIC açılış/kapanış nedeni. */
stx_res_t stx_info(char* d, size_t n);

#ifdef __cplusplus
}
#endif
