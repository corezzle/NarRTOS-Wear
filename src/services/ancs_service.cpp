/**
 * @file ancs_service.cpp
 * @brief ANCS client — service discovery + notification fetch + store push.
 *
 * State machine:
 *   IDLE → DISCOVERING_SERVICE → DISCOVERING_CHARS → SUBSCRIBED → READY
 *   READY: notifications akmaya başlar
 *
 * v1 kapsamı:
 *   - Service + characteristic handle discovery
 *   - Notification Source subscribe (CCC=0x0001)
 *   - 8-byte event parse → store'a "title pending" entry
 *   - Control Point'e GetNotificationAttributes command
 *   - Data Source'tan response (multi-packet potansiyel) → parse title/msg/app
 *   - notification_store_add ile UI'ye push
 *
 * v1 dışı (sonraki versiyonlar):
 *   - GetAppAttributes (DisplayName için)
 *   - PerformNotificationAction (reply/answer)
 *   - Multi-fragment data source buffer
 */

#include "ancs_service.h"
#include "ble_service.h"
#include "notification_store.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>

#include <host/ble_hs.h>
#include <host/ble_gatt.h>
#include <host/ble_uuid.h>

static const char* TAG = "ANCS";

/* ─── UUIDs (Apple spec) ───────────────────────────────────────────────────── */

static const ble_uuid128_t s_uuid_service = BLE_UUID128_INIT(
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79
);
static const ble_uuid128_t s_uuid_notif_source = BLE_UUID128_INIT(
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C,
    0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F
);
static const ble_uuid128_t s_uuid_control_point = BLE_UUID128_INIT(
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98,
    0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69
);
static const ble_uuid128_t s_uuid_data_source = BLE_UUID128_INIT(
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE,
    0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22
);

/* ─── State ────────────────────────────────────────────────────────────────── */

typedef enum {
    ANCS_S_IDLE          = 0,
    ANCS_S_DISCOVERING   = 1,
    ANCS_S_SUBSCRIBING   = 2,
    ANCS_S_READY         = 3,
} ancs_state_t;

static ancs_state_t   s_state             = ANCS_S_IDLE;
static uint16_t       s_conn_handle       = 0xFFFF;

static uint16_t       s_h_notif_src       = 0;     /* Notification Source value handle */
static uint16_t       s_h_notif_src_ccc   = 0;     /* CCC descriptor handle */
static uint16_t       s_h_ctrl_point      = 0;     /* Control Point value handle */
static uint16_t       s_h_data_src        = 0;     /* Data Source value handle */
static uint16_t       s_h_data_src_ccc    = 0;

/* Data Source assembly buffer — multi-packet response için */
static uint8_t        s_ds_buf[512];
static uint16_t       s_ds_len            = 0;
static uint32_t       s_ds_expected_uid   = 0;

/* ─── ANCS protocol constants ──────────────────────────────────────────────── */

/* Event IDs (Notification Source byte 0) */
enum {
    EVT_NOTIF_ADDED     = 0,
    EVT_NOTIF_MODIFIED  = 1,
    EVT_NOTIF_REMOVED   = 2,
};

/* Event flags (Notification Source byte 1) */
#define FLAG_SILENT          0x01
#define FLAG_IMPORTANT       0x02
#define FLAG_PRE_EXISTING    0x04
#define FLAG_POS_ACTION      0x08
#define FLAG_NEG_ACTION      0x10

/* Control Point Command IDs */
enum {
    CMD_GET_NOTIF_ATTRS     = 0,
    CMD_GET_APP_ATTRS       = 1,
    CMD_PERFORM_NOTIF_ACTION= 2,
};

/* NotificationAttributeID values */
enum {
    ATTR_APP_IDENTIFIER     = 0,
    ATTR_TITLE              = 1,
    ATTR_SUBTITLE           = 2,
    ATTR_MESSAGE            = 3,
    ATTR_MESSAGE_SIZE       = 4,
    ATTR_DATE               = 5,
    ATTR_POS_ACTION_LABEL   = 6,
    ATTR_NEG_ACTION_LABEL   = 7,
};

/* ActionID values */
enum {
    ACTION_POSITIVE         = 0,
    ACTION_NEGATIVE         = 1,   /* "dismiss" eşdeğeri çoğu app için */
};

/* ─── Forward decls ────────────────────────────────────────────────────────── */

static void start_discovery(uint16_t conn_handle);
static int  on_service_discovered(uint16_t conn_handle,
                                    const struct ble_gatt_error* error,
                                    const struct ble_gatt_svc* service, void* arg);
