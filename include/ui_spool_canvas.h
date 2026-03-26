// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

/**
 * @file ui_spool_canvas.h
 * @brief Pseudo-3D filament spool visualization using LVGL canvas
 *
 * Creates a visually appealing 3D-style spool with approximately 20 degrees
 * rotation, showing a front face ellipse plus visible side depth.
 *
 * XML usage:
 * @code{.xml}
 * <spool_canvas color="0xFF5722" fill_level="0.75" size="64"/>
 * @endcode
 *
 * Properties:
 *   - color: Filament color as hex (e.g., "0xFF5722")
 *   - fill_level: Amount of filament 0.0 (empty) to 1.0 (full)
 *   - size: Canvas size in pixels (default 64)
 */

void ui_spool_canvas_register(void);

/**
 * @brief Create a spool canvas programmatically
 *
 * Alternative to XML creation for use in C++ code.
 *
 * @param parent Parent LVGL object
 * @param size Canvas size in pixels (default 64 if 0)
 * @return Created canvas object, or NULL on failure
 */
lv_obj_t* ui_spool_canvas_create(lv_obj_t* parent, int32_t size);

void ui_spool_canvas_set_color(lv_obj_t* canvas, lv_color_t color);
void ui_spool_canvas_set_fill_level(lv_obj_t* canvas, float fill_level);
void ui_spool_canvas_set_size(lv_obj_t* canvas, int32_t size);
void ui_spool_canvas_redraw(lv_obj_t* canvas);
float ui_spool_canvas_get_fill_level(lv_obj_t* canvas);
lv_color_t ui_spool_canvas_get_color(lv_obj_t* canvas);

#ifdef __cplusplus
}
#endif
