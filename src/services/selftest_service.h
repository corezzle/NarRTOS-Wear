#pragma once
/**
 * @file selftest_service.h
 * @brief Açılış donanım checklist'i — sonuçları YALNIZ seri monitöre basar.
 *
 * narrtos-board-selftest projesindeki otomatik testlerin OS içi karşılığı.
 * Ekrana hiçbir şey çizmez; ekran ve dokunmatik OS'un kullanımında olduğu
 * için görsel/etkileşimli testler (renk, yüzey, butonlar) burada yok.
 *
 * Soğuk açılışta (reset/güç verme) bir kez, düşük öncelikli arka plan
 * görevinde çalışır. Deep sleep uyanışında ve sessiz sync uyanışında çalışmaz.
 * Beklentiler board varyantına göre değişir (AMOLED 2.06 / 1.8 / LCD128).
 */

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Checklist görevini başlatır (açılışta, görevler spawn edildikten sonra). */
esp_err_t selftest_service_start(void);

#ifdef __cplusplus
}
#endif
