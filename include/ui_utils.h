// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_effects.h"
#include "ui_update_queue.h"

#include "lvgl/lvgl.h"
#include "static_panel_registry.h"

#include <cstdint>
#include <optional>
#include <string>

// ============================================================================
// Responsive Layout Utilities
// ============================================================================

/**
 * @brief Get responsive padding for content areas below headers
 *
 * Returns smaller padding on tiny/small screens for more compact layouts.
 *
 * @param screen_height Current screen height in pixels
 * @return Padding value in pixels
 */
lv_coord_t ui_get_header_content_padding(lv_coord_t screen_height);

/**
 * @brief Get responsive header height based on screen size
 *
 * Returns smaller header on tiny/small screens for more compact layouts.
 *
 * @param screen_height Current screen height in pixels
 * @return Header height in pixels (60px for large/medium, 48px for small, 40px for tiny)
 */
lv_coord_t ui_get_responsive_header_height(lv_coord_t screen_height);

// ============================================================================
// LED Icon Utilities
// ============================================================================

/**
 * @brief Get lightbulb icon name for LED brightness level
 *
 * Maps brightness percentage (0-100) to appropriate graduated lightbulb icon.
 * Returns icons from lightbulb_outline (off) through lightbulb_on_10..90 to
 * lightbulb_on (100%).
 *
 * @param brightness LED brightness 0-100%
 * @return Icon name string for ui_icon_set_source()
 */
const char* ui_brightness_to_lightbulb_icon(int brightness);

// ============================================================================
// Color Utilities
// ============================================================================

/**
 * @brief Parse hex color string to RGB integer value
 *
 * Converts color strings like "#ED1C24" or "ED1C24" to 0xRRGGBB format.
 * Returns std::nullopt for invalid input, allowing black (#000000) to be
 * correctly distinguished from parse errors.
 *
 * @param hex_str Color string with optional # prefix (e.g., "#FF0000", "00FF00")
 * @return RGB value as 0xRRGGBB, or std::nullopt if invalid/empty
 */
std::optional<uint32_t> ui_parse_hex_color(const std::string& hex_str);

/**
 * @brief Calculate perceptual color distance between two RGB colors
 *
 * Uses weighted Euclidean distance with human perception weights
 * (R=0.30, G=0.59, B=0.11 based on luminance).
 *
 * @param color1 First color as 0xRRGGBB
 * @param color2 Second color as 0xRRGGBB
 * @return Perceptual distance (0 = identical, larger = more different)
 */
int ui_color_distance(uint32_t color1, uint32_t color2);

// ============================================================================
// List/Empty State Visibility
// ============================================================================

namespace helix::ui {

/**
 * @brief Toggle visibility between a list container and its empty state
 *
 * Common pattern for panels that show either a populated list or an empty
 * state placeholder. When has_items is true, the list is shown and empty
 * state is hidden; when false, the opposite.
 *
 * @param list The list/content container widget (may be nullptr)
 * @param empty_state The empty state placeholder widget (may be nullptr)
 * @param has_items Whether the list has items to display
 */
inline void toggle_list_empty_state(lv_obj_t* list, lv_obj_t* empty_state, bool has_items) {
    if (list)
        lv_obj_set_flag(list, LV_OBJ_FLAG_HIDDEN, !has_items);
    if (empty_state)
        lv_obj_set_flag(empty_state, LV_OBJ_FLAG_HIDDEN, has_items);
}

// ============================================================================
// Object Lifecycle Utilities
// ============================================================================

/**
 * @brief Safely delete an LVGL object, guarding against shutdown race conditions
 *
 * During app shutdown, lv_is_initialized() can return true even after the display
 * has been torn down. This function checks both that LVGL is initialized AND
 * that a display still exists before attempting deletion.
 *
 * The pointer is automatically set to nullptr after deletion (or if skipped).
 *
 * WARNING: This performs SYNCHRONOUS deletion. Do NOT call from inside
 * queue_update(), async_call(), or overlay close callbacks — multiple
 * synchronous deletions in the same UpdateQueue batch corrupt LVGL's event
 * linked list (SIGSEGV in lv_event_mark_deleted). Use safe_delete_deferred()
 * in those contexts instead.
 *
 * @param obj Reference to pointer to the LVGL object (will be set to nullptr)
 * @return true if object was deleted, false if skipped (nullptr or shutdown in progress)
 */
inline bool safe_delete(lv_obj_t*& obj) {
    if (!obj)
        return false;
    if (!lv_is_initialized()) {
        obj = nullptr;
        return false;
    }
    if (!lv_display_get_next(nullptr)) {
        obj = nullptr;
        return false;
    }
    // Skip during destroy_all() - lv_deinit() will clean up all widgets
    if (StaticPanelRegistry::is_destroying_all()) {
        obj = nullptr;
        return false;
    }
    // Guard against stale pointers to already-deleted objects (e.g. children
    // auto-deleted by parent deletion before the child pointer was nulled)
    if (!lv_obj_is_valid(obj)) {
        obj = nullptr;
        return false;
    }
    // Remove entire tree from focus group before deletion to prevent LVGL from
    // auto-focusing the next element (which triggers scroll-on-focus)
    helix::ui::defocus_tree(obj);
    lv_obj_delete(obj);
    obj = nullptr;
    return true;
}

/**
 * @brief Queue LVGL object deletion for the next frame
 *
 * Immediately nullifies the pointer to prevent further use, then queues
 * the actual deletion via UpdateQueue. This prevents crashes when deleting
 * objects that have pending timer events or are referenced by in-flight
 * event processing (e.g., spinners with animation timers).
 *
 * @param obj Reference to pointer to the LVGL object (set to nullptr immediately)
 */
inline void safe_delete_deferred(lv_obj_t*& obj) {
    if (!obj)
        return;
    lv_obj_t* to_delete = obj;
    obj = nullptr;
    queue_update("safe_delete_deferred", [to_delete]() {
        if (!lv_is_initialized())
            return;
        if (!lv_display_get_next(nullptr))
            return;
        if (StaticPanelRegistry::is_destroying_all())
            return;
        if (!lv_obj_is_valid(to_delete))
            return;
        defocus_tree(to_delete);
        lv_obj_delete(to_delete);
    });
}

// ============================================================================
// Recursive Widget Flag Utilities
// ============================================================================

/**
 * @brief Recursively remove CLICKABLE flag from all descendants of obj
 *
 * Used by edit mode to prevent widget click handlers from firing
 * while grid rearrangement is in progress.
 *
 * @param obj Parent object whose descendants will have CLICKABLE removed
 */
inline void disable_widget_clicks_recursive(lv_obj_t* obj) {
    if (!obj) return;
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(obj, static_cast<int32_t>(i));
        if (!child) continue;
        lv_obj_remove_flag(child, LV_OBJ_FLAG_CLICKABLE);
        disable_widget_clicks_recursive(child);
    }
}

/**
 * @brief Recursively remove PRESSED state from obj and all descendants
 *
 * Clears visual press feedback from deeply nested children after
 * cancelling a press (e.g., when entering edit mode via long-press).
 *
 * @param obj Root object to clear PRESSED state from
 */
inline void clear_pressed_state_recursive(lv_obj_t* obj) {
    if (!obj) return;
    lv_obj_remove_state(obj, LV_STATE_PRESSED);
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        clear_pressed_state_recursive(lv_obj_get_child(obj, static_cast<int32_t>(i)));
    }
}

} // namespace helix::ui