static int  on_char_discovered(uint16_t conn_handle,
                                const struct ble_gatt_error* error,
                                const struct ble_gatt_chr* chr, void* arg);
static int  on_dsc_discovered(uint16_t conn_handle,
                                const struct ble_gatt_error* error,
                                uint16_t chr_val_handle,
                                const struct ble_gatt_dsc* dsc, void* arg);
static void subscribe_to_notifications(void);
static int  on_subscribe_complete(uint16_t conn_handle,
                                   const struct ble_gatt_error* error,
                                   struct ble_gatt_attr* attr, void* arg);
static void request_notif_attrs(uint32_t notif_uid);
static void handle_notif_source(const uint8_t* data, uint16_t len);
static void handle_data_source(const uint8_t* data, uint16_t len);
static void parse_attrs_response(const uint8_t* data, uint16_t len);

/* ─── Init ─────────────────────────────────────────────────────────────────── */

esp_err_t ancs_service_init(void)
{
    s_state = ANCS_S_IDLE;
    s_conn_handle = 0xFFFF;
    ESP_LOGI(TAG, "ancs_service init OK");
    return ESP_OK;
}

/* ─── BLE connection callbacks ─────────────────────────────────────────────── */

void ancs_service_on_connected(uint16_t conn_handle)
{
    s_conn_handle = conn_handle;
    start_discovery(conn_handle);
}

void ancs_service_on_disconnected(uint16_t conn_handle)
{
    (void)conn_handle;
    s_state            = ANCS_S_IDLE;
    s_conn_handle      = 0xFFFF;
    s_h_notif_src      = 0;
    s_h_notif_src_ccc  = 0;
    s_h_ctrl_point     = 0;
    s_h_data_src       = 0;
    s_h_data_src_ccc   = 0;
    s_ds_len           = 0;
    /* v1.4.71 — Disconnect'te store'u TEMİZLEME. iPhone BLE arada bir
     * disconnect/reconnect yapıyor (range, power save). Eski davranışta her
     * disconnect bildirimleri siliyordu → kullanıcı mesajı kaçırıyordu.
     * Reconnect olunca iPhone "pre-existing" event'leri tekrar gönderir,
     * UID-bazlı dedup zaten same-UID'i modify olarak işliyor. */
    ESP_LOGI(TAG, "disconnected, state reset (notifications preserved)");
}

/* ─── Service discovery ────────────────────────────────────────────────────── */

static void start_discovery(uint16_t conn_handle)
{
    s_state = ANCS_S_DISCOVERING;
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle,
                                          (const ble_uuid_t*)&s_uuid_service,
                                          on_service_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "disc_svc_by_uuid fail: %d", rc);
        s_state = ANCS_S_IDLE;
    }
}

static int on_service_discovered(uint16_t conn_handle,
                                  const struct ble_gatt_error* error,
                                  const struct ble_gatt_svc* service, void* arg)
{
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (s_h_notif_src == 0 && s_h_ctrl_point == 0 && s_h_data_src == 0) {
            ESP_LOGW(TAG, "no ANCS service found on peer");
            s_state = ANCS_S_IDLE;
            return 0;
        }
        /* Service found — chars discovery start */
        ESP_LOGI(TAG, "ANCS service found, handle range %d..%d (chars next)",
                 service ? service->start_handle : 0,
                 service ? service->end_handle   : 0);
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "disc_svc error: %d", error->status);
        return 0;
    }
    /* Service range bulundu → karakterler için disc_all_chrs */
    ESP_LOGI(TAG, "found ANCS service, handles %d..%d",
             service->start_handle, service->end_handle);
    int rc = ble_gattc_disc_all_chrs(conn_handle,
                                      service->start_handle, service->end_handle,
                                      on_char_discovered, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "disc_all_chrs fail: %d", rc);
    }
    return 0;
}

