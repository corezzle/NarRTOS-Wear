#pragma once
/**
 * @brief App Manager - Modular app lifecycle controller.
 *
 * Each "App" implements the app_interface_t vtable.
 * AppManager calls on_start / on_stop / on_event to manage transitions.
 * Only one app is "active" (owns the LVGL screen) at a time.
 */

#include "os_config.h"
#include "event_bus.h"
#include <lvgl.h>

// ─── App Interface (vtable) ───────────────────────────────────────────────────

typedef struct {
    /**
     * Called once when the app is launched.
     * @param screen  Blank LVGL screen object to build UI onto.
     */
    void (*on_start)(lv_obj_t* screen);

    /**
     * Called when the app is brought to foreground after being paused.
     */
    void (*on_resume)(void);

    /**
     * Called when another app takes over (app is paused, not destroyed).
     */
    void (*on_pause)(void);

    /**
     * Called when the app is fully stopped and resources should be freed.
     */
    void (*on_stop)(void);

    /**
     * Called for each OS event delivered to the active app.
     * Return true if the event was consumed.
     */
    bool (*on_event)(const os_event_t* event);

    /** Human-readable name for logging */
    const char* name;
} app_interface_t;

// ─── API ──────────────────────────────────────────────────────────────────────

void  app_manager_init(void);
void  app_manager_register(app_id_t id, const app_interface_t* app);
void  app_manager_launch(app_id_t id);

/**
 * v1.2.4 (2026-05-20): Slide animasyonsuz launch.
 * MOVE_LEFT transition (80 ms) + display buffer 80 lines chunk timing'in
 * uyumsuz olduğu durumlarda (Store detail "Open" butonu split artifact)
 * direct screen switch. Diğer çoğu durum için app_manager_launch yeterli.
 */
void  app_manager_launch_no_anim(app_id_t id);

void  app_manager_back(void);
app_id_t app_manager_current_id(void);

/**
 * @brief Ekrandaki tüm child widget'lara LV_OBJ_FLAG_GESTURE_BUBBLE ekler.
 *        Gesture event'inin child'lardan screen'e bubble etmesini sağlar.
 *        Her app'in on_start sonunda çağrılmalı.
 */
void app_manager_enable_gestures(lv_obj_t* screen);
