/**
 * @file wear_motion.h
 * @brief Apple Watch tarzı motion sistemi — spring path + standart süreler.
 *
 * LVGL'in default animation path'leri (linear, ease_in/out, overshoot)
 * matematiksel eğrilerdir; Apple'ın Core Animation spring physics'i ise
 * gerçek diferansiyel denklem çözümü. ESP32-S3 CPU bütçesinde tam spring
 * pahalı olduğu için, görsel olarak çok yakın bir "ease-out + hint of
 * elastic settle" hibrit eğri kullanıyoruz: hızlı başlangıç, hafif
 * overshoot, yumuşak yerleşme. Apple'ın standard "default spring"i ile
 * pratikte ayırt edilemez.
 *
 * Standart süreler (Apple HIG'den uyarlandı):
 *   FAST   100 ms — buton press feedback, küçük indicator move
 *   QUICK  180 ms — sub-page push/pop, modal overlay açılış
 *   NORMAL 260 ms — full-screen transition, app launch
 *   SLOW   380 ms — modal sheet present/dismiss, AOD geçiş
 *
 * Kullanım:
 *   lv_anim_set_path_cb(&a, wear_anim_path_apple_spring);
 *   lv_anim_set_duration(&a, WEAR_ANIM_QUICK);
 */

#pragma once

#include <lvgl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Standard durations (ms) ─────────────────────────────────────────────── */
#define WEAR_ANIM_FAST    100
#define WEAR_ANIM_QUICK   180
#define WEAR_ANIM_NORMAL  260
#define WEAR_ANIM_SLOW    380

/* Press feedback özel: çok kısa, snap-back hissi.
 * v1.2.1 (2026-05-18): PRESS_DOWN 60 → 50. Apple HIG "haptic immediacy"
 * budget < 50 ms; 60 ms'de ~4 frame interval kullanıcı parmağını
 * kaldırırken dim anim hâlâ devam ediyor → release anim cur_opa'dan
 * interpole eder → "ikon takıldı" hissi. 50 ms'de partial render mid-anim
 * yakalama riski biraz artar ama SW renderer için makul (60 FPS'te 3 frame). */
#define WEAR_ANIM_PRESS_DOWN  50   /* opa dim 1.0 → 0.7 */
#define WEAR_ANIM_PRESS_UP    140  /* spring back to 1.0 */

/* ─── Apple-style spring path ─────────────────────────────────────────────── */

/**
 * @brief Apple Watch standart spring eğrisi.
 *
 * Görsel davranış:
 *   - İlk %25 zamanda mesafenin %60'ını alır (snappy başlangıç)
 *   - %50 zamanda %92 mesafe (overshoot eşiğine yaklaşma)
 *   - %75 zamanda %103 mesafe (hafif overshoot — gözle zar zor görünür)
 *   - %100 zamanda tam yerleşme (decay)
 *
 * Matematiksel temel: damped oscillator approximation.
 *   y(t) = 1 + (overshoot - 1) * exp(-k*t) * (1 + k*t) * step
 *   Buradaki sabitler Apple HIG'in "default spring" preset'iyle eşleşir
 *   (damping=0.825, stiffness=180, mass=1.0).
 *
 * CPU bütçesi: float aritmetik + bir cosf çağrısı (~50 cycle ESP32-S3'te
 * FPU ile). Frame başına bir çağrı → ihmal edilebilir maliyet.
 */
int32_t wear_anim_path_apple_spring(const lv_anim_t* a);

/**
 * @brief Hafif overshoot (back-out): snappy slide için ideal.
 *
 * Spring'in daha hafif versiyonu. App switch, sub-view nav gibi sık
 * tetiklenen geçişlerde "natural physics" hissi verir ama overshoot çok
 * hafif (1.05×) olduğu için göz yormaz.
 */
int32_t wear_anim_path_back_out(const lv_anim_t* a);

/**
 * @brief Standard ease-out tuned curve — Apple "easeOut" eşleniği.
 *
 * Quintic (1 - (1-t)^5). LVGL'in default ease_out'u cubic (1 - (1-t)^3) →
 * son %30'da daha yavaş yerleşir. Quintic snap-then-glide hissi verir.
 */
int32_t wear_anim_path_ease_out_quint(const lv_anim_t* a);

/* ─── Press feedback (Apple Watch tap "tactility") ────────────────────────── */

/**
 * @brief Bir interaktif widget'a Apple Watch tarzı press feedback ekler.
 *
 * Davranış:
 *   - LV_EVENT_PRESSED: 60 ms ease-out ile scale 1.0× → 0.96× (hafif "basıldı")
 *   - LV_EVENT_RELEASED veya PRESS_LOST: 140 ms apple_spring ile 0.96× → 1.0×
 *     (yaylı geri dönüş, taptik yokluğunda görsel "tactility")
 *
 * Helper iç anim'leri yönetir, transform_scale'i lokal stil olarak set eder.
 * Önemli: çağrılmadan önce widget'ın PRESSED state'inde transform_scale
 * pinning'i OLMAMALI (varsa LVGL state-style önceliği press anim'ini ezer).
 * Helper ekleme sırasında her iki state için pinning'i kaldırır.
 *
 * Uyarı: Aynı widget'a iki kere ekleme — duplicate event_cb listesi oluşur.
 * Kullanım: yalnızca CLICKABLE leaf widget'lara (button, nav row, tile).
 * Container'a ekleme — child'lar press'i tüketmiyorsa parent fire eder; OK
 * ama performans için: gerçek tıklanan element'e ekle.
 */
void wear_press_feedback_attach(lv_obj_t* obj);

#ifdef __cplusplus
}
#endif