static int on_char_discovered(uint16_t conn_handle,
                               const struct ble_gatt_error* error,
                               const struct ble_gatt_chr* chr, void* arg)
{
    (void)arg;
    (void)conn_handle;
    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "char discovery done: NS=%d CP=%d DS=%d",
                 s_h_notif_src, s_h_ctrl_point, s_h_data_src);
        if (s_h_notif_src && s_h_ctrl_point && s_h_data_src) {
            /* v1.4.68 — descriptor discovery yerine BLE konvansiyonu kullan:
             * notify-able karakter için CCC her zaman val_handle + 1'dedir.
             * Önceki discovery yaklaşımı handle range overlap nedeniyle
             * NS CCC'sini DS CCC'siyle karıştırıyordu → NS subscribe atlanıyor,
             * bildirim event'i gelmiyor (log "subscribed NS OK" yanıltıcıydı). */
            s_h_notif_src_ccc = s_h_notif_src + 1;
            s_h_data_src_ccc  = s_h_data_src  + 1;
            ESP_LOGI(TAG, "CCC handles: NS=%d DS=%d (val_handle+1 convention)",
                     s_h_notif_src_ccc, s_h_data_src_ccc);
            subscribe_to_notifications();
        } else {
            ESP_LOGW(TAG, "ANCS chars incomplete, state idle");
            s_state = ANCS_S_IDLE;
        }
        return 0;
    }
    if (error->status != 0 || !chr) return 0;

    if (ble_uuid_cmp(&chr->uuid.u, (const ble_uuid_t*)&s_uuid_notif_source) == 0) {
        s_h_notif_src = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, (const ble_uuid_t*)&s_uuid_control_point) == 0) {
        s_h_ctrl_point = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, (const ble_uuid_t*)&s_uuid_data_source) == 0) {
        s_h_data_src = chr->val_handle;
    }
    return 0;
}

/* on_dsc_discovered artık kullanılmıyor (v1.4.68) — descriptor discovery
 * yerine val_handle+1 konvansiyonu. Forward decl tutarlılığı için stub
 * bırakıldı (header'da hala referans var). */
static int on_dsc_discovered(uint16_t /*conn_handle*/,
                              const struct ble_gatt_error* /*error*/,
                              uint16_t /*chr_val_handle*/,
                              const struct ble_gatt_dsc* /*dsc*/,
                              void* /*arg*/)
{
    return 0;
}

/* ─── Subscribe to notifications ──────────────────────────────────────────── */

static void subscribe_to_notifications(void)
{
    s_state = ANCS_S_SUBSCRIBING;

    /* CCC value 0x0001 = enable notifications */
    static const uint8_t enable_notif[] = { 0x01, 0x00 };

    int rc = ble_gattc_write_flat(s_conn_handle, s_h_notif_src_ccc,
                                    enable_notif, sizeof(enable_notif),
                                    on_subscribe_complete, (void*)(intptr_t)1);
    if (rc != 0) {
        ESP_LOGE(TAG, "subscribe NS fail: %d", rc);
        s_state = ANCS_S_IDLE;
        return;
    }

    rc = ble_gattc_write_flat(s_conn_handle, s_h_data_src_ccc,
                                enable_notif, sizeof(enable_notif),
                                on_subscribe_complete, (void*)(intptr_t)2);
    if (rc != 0) {
        ESP_LOGE(TAG, "subscribe DS fail: %d", rc);
    }
}

static int on_subscribe_complete(uint16_t conn_handle,
                                  const struct ble_gatt_error* error,
                                  struct ble_gatt_attr* attr, void* arg)
{
    (void)conn_handle;
    (void)attr;
    int which = (int)(intptr_t)arg;

    if (error->status == 0) {
        ESP_LOGI(TAG, "subscribed %s OK", which == 1 ? "NS" : "DS");
        if (which == 2) {
            s_state = ANCS_S_READY;
            ESP_LOGI(TAG, "ANCS ready — waiting for notifications");
        }
    } else {
        ESP_LOGE(TAG, "subscribe %s fail: status=%d", which == 1 ? "NS" : "DS",
                 error->status);
    }
    return 0;
}

/* ─── Incoming notify_rx dispatch ──────────────────────────────────────────── */

void ancs_service_on_notify_rx(uint16_t conn_handle, uint16_t attr_handle,
                                const uint8_t* data, uint16_t len)
{
    ESP_LOGI(TAG, "rx conn=%d handle=%d (NS=%d DS=%d) len=%d state=%d",
             conn_handle, attr_handle, s_h_notif_src, s_h_data_src, len, s_state);
    if (conn_handle != s_conn_handle) {
        ESP_LOGW(TAG, "  → ignored, conn mismatch (mine=%d)", s_conn_handle);
        return;
    }
    if (attr_handle == s_h_notif_src) {
        handle_notif_source(data, len);
    } else if (attr_handle == s_h_data_src) {
        handle_data_source(data, len);
    } else {
        ESP_LOGW(TAG, "  → unknown handle, dropped");
    }
}

/* ─── Notification Source handler (8-byte event) ──────────────────────────── */

