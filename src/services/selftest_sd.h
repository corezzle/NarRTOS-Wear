#pragma once
/**
 * selftest_sd — SD kart bağla / yaz / oku / sil / ayır.
 * C dosyasında: ESP-IDF'in SDMMC yapılandırma makroları C'nin serbest sıralı
 * alan ilklendirmesini kullanıyor, C++'ta derlenmiyor.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dönüş: 0 = PASS, 1 = FAIL, 2 = SKIP (kart yok / cevap yok).
 * detail: seriye yazılacak kısa ASCII açıklama. */
int selftest_sd_run(char* detail, size_t n);

#ifdef __cplusplus
}
#endif
