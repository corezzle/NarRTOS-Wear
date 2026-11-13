#pragma once
/**
 * control_center — Apple Watch style slide-down overlay.
 *
 * Shows a panel from the top of the screen containing quick toggles:
 *   WiFi | Bluetooth | Airplane mode | Flashlight
 *
 * Open  : swipe-down (g_swipe_dir == +2) intercepted in app_manager
 * Close : swipe-up   (g_swipe_dir == -2) or tap on the dimmed backdrop
 *
 * Runs entirely on lv_layer_top() — overlays any active app screen.
 * Must be called from the LVGL task (Core 0).
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Call once after app_manager_init(), before the main LVGL loop. */
void control_center_init(void);

/** Slide panel down — no-op if already open. */
void control_center_open(void);

/** Slide panel up — no-op if already closed. */
void control_center_close(void);

/** Returns true while the panel is visible (including during close animation). */
bool control_center_is_open(void);

#ifdef __cplusplus
}
#endif
