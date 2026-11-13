#pragma once
/**
 * @brief Power Service - Battery monitoring and sleep state machine.
 *
 * State machine:
 *
 *   ACTIVE ──(inactivity 30s)──► DIM ──(5s)──► DISPLAY_OFF ──(inactivity 10min)──► DEEP_SLEEP
 *      ▲                          │                  │
 *      └──(touch/button/WOM)──────┘                  │
 *      └──(touch/button/WOM)─────────────────────────┘
 *
 * Deep sleep: esp_deep_sleep_start() with RTC wakeup (alarm) or EXT0 (button/WOM).
 */

#include <stdint.h>

typedef enum {
    POWER_STATE_ACTIVE       = 0,
    POWER_STATE_DIM          = 1,
    POWER_STATE_DISPLAY_OFF  = 2,
    POWER_STATE_DEEP_SLEEP   = 3,
} power_state_t;

/** Hangi olayın aktiviteyi tetiklediğini ayırt etmek için.
 *  Sahte uyandırmaları teşhis ederken log'a yazılır. */
typedef enum {
    POWER_WAKE_SRC_UNKNOWN     = 0,
    POWER_WAKE_SRC_TOUCH       = 1,  // ekran tap (ACTIVE iken normal etkileşim)
    POWER_WAKE_SRC_TOUCH_OFF   = 2,  // DISPLAY_OFF'tan ekran dokunuşuyla uyanma
    POWER_WAKE_SRC_TOUCH_DIM   = 3,  // DIM'den ekran dokunuşuyla uyanma
    POWER_WAKE_SRC_BTN_TOP     = 4,
    POWER_WAKE_SRC_BTN_BOTTOM  = 5,
    POWER_WAKE_SRC_WOM         = 6,  // wrist-up (ICM42670 INT1)
} power_wake_source_t;

void          power_service_init(void);
void          power_service_update(void);
power_state_t power_service_get_state(void);

/** Call on any user interaction to reset inactivity timer.
 *  @param src Tetikleyici kaynak — sahte wake teşhisi için log'a yazılır. */
void          power_service_user_activity(power_wake_source_t src);

/** Immediately enter deep sleep (e.g., on critical low battery). */
void          power_service_enter_deep_sleep(void);

/**
 * Deep sleep'e gir ama N saniye sonra RTC timer ile UYANDIRMA da aktif olsun.
 * Mevcut button (EXT1) wake source'larına ek olarak ESP_SLEEP_WAKEUP_TIMER da
 * tetiklenir. Sync wake feature'ı için kullanılır: cihaz periyodik uyanır,
 * WiFi açar, heartbeat + OTA check yapar, sonra tekrar uyur.
 *
 * @param seconds Uyumadan önceki uyanma süresi (saniye). 0 = sadece button wake.
 */
void          power_service_enter_deep_sleep_with_timer(uint32_t seconds);

/**
 * Mevcut boot, deep sleep timer wake'ten mi geliyor? main.cpp boot path'inde
 * handle_wakeup_reason set eder; sync_wake_task bunu kontrol edip OTA check
 * + tekrar sleep flow'unu çalıştırır.
 */
bool          power_service_is_sync_wake(void);

/**
 * v1.0.1 fix (Bug #4): Mevcut boot, alarm timer wake'ten mi geliyor?
 * Deep sleep'e girmeden önce bir sonraki alarm sync interval'dan erken ise
 * RTC slow memory'de işaretlenir. handle_wakeup_reason bunu okur ve sync
 * wake olarak işaretlemekten kaçınır → UI normal spawn olur → alarm fire.
 */
bool          power_service_is_alarm_wake(void);

/**
 * v1.0.1 fix (Bug #4): Alarm wake flag'ini oku ve temizle.
 * alarm_service_init kullanır: alarm wake ise s_last_min=-1 bırakır →
 * mevcut dakikadaki alarmı tick_cb yakalar; değilse mevcut dakikaya set
 * eder → geçmiş alarmları refire etmez.
 */
bool          power_service_consume_alarm_wake(void);

/* Session 34: kullanıcı parlaklık seviyesi — TEK KAYNAK.
 * Daha önce seviye settings_app içinde static duruyordu ve power_service her
 * uyanışta co5300_set_brightness(255) yazdığı için kullanıcının seçimi ilk
 * ekran kapanışında kayboluyordu. Artık burada tutulur, NVS'e yazılır;
 * Control Center ile Ayarlar aynı değeri paylaşır. 10-255'e kırpılır
 * (0 = ekran kapalı görünür, kasten yasak). */
void          power_service_set_brightness(uint8_t level);
uint8_t       power_service_get_brightness(void);

