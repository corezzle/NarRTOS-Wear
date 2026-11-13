#pragma once
/**
 * @file alarm_service.h
 * @brief Wear-OS alarm scheduler + persistence katmanı.
 *
 * Sorumluluklar:
 *   • NVS'te en fazla ALARM_MAX_COUNT alarm slot'u tutar.
 *   • Her alarm için saat, dakika, gün-maskesi (haftanın günleri),
 *     enabled flag'i ve kısa bir etiket persiste edilir.
 *   • 1Hz LVGL timer ile localtime() okunur, dakika sınırı geçildiğinde
 *     pasif olmayan alarmlar arasında "şu an çalması gereken" varsa
 *     pending olarak işaretlenip EVENT_APP_SWITCH(APP_ID_ALARM)
 *     event'i yayınlanır → app_manager alarm uygulamasını ön plana
 *     çıkarır, alarm uygulaması da ringing moduna geçer.
 *   • Tek seferlik alarmlar (dow_mask=0) çaldıktan sonra otomatik
 *     enable=false yapılır; tekrarlayan alarmlar enabled kalır.
 *
 * Thread-safety:
 *   Tüm fonksiyonlar UI task'ından (Core 0) çağrılmalı. NVS yazıları
 *   alarm_service_set/remove içinde senkron yapılır (kısa, bloklayıcı).
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_MAX_COUNT     8
#define ALARM_LABEL_MAX     16   /* '\0' dahil */

/* dow_mask bit haritası — Pazar=bit0 ... Cumartesi=bit6 (POSIX wday) */
#define ALARM_DOW_SUN  (1u << 0)
#define ALARM_DOW_MON  (1u << 1)
#define ALARM_DOW_TUE  (1u << 2)
#define ALARM_DOW_WED  (1u << 3)
#define ALARM_DOW_THU  (1u << 4)
#define ALARM_DOW_FRI  (1u << 5)
#define ALARM_DOW_SAT  (1u << 6)
#define ALARM_DOW_WEEKDAYS  (ALARM_DOW_MON|ALARM_DOW_TUE|ALARM_DOW_WED|ALARM_DOW_THU|ALARM_DOW_FRI)
#define ALARM_DOW_WEEKENDS  (ALARM_DOW_SAT|ALARM_DOW_SUN)
#define ALARM_DOW_DAILY     0x7Fu

typedef struct {
    uint8_t  hour;     /* 0..23 */
    uint8_t  minute;   /* 0..59 */
    uint8_t  dow_mask; /* 0=tek seferlik, aksi halde haftanın günleri  */
    bool     enabled;
    char     label[ALARM_LABEL_MAX];
} alarm_t;

/* Servisi NVS'ten yükler ve 1Hz scheduler timer'ını başlatır. */
void alarm_service_init(void);

/* UI tarafından kullanılır: indekse göre alarmı doldurur. used=true ise
 * slot dolu, false ise boş slot. */
bool alarm_service_get(uint8_t idx, alarm_t* out);
bool alarm_service_slot_used(uint8_t idx);
uint8_t alarm_service_used_count(void);

/* idx'e yaz (used=true). NVS'e persist eder. */
void alarm_service_set(uint8_t idx, const alarm_t* a);

/* Slot'u sil (used=false). NVS'ten kaldırır. */
void alarm_service_remove(uint8_t idx);

/* Sadece enabled flag'ini değiştirir (NVS persist). */
void alarm_service_set_enabled(uint8_t idx, bool en);

/* İlk boş slotu döndürür, yoksa -1. */
int alarm_service_find_free_slot(void);

/* Pending (henüz tüketilmemiş) tetiklenmiş alarm idx'i; yoksa -1.
 * UI bunu çağırıp tüketir → alarm_app on_start'ta ringing moduna
 * geçeceğini anlar. */
int alarm_service_pop_pending(void);

/* UI bilgilendirme: mevcut zamandan en yakın aktif alarma kalan saniye.
 * Hiç aktif alarm yoksa -1. */
int64_t alarm_service_seconds_to_next(void);

/* Snooze: verilen alarmı şu an'dan +minutes dakikası için tek seferlik
 * tekrar pending'e ekler. NVS'i değiştirmez (sadece runtime). */
void alarm_service_snooze(uint8_t idx, uint8_t minutes);

#ifdef __cplusplus
}
#endif
