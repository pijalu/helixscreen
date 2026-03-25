// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_panel_motion.h" // For JogDirection and JogMode enums

#include <lvgl.h>

// Jog pad event callbacks
typedef void (*jog_pad_jog_cb_t)(helix::JogDirection direction, float distance_mm, void* user_data);
typedef void (*jog_pad_home_cb_t)(void* user_data);

/**
 * Create a circular jog pad widget (Bambu Lab style)
 *
 * Features:
 * - Two-zone design: inner ring (small jogs), outer ring (large jogs)
 * - 8 directional zones (N, S, E, W, NE, NW, SE, SW)
 * - Center home button
 * - Theme-aware colors (reads from component scope or uses fallbacks)
 * - Visual press feedback
 *
 * @param parent Parent LVGL object
 * @return Created jog pad object (use as clickable container)
 */
lv_obj_t* ui_jog_pad_create(lv_obj_t* parent);

/**
 * Set jog callback (called when directional zone is clicked)
 *
 * @param obj Jog pad object
 * @param cb Callback function (direction, distance_mm, user_data)
 * @param user_data Optional user data passed to callback
 */
void ui_jog_pad_set_jog_callback(lv_obj_t* obj, jog_pad_jog_cb_t cb, void* user_data);

/**
 * Set home callback (called when center button is clicked)
 *
 * @param obj Jog pad object
 * @param cb Callback function (user_data)
 * @param user_data Optional user data passed to callback
 */
void ui_jog_pad_set_home_callback(lv_obj_t* obj, jog_pad_home_cb_t cb, void* user_data);

/**
 * Set jog mode (Fine/Coarse/Turbo - affects inner/outer ring distances)
 *
 * Fine:   inner=0.1mm, outer=1mm
 * Coarse: inner=1mm,   outer=10mm
 * Turbo:  inner=10mm,  outer=50mm
 *
 * @param obj Jog pad object
 * @param mode Jog mode
 */
void ui_jog_pad_set_mode(lv_obj_t* obj, helix::JogMode mode);

/**
 * Get current jog mode
 *
 * @param obj Jog pad object
 * @return Current jog mode
 */
helix::JogMode ui_jog_pad_get_mode(lv_obj_t* obj);

/**
 * Refresh colors from theme (call when theme changes)
 *
 * @param obj Jog pad object
 */
void ui_jog_pad_refresh_colors(lv_obj_t* obj);
