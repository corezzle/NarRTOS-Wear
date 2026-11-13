/**
 * @file unistroke.h
 * @brief $1 Unistroke Recognizer — tek stroke'lu elyazısı karakter tanıma.
 *
 * Wobbrock et al., "Gestures without libraries, toolkits or training: a $1
 * recognizer for user interface prototypes" (UIST 2007) algoritması.
 *
 * Kullanım:
 *   1) Kullanıcı parmağıyla bir stroke çizer; press/move/release boyunca
 *      pozisyonlar (ekran veya widget-local coords, tutarlı olduğu sürece
 *      fark etmez) unistroke_point_t dizisine toplanır.
 *   2) Release anında unistroke_classify() çağrılır.
 *   3) Fonksiyon algoritmayı çalıştırır: resample → rotate → scale →
 *      translate → her templateçe "best angle" path distance → minimumu
 *      threshold ile kabul eder.
 *   4) Başarılıysa karakter, değilse 0 döner.
 *
 * Templates init_templates() ile ilk çağrıda lazı olarak üretilir; her
 * karakter matematiksel eğri/doğru üzerinden örneklenir ve normalize edilir.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t x;
    int16_t y;
} unistroke_point_t;

/**
 * Stroke'u en yakın template ile eşleştir ($1 tek-stroke unistroke
 * recognizer ile). Birden çok stroke verildiğinde de tek kesintisiz path
 * gibi işlenir — multi-stroke harflerde yanlış eşleşmeler olabilir, bu
 * sebeple multi-stroke girişler için unistroke_classify_multi() tercih
 * edilmeli (içeride geometrik sınıflandırıcı çalışır, gerekirse $1'e düşer).
 *
 * @param pts canvas-local (veya global, tutarlı) koordinatlarda ham nokta
 *            dizisi
 * @param n   nokta sayısı
 * @return    tanınan karakter veya 0 (belirsiz/ret)
 */
char unistroke_classify(const unistroke_point_t* pts, size_t n);

/**
 * Stroke yapısını bilen multi-stroke sınıflandırıcı. Stroke sayısı >= 2
 * ise önce geometrik analizle harf tipini tahmin eder (H / T / t / i / L
 * / x / k / + gibi stroke-sayısı + yön + konum ilişkisi netleşen harfler).
 * Eğer geometrik analiz karar veremezse $1'e düşer.
 *
 * Tek stroke verilirse direkt $1.
 *
 * @param pts            ardışık tüm noktalar (stroke'lar arasında tampon yok)
 * @param n              toplam nokta sayısı
 * @param stroke_starts  her stroke'un pts içindeki başlangıç indeksi
 * @param stroke_lens    her stroke'un nokta sayısı
 * @param n_strokes      stroke sayısı
 * @return               tanınan karakter veya 0
 */
char unistroke_classify_multi(const unistroke_point_t* pts, size_t n,
                              const uint16_t* stroke_starts,
                              const uint16_t* stroke_lens,
                              uint8_t n_strokes);

#ifdef __cplusplus
}
#endif
