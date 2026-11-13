/**
 * @file alarm_service.cpp
 * @brief Alarm scheduler implementation.
 *
 * Yapı:
 *   • s_slots[ALARM_MAX_COUNT] — runtime cache, ilk boot'ta NVS'ten doldurulur.
 *   • s_used[]                  — slot dolu mu?
 *   • s_pending_idx             — şu an ringing moduna geçirilmesi gereken
 *                                 alarm. UI pop_pending() ile tüketir.
 *   • s_snooze_epoch[]          — runtime-only one-shot snooze tetikleyicileri
 *                                 (her alarm slot'u için en fazla 1 snooze).
 *
 * 1Hz timer döngüsü:
 *   • localtime() oku → current_minute = HH*60+MM
 *   • last_minute != current_minute ise (yani dakika değişti):
 *       - Her slot için: enabled && (dow_mask=0 || (1<<wday)&dow_mask) &&
 *         (hh*60+mm == current_minute) → fire
 *       - Snooze listesi: epoch_now >= snooze_epoch ise → fire, snooze sıfırla
 *       - Fire: s_pending_idx=idx; bir kez EVENT_APP_SWITCH(APP_ID_ALARM)
 *         yayınla. dow_mask=0 ise enabled=false yap (NVS'e persist) — tek
 *         seferlik alarm tüketildi.
 *
 * Çakışma: aynı dakika içinde birden fazla alarm tetiklenirse en küçük idx
 * pending olur. Diğerleri kaybolur (gerçekçi: alarm zaten çalmış sayılır).
 *
 * NVS şeması:
 *   namespace "alarms"
 *   anahtar  "u"        u8  — used bitmask (8 alarm = 1 byte)
 *   anahtar  "s0".."s7" blob — alarm_nvs_t (paketlenmiş) sadece used=1 ise
 */

#include "alarm_service.h"
#include "../core/os_config.h"
#include "../core/event_bus.h"
#include "power_service.h"      // v1.0.1 fix (Bug #4): alarm wake consume
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <lvgl.h>
#include <string.h>
#include <time.h>

static const char* TAG = "AlarmSvc";

#define NVS_NS         "alarms"
#define NVS_KEY_USED   "u"

typedef struct __attribute__((packed)) {
    uint8_t hour;
    uint8_t minute;
    uint8_t dow_mask;
    uint8_t enabled;            /* bool */
    char    label[ALARM_LABEL_MAX];
} alarm_nvs_t;

static alarm_t       s_slots[ALARM_MAX_COUNT] = {};
static bool          s_used [ALARM_MAX_COUNT] = {};
static int           s_pending_idx            = -1;
static int           s_last_min               = -1;     /* HH*60+MM */
static lv_timer_t*   s_tick_timer             = NULL;

/* Runtime-only snooze tetikleyicileri (epoch saniye). 0 = inactive. */
static time_t        s_snooze_epoch[ALARM_MAX_COUNT] = {};

/* ─── NVS yardımcıları ────────────────────────────────────────────────────── */

static void nvs_save_used_mask(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t mask = 0;
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (s_used[i]) mask |= (uint8_t)(1u << i);
    }
    nvs_set_u8(h, NVS_KEY_USED, mask);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_slot(uint8_t idx)
{
    if (idx >= ALARM_MAX_COUNT) return;
    char key[4]; snprintf(key, sizeof(key), "s%u", (unsigned)idx);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    if (s_used[idx]) {
        alarm_nvs_t blob = {};
        blob.hour     = s_slots[idx].hour;
        blob.minute   = s_slots[idx].minute;
        blob.dow_mask = s_slots[idx].dow_mask;
        blob.enabled  = s_slots[idx].enabled ? 1 : 0;
        strncpy(blob.label, s_slots[idx].label, ALARM_LABEL_MAX - 1);
        blob.label[ALARM_LABEL_MAX - 1] = '\0';
        nvs_set_blob(h, key, &blob, sizeof(blob));
    } else {
        nvs_erase_key(h, key);   /* yoksa zaten ESP_ERR_NVS_NOT_FOUND, sorun değil */
    }
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGD(TAG, "NVS namespace yok, alarm cache boş");
        return;
    }

    uint8_t mask = 0;
    if (nvs_get_u8(h, NVS_KEY_USED, &mask) != ESP_OK) mask = 0;

    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        s_used[i] = ((mask >> i) & 1u) != 0;
        if (!s_used[i]) continue;

        char key[4]; snprintf(key, sizeof(key), "s%u", (unsigned)i);
        alarm_nvs_t blob = {};
        size_t sz = sizeof(blob);
        if (nvs_get_blob(h, key, &blob, &sz) != ESP_OK || sz != sizeof(blob)) {
            ESP_LOGW(TAG, "Slot %d bozuk, kaldırıldı", i);
            s_used[i] = false;
            continue;
        }
        s_slots[i].hour     = blob.hour;
        s_slots[i].minute   = blob.minute;
        s_slots[i].dow_mask = blob.dow_mask;
        s_slots[i].enabled  = blob.enabled != 0;
        memcpy(s_slots[i].label, blob.label, ALARM_LABEL_MAX);
        s_slots[i].label[ALARM_LABEL_MAX - 1] = '\0';
        ESP_LOGI(TAG, "  [%d] %02u:%02u dow=0x%02X en=%d \"%s\"",
                 i, blob.hour, blob.minute, blob.dow_mask, blob.enabled,
                 s_slots[i].label);
    }
    nvs_close(h);
}

