/**
 * @file weather_icon.cpp
 * @brief LVGL primitive drawings for weather conditions.
 *
 * Each icon fits into a container that is assumed to be roughly square.
 * We compute sizes relative to the container's current width so the icons
 * automatically scale when the background circle changes size (the activity
 * watchface zooms the background when switching modes).
 */

#include "weather_icon.h"
#include <string.h>
#include <math.h>

// ─── Palette (Apple Watch-like) ──────────────────────────────────────────────

static const lv_color_t C_SUN      = LV_COLOR_MAKE(0xFF, 0xC8, 0x3D);
static const lv_color_t C_CLOUD    = LV_COLOR_MAKE(0xE8, 0xEB, 0xEE);
static const lv_color_t C_CLOUD_D  = LV_COLOR_MAKE(0xA7, 0xAE, 0xB8);
static const lv_color_t C_RAIN     = LV_COLOR_MAKE(0x4A, 0xA8, 0xFF);
static const lv_color_t C_SNOW     = LV_COLOR_MAKE(0xF0, 0xF6, 0xFF);
static const lv_color_t C_BOLT     = LV_COLOR_MAKE(0xFF, 0xD2, 0x3C);
static const lv_color_t C_MOON     = LV_COLOR_MAKE(0xE6, 0xEA, 0xF2);
static const lv_color_t C_FOG      = LV_COLOR_MAKE(0xC8, 0xCE, 0xD6);
static const lv_color_t C_UNKNOWN  = LV_COLOR_MAKE(0x7E, 0x86, 0x90);

// ─── Helpers ─────────────────────────────────────────────────────────────────

/** Create a solid-colored circle of diameter d at (x,y) relative to parent.
 *  (x,y) = center; the function offsets by d/2 internally. */
