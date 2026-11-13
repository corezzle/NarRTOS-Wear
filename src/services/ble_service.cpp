/**
 * @file ble_service.cpp
 * @brief NimBLE peripheral implementation — central'a (iPhone) bağlanılabilir
 *        bir periferik olarak görünüp ANCS gibi servisleri kullanmamızı sağlar.
 *
 * Pattern referans: ESP-IDF nimble bleprph örneği + Wasp-OS yaklaşımı.
 *
 * iOS/ANCS notları:
 *   - iPhone "Solicit Service" advertise edersek ANCS'i hızlı keşfeder
 *     (Apple güvenlik politikası: ANCS'e erişmek için pairing + bond şart).
 *   - Pairing tipi: LE Secure Connections (LESC), Just Works veya Passkey.
 *     v1: Just Works (kullanıcı saatta confirm), v2: passkey display.
 *   - Bond NVS'te tutulur — power cycle sonrası otomatik reconnect.
 */

#include "ble_service.h"
#include "ancs_service.h"
#include "ble_cmd_service.h"   /* NarRTOS CMD — telefondan komut (LCD128) */
#include <esp_log.h>
#include <esp_nimble_hci.h>
#include <nvs_flash.h>
#include <string.h>

/* NimBLE headers */
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/util/util.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

/* NimBLE config store — NVS-backed bond persistence (v1.4.67).
 * Header not always in public include path; declare forward. */
extern "C" void ble_store_config_init(void);

static const char* TAG = "BLE";

#define BLE_DEVICE_NAME   "NarRTOS Watch"

/* ANCS service UUID (Apple spec): 7905F431-B5CE-4E99-A40F-4B1E122D00D0
 * Solicit (advertise'da bildiğimiz peer service) → iOS hızlı bonding'i hak verir. */
static const ble_uuid128_t s_ancs_uuid = BLE_UUID128_INIT(
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79
);

/* ─── State ────────────────────────────────────────────────────────────────── */

static ble_state_t            s_state            = BLE_STATE_OFF;
static uint16_t               s_conn_handle      = 0xFFFF;
static uint8_t                s_own_addr_type    = BLE_OWN_ADDR_PUBLIC;
static char                   s_peer_name[64]    = {0};

static ble_state_cb_t         s_state_cb         = NULL;
static void*                  s_state_cb_ud      = NULL;
static ble_connected_cb_t     s_on_connect       = NULL;
static ble_disconnected_cb_t  s_on_disconnect    = NULL;
static void*                  s_conn_cb_ud       = NULL;

/* ─── Helpers ──────────────────────────────────────────────────────────────── */

static void set_state(ble_state_t s)
{
    if (s_state == s) return;
    s_state = s;
    ESP_LOGI(TAG, "state → %d", (int)s);
    if (s_state_cb) s_state_cb(s, s_conn_handle, s_state_cb_ud);
}

/* Forward decl'lar */
static int  gap_event_cb(struct ble_gap_event* event, void* arg);
static void start_advertising_internal(void);

/* ─── GAP advertising ──────────────────────────────────────────────────────── */

static void start_advertising_internal(void)
{
    /* BLE adv packet limit: 31 bytes. Flags(3) + Name(15) + SolicitUUID128(18)
     * + TxPwr(3) = 39 → overflow. v1.4.66 fix: Solicit UUID scan response'a
     * taşındı, TX power kaldırıldı.
     *
     *   Adv packet (18 byte): Flags + Complete Local Name
     *   Scan rsp   (18 byte): Solicit Service UUID128 (ANCS) */
    struct ble_hs_adv_fields adv = {};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name             = (uint8_t*)BLE_DEVICE_NAME;
    adv.name_len         = (uint8_t)strlen(BLE_DEVICE_NAME);
    adv.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields fail: %d", rc);
        set_state(BLE_STATE_ERROR);
        return;
    }

    /* Scan response — iOS ANCS pairing flow için solicit hint */
    struct ble_hs_adv_fields rsp = {};
    rsp.sol_uuids128     = (ble_uuid128_t*)&s_ancs_uuid;
    rsp.sol_num_uuids128 = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields fail: %d (continuing without solicit)", rc);
        /* Non-fatal — adv hala çalışır, sadece iOS auto-pair UX'i biraz daha
         * yavaş olabilir. iOS yine de connect edip ANCS'i kullanabilir. */
    }

    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;       /* undirected connectable */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;       /* general discoverable */
    /* Interval: 30-60 ms aralığı — bonding sırasında hızlı response. ANCS
     * connected olunca peripheral disconnect olur ve advertise durur, yani
     * "her zaman açık" değiliz. */
    adv_params.itvl_min  = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    adv_params.itvl_max  = BLE_GAP_ADV_FAST_INTERVAL1_MAX;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event_cb, NULL);
    if (rc == BLE_HS_EALREADY) {
        /* Stack disconnect sonrası auto-resume yapıyor; çift call no-op */
        ESP_LOGD(TAG, "adv_start: already advertising");
        set_state(BLE_STATE_ADVERTISING);
        return;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start fail: %d", rc);
        set_state(BLE_STATE_ERROR);
        return;
    }

    ESP_LOGI(TAG, "advertising as \"%s\"", BLE_DEVICE_NAME);
    set_state(BLE_STATE_ADVERTISING);
}

