#pragma once
/**
 * @brief WatchFace App - Default home screen.
 *
 * Displays: Time, Date, Battery %, Step count
 * Registers with AppManager via watchface_app_get_interface().
 *
 * LVGL objects are created in on_start(), destroyed in on_stop().
 * Battery and step count labels are updated via EVENT_BATTERY_LEVEL
 * and EVENT_STEP_COUNT events dispatched through on_event().
 */

#include "../../core/app_manager.h"

const app_interface_t* watchface_app_get_interface(void);
