#pragma once
#include <stdint.h>

void     sensor_service_init(void);
void     sensor_service_update(void);   // 20Hz döngüsünde çağrılır
uint32_t sensor_service_get_steps(void);
uint8_t  sensor_service_get_activity(void);      // APEX: 0 bilinmiyor/durağan, 1 yürüme, 2 koşu
uint32_t sensor_service_apex_event_count(void);  // açılıştan beri işlenen APEX kesmesi