/* ─── GAP event handler ────────────────────────────────────────────────────── */

static int gap_event_cb(struct ble_gap_event* event, void* arg)
{
    (void)arg;

    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected, handle=%d", s_conn_handle);
            set_state(BLE_STATE_CONNECTED);

            /* Encryption başlat (iOS ANCS için şart). Eğer bond varsa
             * NimBLE otomatik restore eder; yoksa pairing prompt çıkar. */
            int rc = ble_gap_security_initiate(s_conn_handle);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "security_initiate fail: %d", rc);
            }
        } else {
            ESP_LOGW(TAG, "connect failed status=%d, re-advertising",
                     event->connect.status);
            start_advertising_internal();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t old_handle = s_conn_handle;
        ESP_LOGI(TAG, "disconnect reason=0x%x, re-advertising",
                 event->disconnect.reason);
        s_conn_handle  = 0xFFFF;
        s_peer_name[0] = '\0';
        if (s_on_disconnect) s_on_disconnect(old_handle, s_conn_cb_ud);
        set_state(BLE_STATE_INIT);
        start_advertising_internal();
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "enc_change status=%d", event->enc_change.status);
        if (event->enc_change.status == 0) {
            /* Encryption + bond OK → ANCS service'ini kullanabiliriz */
            struct ble_gap_conn_desc desc;
            int rc = ble_gap_conn_find(s_conn_handle, &desc);
            if (rc == 0 && desc.sec_state.bonded) {
                set_state(BLE_STATE_BONDED);
                if (s_on_connect) s_on_connect(s_conn_handle, s_conn_cb_ud);
            } else {
                ESP_LOGW(TAG, "encrypted but not bonded (yet)");
            }
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* v1: Just Works pairing — passkey gerekli değil. Yine de event'i
         * loglarız, ileride passkey display eklenirse buraya UI prompt girer. */
        ESP_LOGI(TAG, "passkey_action: action=%d",
                 event->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "adv_complete reason=%d",
                 event->adv_complete.reason);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        /* ANCS notifications subscribe sonrası buraya gelir. mbuf'i flatten
         * edip ancs_service'e forward et.
         * v1.4.69 — LOGD → LOGI: bildirimlerin gerçekten gelip gelmediğini
         * görmek için her notify_rx event'i log'lansın. */
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        ESP_LOGI(TAG, "notify_rx handle=%d len=%d conn=%d",
                 event->notify_rx.attr_handle, len,
                 event->notify_rx.conn_handle);
        if (len > 0 && len <= 512) {
            uint8_t buf[512];
            uint16_t copied = 0;
            int rc = ble_hs_mbuf_to_flat(event->notify_rx.om, buf, len, &copied);
            if (rc == 0 && copied > 0) {
                ancs_service_on_notify_rx(event->notify_rx.conn_handle,
                                           event->notify_rx.attr_handle,
                                           buf, copied);
            } else {
                ESP_LOGW(TAG, "mbuf_to_flat fail rc=%d copied=%d", rc, copied);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu update conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%d notify=%d indicate=%d",
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify,
                 event->subscribe.cur_indicate);
        return 0;

    default:
        return 0;
    }
}

/* ─── Host stack callbacks ─────────────────────────────────────────────────── */

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);   /* random veya public addr garanti et */
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr fail: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "id_infer_auto fail: %d", rc);
        return;
    }

    uint8_t addr_val[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr_val, NULL);
    ESP_LOGI(TAG, "own addr: %02x:%02x:%02x:%02x:%02x:%02x",
             addr_val[5], addr_val[4], addr_val[3],
             addr_val[2], addr_val[1], addr_val[0]);

    start_advertising_internal();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset reason=%d", reason);
    set_state(BLE_STATE_ERROR);
}

