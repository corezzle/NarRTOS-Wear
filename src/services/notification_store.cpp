/**
 * @file notification_store.cpp
 * @brief Ring-buffer notification store, single-listener model.
 *
 * Implementation notes:
 *   - Ring buffer: head = next write slot. Wrap-around overwrites oldest.
 *   - Iteration: en yeni → en eski, head'den geriye yürür.
 *   - Mutex koruma: BLE task add/remove, UI task read iterate.
 *   - Listener callback BLE task'tan tetiklenebilir — UI must lv_async_call.
 */

#include "notification_store.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>

static const char* TAG = "NotifStore";

static SemaphoreHandle_t       s_lock         = NULL;
static notification_entry_t    s_buf[NOTIF_STORE_CAPACITY];
static size_t                  s_count        = 0;   /* doluluk (0..CAPACITY) */
static size_t                  s_head         = 0;   /* sıradaki yazma slot'u */

/* v1.4.70 — multi-listener support. Banner service + Notifications app
 * aynı anda dinler. Slot doluluğu compile-time bound (NOTIF_LISTENERS_MAX). */
typedef struct {
    notif_store_listener_t cb;
    void*                  ud;
} listener_slot_t;

static listener_slot_t s_listeners[NOTIF_LISTENERS_MAX] = {0};

/* ─── Helpers ──────────────────────────────────────────────────────────────── */

static inline size_t idx_logical_to_phys(size_t logical)
{
    /* logical=0 en yeni (head-1), logical=count-1 en eski.
     * Phys index = (head - 1 - logical + CAPACITY) % CAPACITY. */
    return (s_head + NOTIF_STORE_CAPACITY - 1 - logical) % NOTIF_STORE_CAPACITY;
}

/* UID ile fiziksel slot index'i bul. -1 = bulunamadı. */
static int find_phys_by_uid(uint32_t uid)
{
    for (size_t i = 0; i < s_count; i++) {
        size_t p = idx_logical_to_phys(i);
        if (s_buf[p].uid == uid) return (int)p;
    }
    return -1;
}

static void notify_listener(notif_event_type_t evt, uint32_t uid)
{
    /* listener'ları lock dışında çağır → caller stack overflow / re-entry yok.
     * Caller mutex hold etmiyor olmalı — burada açıkça lock release sonrası
     * çağrılıyor (add/remove functions'ta). */
    for (size_t i = 0; i < NOTIF_LISTENERS_MAX; i++) {
        if (s_listeners[i].cb) {
            s_listeners[i].cb(evt, uid, s_listeners[i].ud);
        }
    }
}

/* ─── Init ─────────────────────────────────────────────────────────────────── */

void notification_store_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "mutex create fail");
        return;
    }
    s_count = 0;
    s_head  = 0;
    memset(s_buf, 0, sizeof(s_buf));
    ESP_LOGI(TAG, "init OK (capacity=%d)", NOTIF_STORE_CAPACITY);
}

/* ─── Add / Modify ─────────────────────────────────────────────────────────── */

void notification_store_add(const notification_entry_t* entry, bool is_modify)
{
    if (!s_lock || !entry) return;

    notif_event_type_t event;
    uint32_t           uid = entry->uid;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Existing UID var mı? → modify path */
    int existing = find_phys_by_uid(uid);
    if (existing >= 0) {
        s_buf[existing] = *entry;
        event = NOTIF_EVENT_MODIFIED;
    } else {
        /* Yeni entry — head slot'a yaz, head ilerlet, count cap'le */
        s_buf[s_head] = *entry;
        s_head = (s_head + 1) % NOTIF_STORE_CAPACITY;
        if (s_count < NOTIF_STORE_CAPACITY) s_count++;
        event = NOTIF_EVENT_ADDED;
    }

    /* is_modify flag caller'ın hint'i; existing check ile zaten doğru olay
     * çıkar ama log + edge case için referans olarak tutuyoruz. */
    (void)is_modify;

    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "%s uid=%lu app=%s title=\"%s\"",
             event == NOTIF_EVENT_ADDED ? "ADD" : "MOD",
             (unsigned long)uid, entry->app_id, entry->title);

    notify_listener(event, uid);
}