/* ─── Public API ──────────────────────────────────────────────────────────── */

bool alarm_service_get(uint8_t idx, alarm_t* out)
{
    if (idx >= ALARM_MAX_COUNT || !s_used[idx]) return false;
    if (out) *out = s_slots[idx];
    return true;
}

bool alarm_service_slot_used(uint8_t idx)
{
    return idx < ALARM_MAX_COUNT && s_used[idx];
}

uint8_t alarm_service_used_count(void)
{
    uint8_t n = 0;
    for (int i = 0; i < ALARM_MAX_COUNT; i++) if (s_used[i]) n++;
    return n;
}

int alarm_service_find_free_slot(void)
{
    for (int i = 0; i < ALARM_MAX_COUNT; i++) if (!s_used[i]) return i;
    return -1;
}

void alarm_service_set(uint8_t idx, const alarm_t* a)
{
    if (idx >= ALARM_MAX_COUNT || !a) return;
    s_slots[idx]       = *a;
    s_slots[idx].label[ALARM_LABEL_MAX - 1] = '\0';
    s_slots[idx].hour   %= 24;
    s_slots[idx].minute %= 60;
    s_slots[idx].dow_mask &= 0x7F;
    s_used[idx]        = true;
    nvs_save_slot(idx);
    nvs_save_used_mask();
    ESP_LOGI(TAG, "Slot[%u] yazıldı: %02u:%02u dow=0x%02X en=%d",
             idx, s_slots[idx].hour, s_slots[idx].minute,
             s_slots[idx].dow_mask, s_slots[idx].enabled);
}

void alarm_service_remove(uint8_t idx)
{
    if (idx >= ALARM_MAX_COUNT) return;
    s_used[idx] = false;
    s_snooze_epoch[idx] = 0;
    memset(&s_slots[idx], 0, sizeof(alarm_t));
    nvs_save_slot(idx);   /* erase */
    nvs_save_used_mask();
    ESP_LOGI(TAG, "Slot[%u] silindi", idx);
}

void alarm_service_set_enabled(uint8_t idx, bool en)
{
    if (idx >= ALARM_MAX_COUNT || !s_used[idx]) return;
    if (s_slots[idx].enabled == en) return;
    s_slots[idx].enabled = en;
    nvs_save_slot(idx);
    ESP_LOGI(TAG, "Slot[%u] enabled=%d", idx, en);
}

int alarm_service_pop_pending(void)
{
    int v = s_pending_idx;
    s_pending_idx = -1;
    return v;
}

int64_t alarm_service_seconds_to_next(void)
{
    time_t now;
    time(&now);
    if (now < 1000000000) return -1;   /* zaman henüz geçerli değil */

    int64_t best = -1;
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (!s_used[i] || !s_slots[i].enabled) continue;

        /* Bir sonraki tetikleme saatini bul (snooze veya programlı) */
        time_t cand = 0;

        /* Snooze öncelikli — eğer ileride bir snooze varsa o */
        if (s_snooze_epoch[i] > now) {
            cand = s_snooze_epoch[i];
        } else if (s_slots[i].dow_mask == 0) {
            /* Tek seferlik: bugünün hh:mm — geçtiyse yarın */
            struct tm t; localtime_r(&now, &t);
            t.tm_hour = s_slots[i].hour;
            t.tm_min  = s_slots[i].minute;
            t.tm_sec  = 0;
            time_t target = mktime(&t);
            if (target <= now) target += 24 * 3600;
            cand = target;
        } else {
            /* Tekrarlayan: önümüzdeki 7 günü tara */
            for (int d = 0; d < 7; d++) {
                struct tm t; localtime_r(&now, &t);
                t.tm_hour = s_slots[i].hour;
                t.tm_min  = s_slots[i].minute;
                t.tm_sec  = 0;
                t.tm_mday += d;
                t.tm_isdst = -1;
                time_t target = mktime(&t);
                if (target < now) continue;
                struct tm tt; localtime_r(&target, &tt);
                if ((s_slots[i].dow_mask >> tt.tm_wday) & 1u) {
                    cand = target; break;
                }
            }
        }

        if (cand > 0) {
            int64_t delta = (int64_t)(cand - now);
            if (best < 0 || delta < best) best = delta;
        }
    }
    return best;
}

