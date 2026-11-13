#pragma once
/**
 * @brief Fiziksel buton yöneticisi.
 *
 * PIN_BTN_TOP ve PIN_BTN_BOTTOM için GPIO ISR tabanlı basma/uzun basma algılaması.
 *
 * Kısa basma  (<500ms)  → EVENT_BTN_TOP / EVENT_BTN_BOTTOM  (data=0)
 * Uzun basma  (≥500ms)  → EVENT_BTN_TOP / EVENT_BTN_BOTTOM  (data=1)
 *
 * ISR debounce: 50ms hardware filter (GPIO_INTR_NEGEDGE + timer).
 * Aynı zamanda power_service_user_activity() çağrılır → ekran uyandırma.
 */

#include <esp_err.h>

esp_err_t button_handler_init(void);

/** Glitch sayaçlarını al ve sıfırla (atomik). Diagnostic — periyodik
 *  olarak power_service_update'te çağrılır ve log'a basılır. */
void button_handler_drain_glitch_counts(uint32_t* out_top, uint32_t* out_bottom);