/* ─── Remove ───────────────────────────────────────────────────────────────── */

void notification_store_remove(uint32_t uid)
{
    if (!s_lock) return;

    bool found = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int phys = find_phys_by_uid(uid);
    if (phys >= 0) {
        /* Slot'u "boş" işaretle — sonraki iterate'de skip için uid=0
         * ama bizim find/iterate UID'e bakıyor, slot'u shift etmek gerek.
         *
         * Ring buffer'da middle delete pahalı; küçük buffer (20) için OK.
         * Strategy: silinen logical idx'ten sonrakileri shift (yeni → eski). */
        notification_entry_t tmp[NOTIF_STORE_CAPACITY];
        size_t new_count = 0;
        for (size_t i = 0; i < s_count; i++) {
            size_t p = idx_logical_to_phys(i);
            if (s_buf[p].uid == uid) continue;
            tmp[new_count++] = s_buf[p];
        }
        /* Repack: en yenisi tmp[0], head'i count'a göre yeniden hesapla */
        for (size_t i = 0; i < new_count; i++) {
            s_buf[i] = tmp[new_count - 1 - i];   /* en eskisi slot 0'a */
        }
        s_count = new_count;
        s_head  = new_count;
        memset(&s_buf[new_count], 0,
               (NOTIF_STORE_CAPACITY - new_count) * sizeof(s_buf[0]));
        found = true;
    }

    xSemaphoreGive(s_lock);

    if (found) {
        ESP_LOGI(TAG, "REMOVE uid=%lu", (unsigned long)uid);
        notify_listener(NOTIF_EVENT_REMOVED, uid);
    }
}

/* ─── Clear ────────────────────────────────────────────────────────────────── */

void notification_store_clear(void)
{
    if (!s_lock) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    s_head  = 0;
    memset(s_buf, 0, sizeof(s_buf));
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "CLEAR all notifications");
    notify_listener(NOTIF_EVENT_CLEARED, 0);
}

/* ─── Read API ─────────────────────────────────────────────────────────────── */

size_t notification_store_count(void)
{
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t c = s_count;
    xSemaphoreGive(s_lock);
    return c;
}

const notification_entry_t* notification_store_at(size_t idx)
{
    if (!s_lock) return NULL;
    /* No lock around the returned pointer — caller'a snapshot uyarısı header'da.
     * UI tek frame'de okuyup bitirir, race window ihmal edilebilir küçüklükte.
     * Eğer paranoid olursak: read-copy-update yapılabilir (yeni alloc döndür). */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (idx >= s_count) {
        xSemaphoreGive(s_lock);
        return NULL;
    }
    size_t phys = idx_logical_to_phys(idx);
    const notification_entry_t* p = &s_buf[phys];
    xSemaphoreGive(s_lock);
    return p;
}

const notification_entry_t* notification_store_get(uint32_t uid)
{
    if (!s_lock) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int phys = find_phys_by_uid(uid);
    const notification_entry_t* p = (phys >= 0) ? &s_buf[phys] : NULL;
    xSemaphoreGive(s_lock);
    return p;
}

void notification_store_add_listener(notif_store_listener_t cb, void* user_data)
{
    if (!cb) return;
    /* Duplicate check */
    for (size_t i = 0; i < NOTIF_LISTENERS_MAX; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].ud == user_data) return;
    }
    /* Boş slot bul */
    for (size_t i = 0; i < NOTIF_LISTENERS_MAX; i++) {
        if (!s_listeners[i].cb) {
            s_listeners[i].cb = cb;
            s_listeners[i].ud = user_data;
            return;
        }
    }
    ESP_LOGW(TAG, "add_listener: slots full (max %d)", NOTIF_LISTENERS_MAX);
}

void notification_store_remove_listener(notif_store_listener_t cb, void* user_data)
{
    for (size_t i = 0; i < NOTIF_LISTENERS_MAX; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].ud == user_data) {
            s_listeners[i].cb = NULL;
            s_listeners[i].ud = NULL;
            return;
        }
    }
}

/* Backward compat — eski single-listener API. */
void notification_store_set_listener(notif_store_listener_t cb, void* user_data)
{
    notification_store_add_listener(cb, user_data);
}
