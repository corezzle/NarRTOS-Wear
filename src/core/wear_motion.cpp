/**
 * @file wear_motion.cpp
 * @brief Apple Watch tarzı motion path implementations.
 */

#include "wear_motion.h"
#include <math.h>

/* LVGL'in lv_anim_t state'ten current/start/end değerleri çekme:
 *   a->act_time   — geçen süre (ms)
 *   a->duration   — toplam süre (ms)
 *   a->start_value — başlangıç değer
 *   a->end_value   — bitiş değer
 * Path callback dönüş değeri = current animated value (start..end arası interpolation).
 */

extern "C" {

int32_t wear_anim_path_apple_spring(const lv_anim_t* a)
{
    if (a->duration <= 0) return a->end_value;
    /* Normalized time [0..1] */
    float t = (float)a->act_time / (float)a->duration;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    /* Apple "default spring" yaklaşımı: damped sine wave with quick decay.
     *   y(t) = 1 - cos(omega*t) * exp(-decay*t)
     *   omega ≈ 7.5  → ~1.2 oscillation cycle ekran içinde
     *   decay ≈ 6.0  → 1. tepede %92 mesafe alınır, 2. tepede zar zor görünür
     * Bu kombinasyon yumuşak overshoot + snap-settle verir. */
    const float omega = 7.5f;
    const float decay = 6.0f;
    float v = 1.0f - cosf(omega * t) * expf(-decay * t);
    /* Clamp: aşırı overshoot izlerini önle (matematiksel olarak v ∈ [0, 1.05]) */
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    int32_t span = a->end_value - a->start_value;
    return a->start_value + (int32_t)((float)span * v);
}

int32_t wear_anim_path_back_out(const lv_anim_t* a)
{
    if (a->duration <= 0) return a->end_value;
    float t = (float)a->act_time / (float)a->duration;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    /* easeOutBack with mild overshoot (s=1.2, ~5% overshoot at 70% time):
     *   y(t) = 1 + (s+1)*(t-1)^3 + s*(t-1)^2
     */
    const float s = 1.20f;
    float t1 = t - 1.0f;
    float v = 1.0f + (s + 1.0f) * t1 * t1 * t1 + s * t1 * t1;
    /* Hafif overshoot doğal görünür, clamp etme. Ama upper bound koy ki
     * positional anim'lerde widget ekran dışına taşmasın. */
    if (v > 1.05f) v = 1.05f;
    if (v < 0.0f)  v = 0.0f;

    int32_t span = a->end_value - a->start_value;
    return a->start_value + (int32_t)((float)span * v);
}

int32_t wear_anim_path_ease_out_quint(const lv_anim_t* a)
{
    if (a->duration <= 0) return a->end_value;
    float t = (float)a->act_time / (float)a->duration;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    /* Quintic ease-out: 1 - (1-t)^5
     * Cubic'ten daha keskin başlangıç ve daha yumuşak yerleşme. */
    float u = 1.0f - t;
    float v = 1.0f - u * u * u * u * u;

    int32_t span = a->end_value - a->start_value;
    return a->start_value + (int32_t)((float)span * v);
}

/* ─── Press feedback ──────────────────────────────────────────────────────── */

/* Press feedback: OPACITY DIM yaklaşımı.
 * ──────────────────────────────────────────────────────────────────
 * İlk versiyon transform_scale kullanıyordu ama LVGL renderer eski
 * (büyük) bbox'ı clear etmeden yeni (küçük) bbox'ı çiziyor → kullanıcı
 * "ikon ikiye bölünüyor gibi" raporladı (eski outline + yeni outline
 * aynı anda görünüyor).
 *
 * Apple Watch'ın gerçek press feedback'i de aslında SCALE değil
 * OPACITY DIM (~%60 opa). Geometri değişmediği için bbox stable, LVGL
 * partial-redraw temiz çalışır, hiçbir artifact yok.
 *
 * Davranış:
 *   - PRESSED: 60 ms ease-out, opa 1.0 → 0.55 (belirgin koyu)
 *   - RELEASED: 140 ms apple_spring, opa current → 1.0 (snap-back)
 */

static void anim_opa_exec_cb(void* var, int32_t v)
{
    lv_obj_t* obj = (lv_obj_t*)var;
    /* style_opa: widget'ın TÜMÜNE (children dahil) opacity uygular,
     * bg_opa'dan farklı olarak içerik de dim olur. */
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void press_feedback_pressed_cb(lv_event_t* e)
{
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    if (!obj) return;
    lv_anim_delete(obj, anim_opa_exec_cb);

    int32_t cur_opa = lv_obj_get_style_opa(obj, 0);
    if (cur_opa <= 0) cur_opa = LV_OPA_COVER;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_exec_cb);
    /* v1.2.1 (2026-05-18): LV_OPA_60 → LV_OPA_70 (255→178). 60% dim
     * gradient ikonlarda (launcher renkli ikonlar) çok belirgin sönüş
     * yaratıyordu — "yarı kapanma" hissi. Apple Watch gerçek press dim
     * ~%75-80; 70% fark görsel ama "ölmüş" görünmüyor. */
    lv_anim_set_values(&a, cur_opa, LV_OPA_70);
    lv_anim_set_duration(&a, WEAR_ANIM_PRESS_DOWN);
    lv_anim_set_path_cb(&a, wear_anim_path_ease_out_quint);
    lv_anim_start(&a);
}

static void press_feedback_released_cb(lv_event_t* e)
{
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    if (!obj) return;
    lv_anim_delete(obj, anim_opa_exec_cb);

    int32_t cur_opa = lv_obj_get_style_opa(obj, 0);
    if (cur_opa <= 0) cur_opa = LV_OPA_70;   /* v1.2.1: 60 → 70 (press dim hizalı) */

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_exec_cb);
    lv_anim_set_values(&a, cur_opa, LV_OPA_COVER);   /* dim → full */
    lv_anim_set_duration(&a, WEAR_ANIM_PRESS_UP);
    lv_anim_set_path_cb(&a, wear_anim_path_apple_spring);
    lv_anim_start(&a);
}

void wear_press_feedback_attach(lv_obj_t* obj)
{
    if (!obj) return;

    /* Önceki transform_scale denemesinden kalan style'ları temizle. */
    lv_obj_remove_local_style_prop(obj, LV_STYLE_TRANSFORM_SCALE_X, 0);
    lv_obj_remove_local_style_prop(obj, LV_STYLE_TRANSFORM_SCALE_Y, 0);
    lv_obj_remove_local_style_prop(obj, LV_STYLE_TRANSFORM_SCALE_X, LV_STATE_PRESSED);
    lv_obj_remove_local_style_prop(obj, LV_STYLE_TRANSFORM_SCALE_Y, LV_STATE_PRESSED);

    /* Default state'te full opacity başlangıç. */
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    /* PRESSED state-style varsa kaldır — anim'in DEFAULT local override'ı
     * tüm state'lerde geçerli olsun. */
    lv_obj_remove_local_style_prop(obj, LV_STYLE_OPA, LV_STATE_PRESSED);

    lv_obj_add_event_cb(obj, press_feedback_pressed_cb,  LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(obj, press_feedback_released_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(obj, press_feedback_released_cb, LV_EVENT_PRESS_LOST, NULL);
}

} /* extern "C" */