static lv_obj_t* prim_circle(lv_obj_t* parent, int x, int y, int d, lv_color_t color)
{
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, d, d);
    lv_obj_set_pos(c, x - d / 2, y - d / 2);
    lv_obj_set_style_bg_color(c, color, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/** Rounded rectangle (for cloud bodies / fog strokes). (x,y) = top-left. */
static lv_obj_t* prim_rect(lv_obj_t* parent, int x, int y, int w, int h, int r, lv_color_t color)
{
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, color, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, r, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/** Sun-ray line segment centered on (cx,cy) at angle deg, inner/outer radius. */
static void prim_ray(lv_obj_t* parent, int cx, int cy, float angle_deg,
                     int r_inner, int r_outer, int thickness, lv_color_t color)
{
    // LVGL stores line points in a static array; allocate via static lifetime.
    // To keep each call self-contained we lean on lv_obj_t style-line via
    // lv_line + heap-allocated points kept alive by LVGL's user_data.
    typedef struct { lv_point_precise_t p[2]; } ray_pts_t;
    ray_pts_t* pts = (ray_pts_t*)lv_malloc(sizeof(ray_pts_t));
    if (!pts) return;

    float a = angle_deg * 3.14159265f / 180.0f;
    pts->p[0].x = cx + (int)(r_inner * cosf(a));
    pts->p[0].y = cy + (int)(r_inner * sinf(a));
    pts->p[1].x = cx + (int)(r_outer * cosf(a));
    pts->p[1].y = cy + (int)(r_outer * sinf(a));

    lv_obj_t* line = lv_line_create(parent);
    lv_line_set_points(line, pts->p, 2);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_width(line, thickness, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_user_data(line, pts);
    // Free the points buffer when the line is deleted.
    lv_obj_add_event_cb(line, [](lv_event_t* e){
        lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
        void* ud = lv_obj_get_user_data(target);
        if (ud) lv_free(ud);
    }, LV_EVENT_DELETE, NULL);
}

// ─── Icon drawers ────────────────────────────────────────────────────────────
// Each drawer receives the container and its normalized dimension `s` (size).
// All coordinates are computed as fractions of `s` so the icons scale cleanly.

static void draw_clear(lv_obj_t* c, int s)
{
    int cx = s / 2, cy = s / 2;
    int sun_r = (s * 22) / 100;   // 22% diameter factor
    // Rays
    int r_in  = sun_r + s / 10;
    int r_out = sun_r + s / 4;
    int thick = (s * 7) / 100; if (thick < 2) thick = 2;
    for (int i = 0; i < 8; ++i) {
        prim_ray(c, cx, cy, i * 45.0f, r_in, r_out, thick, C_SUN);
    }
    // Sun disk
    prim_circle(c, cx, cy, sun_r * 2, C_SUN);
}

/** Cloud body built from 3 circles + a rounded rect base. */
static void draw_cloud_body(lv_obj_t* c, int cx, int cy, int w,
                            lv_color_t cloud_color)
{
    // w = cloud overall width; height ~ w/2
    int h = w / 2;
    // Base rectangle
    prim_rect(c, cx - w/2, cy, w, h/2, h/4, cloud_color);
    // Left bump (smaller)
    prim_circle(c, cx - w/4, cy + h/10, (h*3)/4, cloud_color);
    // Right bump (biggest)
    prim_circle(c, cx + w/6, cy - h/10, h, cloud_color);
    // Front small bump
    prim_circle(c, cx - w/2 + h/3, cy + h/5, (h*3)/5, cloud_color);
}

static void draw_partly_cloudy(lv_obj_t* c, int s)
{
    // Apple Watch tarzı: üst-sağda büyük güneş + ışınları,
    // önde geniş beyaz bulut güneşin alt-solunu örtüyor.
    int cx = s / 2, cy = s / 2;

    // Güneş — üst-sağda, büyük; ışınlar canvas içinde kalacak şekilde
    // sun_r ve r_out ayarlandı (s=95'te en uçtaki ışın ~92 px'e ulaşır).
    int sun_r = (s * 20) / 100;            // disk yarıçapı
    int sx    = cx + (s * 16) / 100;       // sağa kaydır
    int sy    = cy - (s * 18) / 100;       // yukarı kaydır
    int r_in  = sun_r + (s * 5)  / 100;    // ışın iç çap
    int r_out = sun_r + (s * 13) / 100;    // ışın dış çap
    int thick = (s * 8) / 100; if (thick < 2) thick = 2;
    for (int i = 0; i < 8; ++i) {
        prim_ray(c, sx, sy, i * 45.0f, r_in, r_out, thick, C_SUN);
    }
    prim_circle(c, sx, sy, sun_r * 2, C_SUN);

    // Bulut — geniş, alt-merkezde, güneşin alt-solunu örter (sun'dan SONRA
    // çizildiği için üstte kalır). Daha beyaz tonu için override.
    static const lv_color_t C_CLOUD_WHITE = LV_COLOR_MAKE(0xFA, 0xFB, 0xFC);
    int cloud_w  = (s * 78) / 100;
    int cloud_cx = cx - (s * 6) / 100;
    int cloud_cy = cy + (s * 14) / 100;
    draw_cloud_body(c, cloud_cx, cloud_cy, cloud_w, C_CLOUD_WHITE);
}

static void draw_cloudy(lv_obj_t* c, int s)
{
    int cx = s / 2, cy = s / 2;
    // Back cloud (darker, offset up-left)
    draw_cloud_body(c, cx - s/10, cy - s/10, (s * 60) / 100, C_CLOUD_D);
    // Front cloud (lighter)
    draw_cloud_body(c, cx + s/12, cy + s/12, (s * 62) / 100, C_CLOUD);
}

static void draw_fog(lv_obj_t* c, int s)
{
    int bar_h = (s * 9) / 100; if (bar_h < 2) bar_h = 2;
    int gap   = (s * 12) / 100;
    int y0    = s / 4;
    // 4 horizontal bars, slightly varying widths for texture
    int widths[4] = { (s * 80) / 100, (s * 70) / 100, (s * 85) / 100, (s * 65) / 100 };
    for (int i = 0; i < 4; ++i) {
        int w = widths[i];
        int x = (s - w) / 2;
        int y = y0 + i * (bar_h + gap);
        prim_rect(c, x, y, w, bar_h, bar_h / 2, C_FOG);
    }
}

static void draw_rain(lv_obj_t* c, int s)
{
    int cx = s / 2;
    // Cloud (upper)
    draw_cloud_body(c, cx, s / 3, (s * 70) / 100, C_CLOUD);
    // 3 raindrops
    int drop_w = (s * 6) / 100; if (drop_w < 2) drop_w = 2;
    int drop_h = (s * 18) / 100;
    int y0 = (s * 60) / 100;
    int dx[3] = { cx - s/5, cx, cx + s/5 };
    for (int i = 0; i < 3; ++i) {
        prim_rect(c, dx[i] - drop_w/2, y0 + (i%2) * (s/14),
                  drop_w, drop_h, drop_w/2, C_RAIN);
    }
}

static void draw_snow(lv_obj_t* c, int s)
{
    int cx = s / 2;
    draw_cloud_body(c, cx, s / 3, (s * 70) / 100, C_CLOUD);
    int flake_d = (s * 12) / 100; if (flake_d < 4) flake_d = 4;
    int y0 = (s * 68) / 100;
    int fx[3] = { cx - s/5, cx, cx + s/5 };
    for (int i = 0; i < 3; ++i) {
        prim_circle(c, fx[i], y0 + (i%2) * (s/14), flake_d, C_SNOW);
    }
}

static void draw_thunder(lv_obj_t* c, int s)
{
    int cx = s / 2;
    draw_cloud_body(c, cx, s / 3, (s * 70) / 100, C_CLOUD_D);
    // Lightning bolt: 4-point zig-zag approximated with two rotated rects.
    // Simpler: a yellow zig-zag as two diagonal bars.
    int w = (s * 8) / 100; if (w < 3) w = 3;
    // Upper diagonal
    int x1 = cx - s/16, y1 = (s * 52) / 100;
    prim_rect(c, x1, y1, w, s/5, w/2, C_BOLT);
    // Lower diagonal offset
    int x2 = cx - w, y2 = (s * 72) / 100;
    prim_rect(c, x2, y2, w, s/5, w/2, C_BOLT);
    // Center dot to bridge
    prim_circle(c, cx - w/2, (s * 65) / 100, w + 2, C_BOLT);
}

static void draw_night_clear(lv_obj_t* c, int s)
{
    int cx = s / 2, cy = s / 2;
    int moon_r = (s * 26) / 100;
    // Full moon disk
    lv_obj_t* moon = prim_circle(c, cx, cy, moon_r * 2, C_MOON);
    (void)moon;
    // Shadow disk (same bg color as container) — creates crescent.
    // We don't know container color, so draw a slightly darker shadow via
    // semi-transparent overlay that "eats" the right side.
    int shadow_d = moon_r * 2;
    lv_obj_t* sh = lv_obj_create(c);
    lv_obj_remove_style_all(sh);
    lv_obj_set_size(sh, shadow_d, shadow_d);
    lv_obj_set_pos(sh, cx - moon_r + (moon_r * 55) / 100,
                       cy - moon_r);
    lv_obj_set_style_bg_color(sh, LV_COLOR_MAKE(28, 28, 30), 0); // activity bg
    lv_obj_set_style_bg_opa(sh, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sh, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(sh, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(sh, LV_OBJ_FLAG_SCROLLABLE);
}

static void draw_night_cloudy(lv_obj_t* c, int s)
{
    int cx = s / 2, cy = s / 2;
    // Small moon upper-left
    int mr = (s * 14) / 100;
    int mx = cx - s/5, my = cy - s/5;
    prim_circle(c, mx, my, mr * 2, C_MOON);
    lv_obj_t* sh = lv_obj_create(c);
    lv_obj_remove_style_all(sh);
    lv_obj_set_size(sh, mr * 2, mr * 2);
    lv_obj_set_pos(sh, mx - mr + (mr * 55)/100, my - mr);
    lv_obj_set_style_bg_color(sh, LV_COLOR_MAKE(28, 28, 30), 0);
    lv_obj_set_style_bg_opa(sh, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sh, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(sh, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(sh, LV_OBJ_FLAG_SCROLLABLE);
    // Cloud in front
    draw_cloud_body(c, cx + s/12, cy + s/8, (s * 62) / 100, C_CLOUD);
}

static void draw_unknown(lv_obj_t* c, int s)
{
    int cx = s / 2, cy = s / 2;
    // Simple dashed circle ring
    int r = (s * 28) / 100;
    int thick = (s * 8) / 100; if (thick < 2) thick = 2;
    for (int i = 0; i < 8; ++i) {
        prim_ray(c, cx, cy, i * 45.0f + 22.5f, r, r + thick, thick, C_UNKNOWN);
    }
    prim_circle(c, cx, cy, s / 8, C_UNKNOWN);
}

// ─── Public API ──────────────────────────────────────────────────────────────

void weather_icon_render(lv_obj_t* container, weather_condition_t cond)
{
    if (!container) return;

    // Clear existing children (previous icon).
    lv_obj_clean(container);

    int w = lv_obj_get_width(container);
    int h = lv_obj_get_height(container);
    int s = (w < h) ? w : h;
    if (s <= 0) s = 56;  // fallback if layout hasn't happened yet

    switch (cond) {
        case WEATHER_CLEAR:         draw_clear(container, s);         break;
        case WEATHER_PARTLY_CLOUDY: draw_partly_cloudy(container, s); break;
        case WEATHER_CLOUDY:        draw_cloudy(container, s);        break;
        case WEATHER_FOG:           draw_fog(container, s);           break;
        case WEATHER_RAIN:          draw_rain(container, s);          break;
        case WEATHER_SNOW:          draw_snow(container, s);          break;
        case WEATHER_THUNDER:       draw_thunder(container, s);       break;
        case WEATHER_NIGHT_CLEAR:   draw_night_clear(container, s);   break;
        case WEATHER_NIGHT_CLOUDY:  draw_night_cloudy(container, s);  break;
        case WEATHER_UNKNOWN:
        default:                    draw_unknown(container, s);       break;
    }
}

weather_condition_t weather_icon_from_wmo(int wmo, bool is_night)
{
    // Open-Meteo WMO weather_code mapping
    // 0: clear; 1-2: mainly/partly cloudy; 3: overcast
    // 45,48: fog; 51-57: drizzle; 61-67: rain; 71-77: snow
    // 80-82: rain showers; 85-86: snow showers; 95-99: thunder
    if (wmo == 0)
        return is_night ? WEATHER_NIGHT_CLEAR : WEATHER_CLEAR;
    if (wmo == 1 || wmo == 2)
        return is_night ? WEATHER_NIGHT_CLOUDY : WEATHER_PARTLY_CLOUDY;
    if (wmo == 3)                         return WEATHER_CLOUDY;
    if (wmo == 45 || wmo == 48)           return WEATHER_FOG;
    if (wmo >= 51 && wmo <= 57)           return WEATHER_RAIN;
    if (wmo >= 61 && wmo <= 67)           return WEATHER_RAIN;
    if (wmo >= 71 && wmo <= 77)           return WEATHER_SNOW;
    if (wmo >= 80 && wmo <= 82)           return WEATHER_RAIN;
    if (wmo == 85 || wmo == 86)           return WEATHER_SNOW;
    if (wmo >= 95 && wmo <= 99)           return WEATHER_THUNDER;
    return WEATHER_UNKNOWN;
}
