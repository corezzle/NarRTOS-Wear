/**
 * @file ble_cmd_service.cpp
 * @brief BLE komut taşıyıcısı (bkz. header). NimBLE GATT server servisi.
 *
 * Akış: telefon RX characteristic'ine satır yazar → satır kuyruğa konur → ayrı "ble_cmd" görevi
 * command_service_exec_ex(line, ble_reply) çağırır → her yanıt satırı TX characteristic'inden notify
 * edilir. (2026-11-09: eskiden host task içinde çalışıyordu; `mic nabiz` gibi 15-300 sn süren komutlar
 * NimBLE host'u kilitlerdi.) exec çekirdeği USB ile
 * paylaşılan mutex'le serileşir (command_service).
 *
 * Yanıt boyutu: notify payload'ı bağlantı MTU'suyla sınırlı (iOS tipik 185+).
 * Satırlar CMD_BLE_CHUNK'a kırpılır — komut yanıtları zaten kısa.
 */

#if defined(BOARD_VARIANT_LCD128) || defined(BOARD_VARIANT_NAR)   /* 2026-11-09: NAR'da da (nabız/SD sonuçları telefondan) */

#include "ble_cmd_service.h"
#include "command_service.h"

#include <esp_log.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <nimble/ble.h>
#include <host/ble_hs.h>
#include <host/ble_gatt.h>
#include <os/os_mbuf.h>

static const char* TAG = "BleCmd";

#define CMD_BLE_LINE_MAX 128
#define CMD_BLE_CHUNK    180

/* ── UUID'ler — "NarRTOS CMD" özel servisi ──────────────────────────────────
 * 128-bit rastgele taban (proje-özel), son bayt characteristic ayrımı.
 * NimBLE BLE_UUID128_INIT byte'ları LİTTLE-ENDIAN (ters) sırayla alır. */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x53, 0x4f, 0x54, 0x52, 0x52, 0x41,
    0x4e, 0x00, 0x64, 0x6d, 0x63, 0x00, 0x01, 0x4e);
static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x02, 0x00, 0x53, 0x4f, 0x54, 0x52, 0x52, 0x41,
    0x4e, 0x00, 0x64, 0x6d, 0x63, 0x00, 0x01, 0x4e);
static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x03, 0x00, 0x53, 0x4f, 0x54, 0x52, 0x52, 0x41,
    0x4e, 0x00, 0x64, 0x6d, 0x63, 0x00, 0x01, 0x4e);

/* …0004: SON SONUÇ (READ) — command_service_get_result() metni; nRF Connect'te tek okuma (↓) ile görünür. */
static const ble_uuid128_t s_res_uuid = BLE_UUID128_INIT(
    0x04, 0x00, 0x53, 0x4f, 0x54, 0x52, 0x52, 0x41,
    0x4e, 0x00, 0x64, 0x6d, 0x63, 0x00, 0x01, 0x4e);
static uint16_t s_tx_val_handle = 0;
static uint16_t s_res_val_handle = 0;

static int res_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    const char* r = command_service_get_result();
    int rc = os_mbuf_append(ctxt->om, r, (uint16_t)strlen(r));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ── Yanıt sink'i: satırı TX characteristic'inden notify et ─────────────── */
static void ble_reply(const char* line, void* ctx)
{
    uint16_t conn = (uint16_t)(uintptr_t)ctx;
    if (conn == BLE_HS_CONN_HANDLE_NONE || s_tx_val_handle == 0) return;

    size_t len = strlen(line);
    if (len > CMD_BLE_CHUNK) len = CMD_BLE_CHUNK;

    struct os_mbuf* om = ble_hs_mbuf_from_flat(line, (uint16_t)len);
    if (!om) return;
    /* Abone değilse NimBLE sessizce düşürür; USB loguna zaten yazılmıyor —
     * BLE yanıtı BLE'ye, seri yanıtı seriye (taşıyıcı izolasyonu). */
    ble_gatts_notify_custom(conn, s_tx_val_handle, om);
}

/* ── Komut işçisi: uzun komutlar (mic nabiz, sdrec) host task'ı bloklamasın ─ */
typedef struct { char line[CMD_BLE_LINE_MAX]; uint16_t conn; } ble_cmd_job_t;
static QueueHandle_t s_queue = NULL;

static void ble_cmd_worker(void* arg)
{
    (void)arg;
    ble_cmd_job_t job;
    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        command_service_exec_ex(job.line, ble_reply, (void*)(uintptr_t)job.conn);
    }
}

/* ── GATT erişim callback'i ─────────────────────────────────────────────── */
static int cmd_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt* ctxt, void* arg)
{
    (void)attr_handle; (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char line[CMD_BLE_LINE_MAX];
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, line, sizeof(line) - 1, &len);
        if (rc != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
        line[len] = '\0';

        ESP_LOGI(TAG, "cmd from phone: '%s'", line);
        ble_cmd_job_t job; memcpy(job.line, line, sizeof job.line); job.conn = conn_handle;
        if (!s_queue || xQueueSend(s_queue, &job, 0) != pdTRUE) {
            ble_reply("[cmd] ERR: mesgul (onceki komut suruyor)", (void*)(uintptr_t)conn_handle);
        }
        return 0;
    }

    /* TX okunmak istenirse boş dön (yalnız notify taşıyıcısı) */
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return 0;

    return BLE_ATT_ERR_UNLIKELY;
}

/* ── Servis tanımı ──────────────────────────────────────────────────────── */
static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid       = &s_rx_uuid.u,
        .access_cb  = cmd_chr_access,
        .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid       = &s_tx_uuid.u,
        .access_cb  = cmd_chr_access,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_tx_val_handle,
    },
    {
        .uuid       = &s_res_uuid.u,
        .access_cb  = res_chr_access,
        .flags      = BLE_GATT_CHR_F_READ,
        .val_handle = &s_res_val_handle,
    },
    { 0 },   /* sonlandırıcı */
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    { 0 },
};

esp_err_t ble_cmd_gatt_register(void)
{
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg fail: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs fail: %d", rc);
        return ESP_FAIL;
    }
    if (!s_queue) {
        s_queue = xQueueCreate(4, sizeof(ble_cmd_job_t));
        /* 8 KB dahili yığın (komutlar NVS/flaş yazabilir → PSRAM yığın olmaz): mic nabiz/sdrec burada koşar, yanıtlar NimBLE notify */
        if (!s_queue || xTaskCreatePinnedToCore(ble_cmd_worker, "ble_cmd", 8192, NULL, 3, NULL, 1) != pdPASS) {
            ESP_LOGE(TAG, "ble_cmd worker olusturulamadi");
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "NarRTOS CMD GATT service registered");
    return ESP_OK;
}

#endif /* BOARD_VARIANT_LCD128 || BOARD_VARIANT_NAR */
