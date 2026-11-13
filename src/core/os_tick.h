#pragma once
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/** LVGL custom tick kaynağı. FreeRTOS tick'e bağlı ms cinsinden zaman. */
static inline uint32_t os_tick_get_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}
