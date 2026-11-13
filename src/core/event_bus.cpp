/**
 * @file event_bus.cpp
 * @brief Inter-task event bus implementation.
 *
 * Design:
 *  - Her event type için maksimum MAX_SUBS_PER_EVENT abone tutulur.
 *  - publish() her abonenin kuyruğuna xQueueSend ile gönderir (non-blocking).
 *    Kuyruk doluysa event drop edilir ve uyarı loglanır — kritik event'ler için
 *    kuyruk boyutunu artırmak gerekir.
 *  - publish_from_isr() IRAM'da çalışır, xQueueSendFromISR kullanır.
 *  - Mutex ile subscribe thread-safe'dir; publish lock almaz (performans öncelikli).
 */

#include "event_bus.h"
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>
#include <atomic>          /* v1.4.55 audit #2c: drop counter */

static const char* TAG = "EventBus";

/* v1.4.55 (2026-05-19) — audit orta öncelik #2c: queue overflow telemetry.
 * Önceden ISR publish dropp olduğunda hiçbir bilgi yoktu (ISR'dan log basmak
 * unsafe). Normal publish'te log vardı ama counter yoktu → "kaç event drop
 * oldu" sorusuna cevap yoktu. İki atomic counter:
 *   - s_isr_drop_count: ISR publish overflow (lock-free, IRAM güvenli)
 *   - s_publish_drop_count: normal publish overflow (log + counter)
 * event_bus_get_drop_stats() public API ile debug/health-check dump için. */
static std::atomic<uint32_t> s_isr_drop_count{0};
static std::atomic<uint32_t> s_publish_drop_count{0};

// ─── Konfigürasyon ────────────────────────────────────────────────────────────
#define MAX_SUBS_PER_EVENT  4    // Bir event type'ına max abone sayısı
#define QUEUE_DEPTH         8    // Her abone kuyruğunun kapasitesi

// ─── Veri Yapıları ────────────────────────────────────────────────────────────

typedef struct {
    QueueHandle_t queues[MAX_SUBS_PER_EVENT];
    uint8_t       count;
} sub_list_t;

static sub_list_t   s_subs[EVENT_MAX];
static SemaphoreHandle_t s_mutex = NULL;

// ─── Yardımcı: timestamp ─────────────────────────────────────────────────────

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ─── event_bus_init ───────────────────────────────────────────────────────────

void event_bus_init(void)
{
    memset(s_subs, 0, sizeof(s_subs));
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);
    ESP_LOGI(TAG, "Event bus initialized (%d event types)", EVENT_MAX);
}

// ─── event_bus_subscribe ─────────────────────────────────────────────────────

QueueHandle_t event_bus_subscribe(event_type_t type)
{
    if (type >= EVENT_MAX) {
        ESP_LOGE(TAG, "subscribe: invalid type %d", type);
        return NULL;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    sub_list_t* list = &s_subs[type];
    if (list->count >= MAX_SUBS_PER_EVENT) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "subscribe: max subscribers reached for type %d", type);
        return NULL;
    }

    QueueHandle_t q = xQueueCreate(QUEUE_DEPTH, sizeof(os_event_t));
    if (!q) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "subscribe: failed to create queue");
        return NULL;
    }

    list->queues[list->count++] = q;

    xSemaphoreGive(s_mutex);
    ESP_LOGD(TAG, "subscribed to event %d (total subs: %d)", type, list->count);
    return q;
}

// ─── event_bus_publish ───────────────────────────────────────────────────────

void event_bus_publish(event_type_t type, uint32_t data)
{
    if (type >= EVENT_MAX) return;

    const os_event_t event = {
        .type         = type,
        .data         = data,
        .timestamp_ms = now_ms(),
    };

    // publish için mutex almıyoruz — count ve pointer okuma atomik değil ama
    // subscribe çok nadir, publish sık: dirty-read riski minimumdur.
    // Eğer runtime subscribe/unsubscribe gerekirse burada lock ekle.
    const sub_list_t* list = &s_subs[type];

    for (uint8_t i = 0; i < list->count; i++) {
        if (xQueueSend(list->queues[i], &event, 0) != pdTRUE) {
            s_publish_drop_count.fetch_add(1, std::memory_order_relaxed);   /* audit #2c */
            ESP_LOGW(TAG, "event %d dropped for subscriber %d (queue full)", type, i);
        }
    }
}

// ─── event_bus_publish_from_isr ──────────────────────────────────────────────

void IRAM_ATTR event_bus_publish_from_isr(event_type_t type, uint32_t data)
{
    if (type >= EVENT_MAX) return;

    const os_event_t event = {
        .type         = type,
        .data         = data,
        .timestamp_ms = 0,   // esp_timer çağrısı ISR'da güvensiz, 0 bırak
    };

    BaseType_t higher_woken = pdFALSE;
    const sub_list_t* list = &s_subs[type];

    for (uint8_t i = 0; i < list->count; i++) {
        /* v1.4.55 audit #2c: drop telemetry. ISR'dan log basamayız ama
         * atomic counter IRAM-safe — fetch_add relaxed yeterli (sayım,
         * sırası önemli değil). event_bus_get_drop_stats() ile sonradan
         * normal task context'inden okunur. */
        if (xQueueSendFromISR(list->queues[i], &event, &higher_woken) != pdTRUE) {
            s_isr_drop_count.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (higher_woken) {
        portYIELD_FROM_ISR();
    }
}

/* v1.4.55 audit #2c: public API — debug/health check için drop counter dump.
 * out_isr / out_publish NULL geçilebilir; sadece ihtiyaç duyulan okunur. */
void event_bus_get_drop_stats(uint32_t* out_isr, uint32_t* out_publish)
{
    if (out_isr)     *out_isr     = s_isr_drop_count.load(std::memory_order_relaxed);
    if (out_publish) *out_publish = s_publish_drop_count.load(std::memory_order_relaxed);
}

// ─── event_bus_receive ───────────────────────────────────────────────────────

bool event_bus_receive(QueueHandle_t queue, os_event_t* out_event, uint32_t timeout_ms)
{
    return xQueueReceive(queue, out_event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
