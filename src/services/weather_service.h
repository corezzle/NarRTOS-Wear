#pragma once
/**
 * @file weather_service.h
 * @brief Gerçek zamanlı hava durumu servisi (IP geolocation + Open-Meteo).
 *
 * Çoklu şehir desteği (2026-05-05): kullanıcı Apple Watch tarzı bir liste
 * menüsünden farklı şehre geçebiliyor (Auto/Ankara/Izmir). Her şehrin kendi
 * snapshot'ı var; "primary" idx kalıcı (NVS) ve weather_service_get() o şehrin
 * verisini döndürüyor. Boot'ta tüm şehirlerin son cache'i RAM'e yükleniyor →
 * WiFi olmasa da tüm kartlar dolu görünüyor.
 *
 * Düşük RAM tüketimli tasarım:
 *   - Tek FreeRTOS görev (Core 1, 6KB stack, priority 2)
 *   - 15 dakikada bir yenileme: tüm şehirler sırayla çekilir
 *   - JSON parse etmek yerine strstr tabanlı hafif çıkarım (cJSON yok)
 *   - HTTP response buffer'ı heap'te 3KB
 *   - Tüm cache (lokasyon + snapshot) NVS'te kalıcı
 *
 * Yayın:
 *   EVENT_WEATHER_UPDATE (0x32), data = weather_condition_t (uint8_t)
 *   (Primary şehir güncellendiğinde veya değiştirildiğinde tetiklenir.)
 */

#include "../assets/weather_icon.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    weather_condition_t condition;   // İkon için (override'lı: storm/rain)
    int                 wmo_code;    // Ham Open-Meteo WMO kodu (açıklama için)
    float               temperature; // Anlık °C
    float               temp_max;    // Günlük yüksek °C
    float               temp_min;    // Günlük düşük °C
    int                 humidity;    // 0..100 %
    float               wind_speed;  // km/h (10 m)
    int                 wind_dir;    // Derece, 0=Kuzey, 90=Doğu (rüzgârın geldiği yön)
    float               uv_index;    // Günlük max UV (0..~11)
    bool                is_night;
    bool                valid;       // true → en az bir başarılı çekim yapıldı
    uint32_t            updated_at_s;// unix saniye (son güncelleme)
    char                city[32];    // "Istanbul", "Ankara" ...
} weather_data_t;

/** Şehir indeksleri. Genişletmek için: enum'a yeni şehir ekle, .cpp'deki
 *  s_cities[] dizisine lat/lon/name yaz, WEATHER_CITY_COUNT'ı artır. */
typedef enum {
    WEATHER_CITY_AUTO   = 0,   /* IP-tabanlı (kullanıcının bulunduğu yer) */
    WEATHER_CITY_ANKARA = 1,   /* 39.9334, 32.8597 */
    WEATHER_CITY_IZMIR  = 2,   /* 38.4192, 27.1287 */
} weather_city_idx_t;

#define WEATHER_CITY_COUNT  3

/** Başlat (idempotent). Wi-Fi bağlı değilse görev boşta bekler. */
void  weather_service_init(void);

/** Aktif (primary) şehrin son verisini kopyala. @return veri geçerliyse true. */
bool  weather_service_get(weather_data_t* out);

/** Belirli bir şehrin son verisini kopyala (menü kartları için). */
bool  weather_service_get_city(int idx, weather_data_t* out);

/** Şehrin görünür adı (UI menüsü). Auto için IP-detect adı (yoksa "Sehir"). */
const char* weather_service_get_city_name(int idx);

/** Aktif (primary) şehrin idx'i. NVS'te kalıcı, default WEATHER_CITY_AUTO. */
int  weather_service_get_primary_idx(void);

/** Aktif şehri değiştir. NVS'e yazar, EVENT_WEATHER_UPDATE publish eder,
 *  fetch task'i bilgilendirir → en güncel veriyi anında tazeler. */
void weather_service_set_primary_idx(int idx);

/** Zorla yeniden çekim isteği gönder. Görev uyanır, bir sonraki tick'te çeker. */
void  weather_service_refresh_now(void);

/**
 * WMO → kısa Türkçe açıklama (ASCII karakterli — Montserrat fontunda
 * Türkçe glyph yok, ç/ı/ş yerine c/i/s kullanıldı).
 * Örn: 0 → "Acik", 1 → "Az Bulutlu", 2 → "Parcali Bulutlu" vs.
 */
const char* weather_desc_from_wmo(int wmo, bool is_night);

#ifdef __cplusplus
}
#endif
