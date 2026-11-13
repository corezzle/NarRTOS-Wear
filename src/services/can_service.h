#pragma once
/**
 * @file can_service.h
 * @brief Ham CAN frame toplayıcı — kimlik başına EN SON frame. Cihaz çözmez.
 *
 * NarRTOS-Drive'daki (araç takip) kalıbın wear-os'a taşınmış hâli. Amaç: cihaz
 * araç veri yolunu dinler, gördüğü her kimliğin en güncel ham baytlarını
 * telemetriye koyar; DBC/J1939 çözümü SUNUCUDA yapılır.
 * Cihazda hiçbir dönüşüm yok: hex/ölçek/birim çevirisi yapılmaz.
 *
 * Neden FIFO değil, kimlik başına tek frame: J1939'da her PGN periyodik tekrar
 * eder (10 ms – 1 s); 250 kbit/s hat saniyede ~2000 frame taşır. Kuyruklasak
 * saniyeler içinde taşar ve rapora hattın rastgele bir dilimi giderdi. Sunucunun
 * istediği her sinyalin GÜNCEL değeri; o yüzden kimlik başına yalnız son frame.
 *
 * İki kaynaktan beslenir, ikisi de aynı kapıya gider (can_service_ingest):
 *   - drivers/can/twai_driver   gerçek hat (ENABLE_CAN=1, transceiver şart)
 *   - command_service `can inject 0xID,b0,..,b7`   donanımsız test
 *
 * Telemetri biçimi (telemetry_service):
 *   "can":{"rx_total":N,"distinct":D,"dropped":X,"frames":["0x18FEE500,01,02,03,04,05,06,07,08", ...]}
 *   Kimlik HER ZAMAN 8 hex hane (0x00000123, 0x123 değil): sunucu sabit konumdan
 *   kestiği için hane sayısı değişirse yanlış sinyale eşleşir ve hata vermez.
 *
 * Yapılandırma (platformio build_flags ile ezilebilir):
 *   ENABLE_CAN              0 = TWAI derlenmez, yalnız inject (varsayılan)
 *   PIN_CAN_TX / PIN_CAN_RX transceiver'a giden ESP pinleri (ENABLE_CAN=1 için ŞART)
 *   CAN_BITRATE_DEFAULT_KBPS 250 (NVS `set can bitrate` ile kalıcı değişir)
 *   CAN_ID_TABLE_SIZE       96 kimlik
 *   CAN_FRAMES_PER_REPORT   bir raporda azami frame (en taze olanlar)
 */
#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef ENABLE_CAN
#define ENABLE_CAN 0
#endif
#ifndef PIN_CAN_TX
#define PIN_CAN_TX -1
#endif
#ifndef PIN_CAN_RX
#define PIN_CAN_RX -1
#endif
#ifndef PIN_CAN_STBY
#define PIN_CAN_STBY -1          /* transceiver standby pini; yoksa -1 */
#endif
#ifndef CAN_TASK_CORE
#define CAN_TASK_CORE 1
#endif
#ifndef CAN_TASK_PRIORITY
#define CAN_TASK_PRIORITY 6      /* yakalama kaçırılmasın: UI/telemetriden yüksek */
#endif
#ifndef CAN_ID_TABLE_SIZE
#define CAN_ID_TABLE_SIZE 96
#endif
#ifndef CAN_FRAMES_PER_REPORT
#define CAN_FRAMES_PER_REPORT 48
#endif
#ifndef CAN_BITRATE_DEFAULT_KBPS
#define CAN_BITRATE_DEFAULT_KBPS 250
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;          /**< 11 veya 29 bit CAN kimliği (ham) */
    uint8_t  data[8];
    uint8_t  dlc;         /**< 0-8 */
    bool     ext;         /**< 29-bit genişletilmiş kimlik */
    int64_t  last_us;     /**< son görülme (esp_timer_get_time) */
    uint32_t count;       /**< bu kimlikten kaç frame görüldü */
} can_frame_t;

typedef struct {
    uint32_t rx_total;      /**< toplam alınan frame (açılıştan beri) */
    uint32_t dropped_full;  /**< tablo doluyken atılan YENİ kimlik */
    uint16_t distinct;      /**< tablodaki farklı kimlik sayısı */
    uint16_t capacity;      /**< tablo kapasitesi */
} can_stats_t;

/** Tabloyu kur, NVS'ten bit hızını oku. */
esp_err_t can_service_init(void);

/** Bir frame'i tabloya işle (TWAI görevi ve `can inject` aynı kapıyı kullanır; mutex'li). */
void can_service_ingest(uint32_t id, bool ext, const uint8_t* data, uint8_t dlc);

/** Telemetri için anlık görüntü: en taze kimlikten eskiye doğru, en fazla max. */
size_t can_service_snapshot(can_frame_t* out, size_t max);

void can_service_stats(can_stats_t* out);
void can_service_clear(void);

/** Bir frame'i telemetri metnine çevir: "0x%08X,b0,..,b7" (kimlik 8 hane, baytlar 2 hane).
 *  @return yazılan uzunluk; sığmazsa 0 */
size_t can_service_format_frame(const can_frame_t* f, char* out, size_t cap);

/** Telemetri JSON parçası: `"can":{...}` (başında virgül YOK). Tablo boşsa 0 yazar.
 *  @return yazılan uzunluk; sığmazsa 0 */
size_t can_service_json(char* out, size_t cap);

/** Bit hızı (kbps): NVS'teki değer, yoksa CAN_BITRATE_DEFAULT_KBPS. */
uint32_t  can_service_bitrate(void);
esp_err_t can_service_set_bitrate(uint32_t kbps);   /* 125|250|500|1000, NVS'e yazar; reboot gerekir */

#ifdef __cplusplus
}
#endif