void alarm_service_snooze(uint8_t idx, uint8_t minutes)
{
    if (idx >= ALARM_MAX_COUNT || !s_used[idx]) return;
    time_t now;
    time(&now);
    s_snooze_epoch[idx] = now + (time_t)minutes * 60;
    ESP_LOGI(TAG, "Slot[%u] snooze +%u dk", idx, minutes);
}

/* ─── Scheduler tick (1Hz) ───────────────────────────────────────────────── */

static void fire_alarm(int idx)
{
    if (idx < 0 || idx >= ALARM_MAX_COUNT) return;
    s_pending_idx = idx;
    /* Tek seferlik alarmı tüketildi olarak işaretle */
    if (s_used[idx] && s_slots[idx].dow_mask == 0) {
        s_slots[idx].enabled = false;
        nvs_save_slot(idx);
    }
    ESP_LOGI(TAG, "FIRE slot[%d] %02u:%02u dow=0x%02X",
             idx, s_slots[idx].hour, s_slots[idx].minute,
             s_slots[idx].dow_mask);
    /* app_manager bu event'i yakalayıp alarm uygulamasını öne çıkarır */
    event_bus_publish(EVENT_APP_SWITCH, (uint32_t)APP_ID_ALARM);
}

static void tick_cb(lv_timer_t* t)
{
    (void)t;
    time_t now; time(&now);
    if (now < 1000000000) return;   /* time_service henüz set etmedi */
    struct tm ti; localtime_r(&now, &ti);

    int cur_min = ti.tm_hour * 60 + ti.tm_min;

    /* Snooze kontrolü her saniyede yapılır (dakika sınırına bağlı değil).
     *
     * Snooze her zaman koşulsuz tetiklenir; pending alarm zaten varsa
     * (RINGING ekranı açık) s_pending_idx yenisine yazılır → UI bir sonraki
     * tick'te yeni idx'i pop'lar. Çakışma kuralı: en küçük idx kazanır
     * (RINGING zaten açıksa kullanıcı uyanmış, UI değişmez — pop_pending
     * çağrıldığında yeni idx döner). */
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (s_snooze_epoch[i] && now >= s_snooze_epoch[i]) {
            time_t when = s_snooze_epoch[i];
            s_snooze_epoch[i] = 0;
            ESP_LOGI(TAG, "Snooze fire slot[%d] (epoch=%lld)", i, (long long)when);
            fire_alarm(i);   /* one-shot disable + APP_SWITCH publish */
        }
    }

    if (cur_min == s_last_min) return;
    s_last_min = cur_min;

    /* Dakika sınırı — programlı alarmları kontrol et */
    for (int i = 0; i < ALARM_MAX_COUNT; i++) {
        if (!s_used[i] || !s_slots[i].enabled) continue;
        int slot_min = s_slots[i].hour * 60 + s_slots[i].minute;
        if (slot_min != cur_min) continue;

        if (s_slots[i].dow_mask != 0) {
            /* Tekrarlayan: bugün maskede mi? */
            if (((s_slots[i].dow_mask >> ti.tm_wday) & 1u) == 0) continue;
        }
        if (s_pending_idx < 0) {
            fire_alarm(i);
            break;   /* aynı dakikada birden fazla varsa diğerlerini atla */
        }
    }
}

void alarm_service_init(void)
{
    memset(s_slots,        0, sizeof(s_slots));
    memset(s_used,         0, sizeof(s_used));
    memset(s_snooze_epoch, 0, sizeof(s_snooze_epoch));
    s_pending_idx = -1;
    s_last_min    = -1;

    nvs_load_all();

    /* v1.0.1 fix (Bug #4): Alarm wake mu?
     * Deep sleep'ten ALARM için uyandıysak (power_service flag set etmiş)
     * s_last_min'i -1 bırak → tick_cb mevcut dakikadaki alarmı yakalayıp
     * fire eder. Aksi halde (normal boot, buton wake, sync wake sonrası
     * UI'lı yeniden boot) mevcut dakikaya set → geçmiş alarmları refire
     * etmeyiz. */
    if (power_service_consume_alarm_wake()) {
        ESP_LOGI(TAG, "Boot: ALARM WAKE — tick_cb mevcut dakikayı tarayacak");
        /* s_last_min = -1 zaten yukarıda set edildi */
    } else {
        time_t now; time(&now);
        if (now > 1000000000) {
            struct tm ti; localtime_r(&now, &ti);
            s_last_min = ti.tm_hour * 60 + ti.tm_min;
        }
    }

    if (!s_tick_timer) {
        s_tick_timer = lv_timer_create(tick_cb, 1000, NULL);
    }
    ESP_LOGI(TAG, "Alarm service initialized (%u slot dolu)", alarm_service_used_count());
}
