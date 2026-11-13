#pragma once
/**
 * @file notification_store.h
 * @brief In-memory store for phone notifications (mirrored via BLE/ANCS).
 *
 * Single producer (BLE callback task) + single consumer (UI/LVGL task).
 * Thread-safe via mutex. Fixed-capacity ring buffer — yeni notification
 * geldiğinde en eski overwrite olur (FIFO eviction).
 *
 * Notification source: Apple Notification Center Service (ANCS) — iPhone
 * üzerinde çalışan herhangi bir uygulamanın (WhatsApp, Messages, Mail, vb.)
 * push bildirimini saata yansıtır.
 *
 * Lifecycle:
 *   1. ANCS notification arrived event → notification_store_add()
 *   2. UI app her açılışta notification_store_count() + iterate
 *   3. Kullanıcı dismisses → notification_store_remove() + ANCS dismiss
 *   4. Boot'ta liste boş — bildirimler persist edilmez (telefon zaten kaynak)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ANCS Category IDs — Apple spec'inden, app type'ını gruplar */
typedef enum {
    NOTIF_CAT_OTHER         = 0,
    NOTIF_CAT_INCOMING_CALL = 1,
    NOTIF_CAT_MISSED_CALL   = 2,
    NOTIF_CAT_VOICEMAIL     = 3,
    NOTIF_CAT_SOCIAL        = 4,   /* WhatsApp, Telegram, Messenger, Twitter — bizim asıl ilgi alanımız */
    NOTIF_CAT_SCHEDULE      = 5,
    NOTIF_CAT_EMAIL         = 6,
    NOTIF_CAT_NEWS          = 7,
    NOTIF_CAT_HEALTH        = 8,
    NOTIF_CAT_BUSINESS      = 9,
    NOTIF_CAT_LOCATION      = 10,
    NOTIF_CAT_ENTERTAINMENT = 11,
    NOTIF_CAT_COUNT
} notif_category_t;

/* Tek bir notification entry'si. ANCS attributes:
 *   AppIdentifier: bundle ID, örn. "net.whatsapp.WhatsApp"
 *   Title:         genelde sender ismi (kontakt)
 *   Subtitle:      bazı app'lerde grup adı
 *   Message:       gerçek mesaj metni
 *   Date:          ISO 8601 string ("20260608T143025") — biz parse ederiz */
typedef struct {
    uint32_t           uid;           /* ANCS Notification UID (4 byte) */
    notif_category_t   category;
    char               app_id[64];    /* bundle ID */
    char               title[64];     /* sender / source */
    char               subtitle[64];  /* group / room */
    char               message[256];  /* notification body */
    int64_t            received_us;   /* esp_timer_get_time() at arrival */
    bool               is_silent;     /* ANCS EventFlags.Silent bit */
    bool               is_important;  /* ANCS EventFlags.Important bit */
    bool               has_positive_action; /* "Answer" gibi action var mı */
    bool               has_negative_action; /* "Decline" gibi action var mı */
} notification_entry_t;

/* Max simultaneous notifications. Sentinel — boy by boy unlimited değil. */
#define NOTIF_STORE_CAPACITY  20

/* Subscribe callback — yeni notification eklendi/silindi/güncellendi. UI
 * tarafı bu callback'i alır ve liste rebuild eder. Callback BLE task'tan
 * çağrılabilir → UI tarafı LVGL'i lv_async_call ile güncellemeli. */
typedef enum {
    NOTIF_EVENT_ADDED    = 0,
    NOTIF_EVENT_MODIFIED = 1,
    NOTIF_EVENT_REMOVED  = 2,
    NOTIF_EVENT_CLEARED  = 3,   /* tüm liste temizlendi (disconnect, vb.) */
} notif_event_type_t;

typedef void (*notif_store_listener_t)(notif_event_type_t evt, uint32_t uid, void* user_data);

/* ─── Module API ──────────────────────────────────────────────────────────── */

/* Init — boot'ta bir kez. Mutex create, ring buffer reset. */
void notification_store_init(void);

/* Yeni notification ekle (veya aynı uid varsa update). is_modify=true ise
 * ANCS Modify event (örn mesaj edit edildi); false ise Add (yeni geldi). */
void notification_store_add(const notification_entry_t* entry, bool is_modify);

/* Belirli bir UID'i kaldır (ANCS Remove event veya kullanıcı dismiss). */
void notification_store_remove(uint32_t uid);

/* Tüm listeyi temizle (disconnect, factory reset, kullanıcı "Clear All"). */
void notification_store_clear(void);

/* Read API — UI tarafı listeyi render etmek için.
 * count: o anki notification sayısı (0..NOTIF_STORE_CAPACITY).
 * at(idx): sıralı erişim, idx=0 en yeni. NULL döner index out of range.
 * Pointer kısa ömürlü; UI bir frame içinde kopyalayıp bitirsin. */
size_t                          notification_store_count(void);
const notification_entry_t*     notification_store_at(size_t idx);

/* UID ile spesifik notification al (örn. detail page açılırken). */
const notification_entry_t*     notification_store_get(uint32_t uid);

/* Listener register/unregister — UI rebuild trigger için.
 *
 * v1.4.70: Çoklu listener desteği (max NOTIF_LISTENERS_MAX). Banner service
 * (global popup) + Notifications app (liste) aynı anda dinleyebilir.
 *
 * add: aynı (cb,user_data) çifti varsa duplicate eklenmez. Slot doluysa
 *      uyarı log'lanır, register fail (uygulamada nadir — 4 listener yeter).
 * remove: (cb,user_data) çiftine göre kaldırır. */
#define NOTIF_LISTENERS_MAX 4

void notification_store_add_listener(notif_store_listener_t cb, void* user_data);
void notification_store_remove_listener(notif_store_listener_t cb, void* user_data);

/* Backward compat — eski single-listener API. add_listener'a forward. */
void notification_store_set_listener(notif_store_listener_t cb, void* user_data);

#ifdef __cplusplus
}
#endif
