#pragma once
/**
 * @brief TWAI (CAN 2.0B) sürücüsü — ESP32-S3'ün dahili denetleyicisi. NarRTOS-Drive'dan taşındı.
 *
 * ⚠ Dışarıdan hat sürücüsü (transceiver) ŞART: SN65HVD230 / TJA1050 vb. ESP32-S3'te
 * denetleyici var ama fiziksel katman yok; çıplak bağlarsan ne hat çalışır ne pin sağ kalır.
 *
 * Sürücü yalnız ALIR (TX kuyruğu 0): takip cihazının aracın hattına frame basması
 * gereksiz ve riskli. Alınan her frame can_service_ingest()'e verilir; çözüm yok.
 *
 * ENABLE_CAN=0 iken (varsayılan) bu dosya boş derlenir; can_service `can inject` ile
 * beslenir. ENABLE_CAN=1 için PIN_CAN_TX/RX build flag'leri verilmelidir.
 *
 * NAR tasması: padler GPIO4 (TX) / GPIO5 (RX) = ekran QSPI D0/D1. Kurulum ayrı bir
 * görevde ekran init'i bittikten sonra yapılır (can_twai_start hemen döner, `get can`
 * birkaç sn sonra twai=running gösterir).
 */
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** TWAI'yi kur ve alıcı görevini başlat. @param kbps 125|250|500|1000
 *  @return ESP_ERR_NOT_SUPPORTED — ENABLE_CAN=0 ile derlendiyse */
esp_err_t can_twai_start(uint32_t kbps);
esp_err_t can_twai_stop(void);
bool      can_twai_running(void);
/** 0=stopped 1=running 2=bus-off 3=recovering, -1=sürücü yok */
int       can_twai_bus_state(void);

#ifdef __cplusplus
}
#endif
