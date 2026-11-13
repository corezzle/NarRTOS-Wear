#pragma once
/**
 * @file weather_icon.h
 * @brief Dynamic weather icons rendered via LVGL primitives.
 *
 * Memory-lean alternative to bitmaps: each icon is drawn from a handful of
 * `lv_obj_t` primitives (circles, rounded rectangles, lines). Total RAM per
 * icon ≈ 1-3 small LVGL objects — no static image buffers.
 *
 * Usage:
 *   lv_obj_t* container = lv_obj_create(parent);   // ~56x56, clean style
 *   weather_icon_render(container, WEATHER_PARTLY_CLOUDY);
 *
 * Calling render() again clears the container and redraws with the new
 * condition — safe to call from event handlers.
 */
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEATHER_UNKNOWN       = 0,
    WEATHER_CLEAR         = 1,   // Güneşli
    WEATHER_PARTLY_CLOUDY = 2,   // Parçalı bulutlu
    WEATHER_CLOUDY        = 3,   // Kapalı
    WEATHER_FOG           = 4,   // Sisli
    WEATHER_RAIN          = 5,   // Yağmurlu
    WEATHER_SNOW          = 6,   // Karlı
    WEATHER_THUNDER       = 7,   // Gök gürültülü
    WEATHER_NIGHT_CLEAR   = 8,   // Açık / ay
    WEATHER_NIGHT_CLOUDY  = 9,   // Gece bulutlu
    WEATHER_CONDITION_COUNT
} weather_condition_t;

/**
 * Render a weather icon into @p container.
 * @param container an LVGL object to draw into. Will be cleared first.
 *                  Recommended size: square (56x56 ... 120x120).
 * @param cond      weather condition to draw.
 */
void weather_icon_render(lv_obj_t* container, weather_condition_t cond);

/**
 * Map an Open-Meteo WMO code to a weather_condition_t.
 * See https://open-meteo.com/en/docs (weather_code table).
 * @param is_night  if true, CLEAR/PARTLY_CLOUDY variants become NIGHT_* ones.
 */
weather_condition_t weather_icon_from_wmo(int wmo_code, bool is_night);

#ifdef __cplusplus
}
#endif