static void handle_notif_source(const uint8_t* data, uint16_t len)
{
    if (len < 8) {
        ESP_LOGW(TAG, "NS event short len=%d", len);
        return;
    }
    uint8_t  evt_id    = data[0];
    uint8_t  flags     = data[1];
    uint8_t  cat_id    = data[2];
    uint8_t  cat_count = data[3];
    uint32_t uid       = (uint32_t)data[4]       |
                         ((uint32_t)data[5] << 8) |
                         ((uint32_t)data[6] <<16) |
                         ((uint32_t)data[7] <<24);

    (void)cat_count;

    if (evt_id == EVT_NOTIF_REMOVED) {
        notification_store_remove(uid);
        return;
    }

    /* Pre-existing notifications (boot sonrası phone'da zaten varolanlar) —
     * yine de fetch ederiz, kullanıcı eski mesajları görsün. */

    /* ADD veya MODIFY: stub entry hemen store'a yaz (metadata: kategori,
     * flags, timestamp). Data Source attribute response geldiğinde aynı UID
     * MODIFY ile gerçek title/message ile güncellenir. Banner MODIFIED
     * eventinde de tetiklenip içeriği yeniler. */
    notification_entry_t e = {};
    e.uid             = uid;
    e.category        = (notif_category_t)cat_id;
    e.received_us     = esp_timer_get_time();
    e.is_silent       = (flags & FLAG_SILENT) != 0;
    e.is_important    = (flags & FLAG_IMPORTANT) != 0;
    e.has_positive_action = (flags & FLAG_POS_ACTION) != 0;
    e.has_negative_action = (flags & FLAG_NEG_ACTION) != 0;
    /* Stub title boş — banner DS response gelene kadar bekleyecek (banner
     * boş title'lı entry'leri skip eder). */
    strncpy(e.title,   "",  sizeof(e.title) - 1);
    strncpy(e.message, "",  sizeof(e.message) - 1);
    notification_store_add(&e, (evt_id == EVT_NOTIF_MODIFIED));

    /* Now request the attributes for this notification UID. */
    request_notif_attrs(uid);
}

/* ─── Request notification attributes via Control Point ───────────────────── */

static void request_notif_attrs(uint32_t uid)
{
    /* Command format:
     *   CommandID (1) = 0
     *   NotificationUID (4)
     *   AttributeIDList: for each, AttrID (1), [MaxLen (2) if string]
     *
     * İstediğimiz alanlar: AppIdentifier, Title (max 32), Message (max 192).
     * Subtitle çoğu WhatsApp grup'unda group adı; bu da güzel olur. */
    uint8_t cmd[1 + 4 + 1 + (1+2) + (1+2) + (1+2) + 1];
    int p = 0;
    cmd[p++] = CMD_GET_NOTIF_ATTRS;
    cmd[p++] = (uid >>  0) & 0xFF;
    cmd[p++] = (uid >>  8) & 0xFF;
    cmd[p++] = (uid >> 16) & 0xFF;
    cmd[p++] = (uid >> 24) & 0xFF;

    cmd[p++] = ATTR_APP_IDENTIFIER;

    cmd[p++] = ATTR_TITLE;
    cmd[p++] = 32 & 0xFF;
    cmd[p++] = (32 >> 8) & 0xFF;

    cmd[p++] = ATTR_SUBTITLE;
    cmd[p++] = 32 & 0xFF;
    cmd[p++] = (32 >> 8) & 0xFF;

    cmd[p++] = ATTR_MESSAGE;
    cmd[p++] = 192 & 0xFF;
    cmd[p++] = (192 >> 8) & 0xFF;

    s_ds_expected_uid = uid;
    s_ds_len = 0;

    int rc = ble_gattc_write_flat(s_conn_handle, s_h_ctrl_point,
                                    cmd, p, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "CP write fail: %d", rc);
    }
}

/* ─── Data Source handler (multi-packet attribute response) ───────────────── */

static void handle_data_source(const uint8_t* data, uint16_t len)
{
    /* Append to assembly buffer. iPhone potansiyel olarak büyük message'i
     * birden fazla NOTIFY paketinde gönderir (MTU sınırlı). */
    if (s_ds_len + len > sizeof(s_ds_buf)) {
        ESP_LOGW(TAG, "DS buffer overflow, dropping");
        s_ds_len = 0;
        return;
    }
    memcpy(s_ds_buf + s_ds_len, data, len);
    s_ds_len += len;

    /* Response format:
     *   CommandID (1) = 0 (GetNotificationAttributes)
     *   NotificationUID (4)
     *   for each AttrID: AttrID (1), Length (2 LE), Value (N)
     *
     * Tüm attributes geldiğinde parse et. Heuristik: command yanıtı her
     * notification için tek kez geliyor; biz tüm istediğimiz attribute'leri
     * okuduğumuzda dur. */
    parse_attrs_response(s_ds_buf, s_ds_len);
}

