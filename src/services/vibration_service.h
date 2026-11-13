/**
 * @file vibration_service.h
 * @brief Piezo titreşim sensörü (DFRobot Piezo Vibration Sensor V1.0) — dijital darbe sayacı.
 *
 * Modülün analog çıkışı PIEZO_PIN'e bağlanır; ESP girişi ~1.7 V üstünü "1" sayar,
 * yani sert titreşim/darbe anlarında kenar üretir. ADC kullanılmaz (NAR'da boş ADC1
 * pini yok; ADC2 WiFi ile çalışmaz). Çıktı: dakikadaki darbe sayısı → aktivite.
 * PIEZO_PIN tanımsız/-1 ise servis boş çalışır.
 */
#pragma once
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t vibration_service_init(void);
bool      vibration_service_present(void);      /* pin tanımlı mı */
uint32_t  vibration_service_taps_total(void);
uint16_t  vibration_service_taps_per_min(void);
uint32_t  vibration_service_since_last_s(void); /* son darbeden beri sn */
int       vibration_service_level(void);        /* pinin anlik seviyesi 0/1, pin yoksa -1 (teshis) */
/* Analog yakalama DENEYİ (modül anahtarı A konumunda): PIEZO_PIN geçici olarak ADC
 * girişi yapılır (yalnız ADC1 pinleri; NAR'da GPIO6 = ADC1_CH5), seconds boyunca hz
 * örnek/sn okunur, her 20 örnek "[cap] <t_ms> v v ..." satırı olarak emit'e verilir,
 * sonra pin dijital darbe girişine geri döner (sayaçlar sıfırlanır). Nabız/solunum/
 * mırlama dalga şekli araştırması için; darbe sayımı yakalama süresince durur. */
esp_err_t vibration_service_capture(int seconds, int hz, void (*emit)(const char*), int pull /* 0, -1=pd, +1=pu */);
#ifdef __cplusplus
}
#endif