static void host_task(void* arg)
{
    (void)arg;
    nimble_port_run();   /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ─── Public API ───────────────────────────────────────────────────────────── */

esp_err_t ble_service_init(void)
{
    if (s_state != BLE_STATE_OFF) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    /* NVS init — bond store burada tutulur. Wi-Fi service muhtemelen zaten
     * init etmiştir, idempotent. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init fail: %s", esp_err_to_name(err));
        return err;
    }

    /* NimBLE port init */
    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init fail: %s", esp_err_to_name(err));
        return err;
    }

    /* Host configuration */
    ble_hs_cfg.sync_cb              = on_sync;
    ble_hs_cfg.reset_cb             = on_reset;
    ble_hs_cfg.store_status_cb      = ble_store_util_status_rr;

    /* Security: LE Secure Connections, bonding, MITM-free Just Works.
     * iOS pairing prompt'unu accept eden kullanıcı bond oluşmasını sağlar. */
    ble_hs_cfg.sm_io_cap            = BLE_SM_IO_CAP_NO_IO;  /* Just Works */
    ble_hs_cfg.sm_bonding           = 1;
    ble_hs_cfg.sm_mitm              = 0;
    ble_hs_cfg.sm_sc                = 1;                     /* Secure Connections */
    ble_hs_cfg.sm_our_key_dist      = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist    = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* Standart GATT / GAP servisleri */
    ble_svc_gap_init();
    ble_svc_gatt_init();

#if defined(BOARD_VARIANT_LCD128) || defined(BOARD_VARIANT_NAR)
    /* NarRTOS CMD — telefondan komut taşıyıcısı (RX write + TX notify).
     * Host task başlamadan önce kaydedilmeli. Başarısızlık BLE'yi bozmaz. */
    if (ble_cmd_gatt_register() != ESP_OK) {
        ESP_LOGW(TAG, "ble_cmd GATT register failed — komut taşıyıcısı devre dışı");
    }
#endif

    /* NVS-backed bond store — pairing info kalıcı, power cycle sonrası
     * auto-reconnect. Olmadan iOS "Peer removed pairing information" hatası
     * verir (peer bond bekler, store boş). */
    ble_store_config_init();

    int rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGW(TAG, "device_name_set fail: %d", rc);
    }

    /* State'i host task spawn ÖNCE INIT'e set et — host task on_sync'te
     * advertise başlatacak ve state'i ADVERTISING'e çekecek. Sırayı tersine
     * yaparsak race: host task ADVERTISING set eder, biz sonra INIT'le
     * üzerine yazarız → state yanlış (advertising NimBLE'da devam eder ama
     * UI flag'lerimiz yanlış olur). */
    set_state(BLE_STATE_INIT);
    ESP_LOGI(TAG, "ble_service init OK");

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t ble_service_start_advertising(void)
{
    if (s_state == BLE_STATE_ADVERTISING) return ESP_OK;
    if (s_state == BLE_STATE_CONNECTED || s_state == BLE_STATE_BONDED) {
        ESP_LOGW(TAG, "already connected, skip adv");
        return ESP_ERR_INVALID_STATE;
    }
    start_advertising_internal();
    return ESP_OK;
}

esp_err_t ble_service_stop_advertising(void)
{
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv_stop fail: %d", rc);
        return ESP_FAIL;
    }
    if (s_state == BLE_STATE_ADVERTISING) set_state(BLE_STATE_INIT);
    return ESP_OK;
}

esp_err_t ble_service_disconnect(void)
{
    if (s_conn_handle == 0xFFFF) return ESP_ERR_INVALID_STATE;
    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_service_clear_bonds(void)
{
    int rc = ble_store_clear();
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

ble_state_t ble_service_get_state(void)
{
    return s_state;
}

uint16_t ble_service_get_conn_handle(void)
{
    return s_conn_handle;
}

void ble_service_set_state_cb(ble_state_cb_t cb, void* user_data)
{
    s_state_cb    = cb;
    s_state_cb_ud = user_data;
}

void ble_service_set_connection_cb(ble_connected_cb_t   on_connect,
                                    ble_disconnected_cb_t on_disconnect,
                                    void* user_data)
{
    s_on_connect    = on_connect;
    s_on_disconnect = on_disconnect;
    s_conn_cb_ud    = user_data;
}

const char* ble_service_get_peer_name(void)
{
    return (s_peer_name[0] != '\0') ? s_peer_name : NULL;
}