static void parse_attrs_response(const uint8_t* data, uint16_t len)
{
    if (len < 5) return;
    if (data[0] != CMD_GET_NOTIF_ATTRS) return;

    uint32_t uid = (uint32_t)data[1]       |
                   ((uint32_t)data[2] << 8) |
                   ((uint32_t)data[3] <<16) |
                   ((uint32_t)data[4] <<24);

    /* Önceki stub'ı bul ve doldur. Yoksa yeni entry yarat. */
    const notification_entry_t* existing = notification_store_get(uid);
    notification_entry_t entry = {};
    if (existing) {
        entry = *existing;   /* category + flags zaten set */
    } else {
        entry.uid = uid;
        entry.received_us = esp_timer_get_time();
    }

    uint16_t p = 5;
    int parsed_any = 0;
    while (p + 3 <= len) {
        uint8_t  attr_id = data[p++];
        uint16_t attr_len = (uint16_t)data[p] | ((uint16_t)data[p+1] << 8);
        p += 2;
        if (p + attr_len > len) {
            /* Eksik — daha fazla packet bekle */
            return;
        }

        size_t copy = attr_len;
        switch (attr_id) {
        case ATTR_APP_IDENTIFIER:
            if (copy >= sizeof(entry.app_id)) copy = sizeof(entry.app_id) - 1;
            memcpy(entry.app_id, data + p, copy);
            entry.app_id[copy] = '\0';
            break;
        case ATTR_TITLE:
            if (copy >= sizeof(entry.title)) copy = sizeof(entry.title) - 1;
            memcpy(entry.title, data + p, copy);
            entry.title[copy] = '\0';
            break;
        case ATTR_SUBTITLE:
            if (copy >= sizeof(entry.subtitle)) copy = sizeof(entry.subtitle) - 1;
            memcpy(entry.subtitle, data + p, copy);
            entry.subtitle[copy] = '\0';
            break;
        case ATTR_MESSAGE:
            if (copy >= sizeof(entry.message)) copy = sizeof(entry.message) - 1;
            memcpy(entry.message, data + p, copy);
            entry.message[copy] = '\0';
            break;
        default:
            break;
        }
        p += attr_len;
        parsed_any = 1;
    }

    if (parsed_any) {
        /* DEBUG (v1.4.72): Türkçe karakter teşhisi — title/message'in raw
         * byte'larını hex olarak dump et. UTF-8 ise ü = C3 BC bekleriz;
         * Latin-9/ISO-8859-9 ise FC; başka bir şey ise teşhis edebilelim. */
        ESP_LOGI(TAG, "DEBUG title bytes: \"%s\"", entry.title);
        for (size_t i = 0; i < strlen(entry.title); i++) {
            ESP_LOGI(TAG, "  title[%zu] = 0x%02X ('%c')",
                     i, (unsigned char)entry.title[i],
                     (entry.title[i] >= 0x20 && entry.title[i] < 0x7F)
                       ? entry.title[i] : '.');
        }
        ESP_LOGI(TAG, "DEBUG msg bytes: \"%s\"", entry.message);
        for (size_t i = 0; i < strlen(entry.message) && i < 32; i++) {
            ESP_LOGI(TAG, "  msg[%zu] = 0x%02X ('%c')",
                     i, (unsigned char)entry.message[i],
                     (entry.message[i] >= 0x20 && entry.message[i] < 0x7F)
                       ? entry.message[i] : '.');
        }
        notification_store_add(&entry, /*is_modify=*/true);
        s_ds_len = 0;   /* assembly reset */
    }
}

/* ─── Dismiss action ───────────────────────────────────────────────────────── */

esp_err_t ancs_service_dismiss(uint32_t uid)
{
    if (s_state != ANCS_S_READY || s_h_ctrl_point == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t cmd[1 + 4 + 1];
    cmd[0] = CMD_PERFORM_NOTIF_ACTION;
    cmd[1] = (uid >>  0) & 0xFF;
    cmd[2] = (uid >>  8) & 0xFF;
    cmd[3] = (uid >> 16) & 0xFF;
    cmd[4] = (uid >> 24) & 0xFF;
    cmd[5] = ACTION_NEGATIVE;

    int rc = ble_gattc_write_flat(s_conn_handle, s_h_ctrl_point,
                                    cmd, sizeof(cmd), NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "dismiss write fail: %d", rc);
        return ESP_FAIL;
    }
    /* Optimistically remove local copy — ANCS NOTIF_REMOVED event de gelir */
    notification_store_remove(uid);
    return ESP_OK;
}
