// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_effects.h"
#include "ui_update_queue.h"

#include "lvgl/lvgl.h"
#include "static_panel_registry.h"

#include <cstdint>

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
 * @brief Check that an LVGL object's parent chain terminates within a sane depth
 *
 * A non-terminating parent chain indicates corruption: UAF where freed memory
 * was reused, double-reparent producing a cycle, or wild pointer mistaken as
 * valid by lv_obj_is_valid() (which walks the screen tree — a reused address
 * can pass validity while pointing at a different live object).
 *
 * When this returns false, callers should avoid operations that walk or
 * rewrite the parent chain (lv_obj_set_parent, lv_obj_get_display), since
 * they'll spin or corrupt state further. Mirrors the 128-step cap applied
 * in the lvgl_obj_get_screen_cycle_guard patch.
 */
inline bool has_sane_parent_chain(lv_obj_t* obj) {
    uint32_t depth = 0;
    lv_obj_t* p = obj;
    while (p) {
        if (++depth > 128) {
            return false;
        }
        p = lv_obj_get_parent(p);
    }
    return true;
}

/**
 * @brief Queue LVGL object deletion for the next timer tick
 *
 * Immediately nullifies the pointer to prevent further use, hides the
 * object, and defers actual deletion via lv_obj_delete_async(). This
 * avoids crashes from calling lv_obj_delete() inside UpdateQueue's
 * process_pending() batch, where multiple deletions corrupt LVGL's
 * global event linked list (SIGSEGV in lv_event_mark_deleted).
 *
 * Uses lv_obj_delete_async() (not lv_async_call with a custom lambda)
 * so that LVGL's built-in cancellation logic works — if something else
 * deletes the object first, obj_delete_core() cancels the pending async.
 *
 * @param obj Reference to pointer to the LVGL object (set to nullptr immediately)
 */
inline void safe_delete_deferred(lv_obj_t*& obj) {
    if (!obj)
        return;
    if (!lv_is_initialized() || !lv_display_get_next(nullptr) ||
        StaticPanelRegistry::is_destroying_all()) {
        obj = nullptr;
        return;
    }
    if (!lv_obj_is_valid(obj)) {
        obj = nullptr;
        return;
    }
    // Abort before touching obj if its parent chain is cyclic — lv_obj_is_valid
    // has false positives when memory is reused by a different live object, and
    // lv_obj_set_parent below would then spin in lv_obj_get_screen (#SonicPad
    // v0.99.35 grid-edit hang). Null the caller's pointer and let the corrupt
    // tree be cleaned up by lv_deinit() at shutdown.
    if (!has_sane_parent_chain(obj)) {
        obj = nullptr;
        return;
    }
    lv_obj_t* to_delete = obj;
    obj = nullptr;
    // Hide immediately so the widget isn't visible while deletion is deferred
    lv_obj_add_flag(to_delete, LV_OBJ_FLAG_HIDDEN);
    defocus_tree(to_delete);
    // Reparent to lv_layer_top() so the original parent's destruction won't
    // recursively delete this child while it's queued for async delete.
    // Without this, obj_delete_core can hit an already-freed child whose
    // event list is corrupted — SIGSEGV in lv_event_mark_deleted (ad5x).
    lv_obj_t* layer = lv_layer_top();
    if (layer) {
        lv_obj_set_parent(to_delete, layer);
    }
    // Defer deletion to next lv_timer_handler tick — outside UpdateQueue batch
    lv_obj_delete_async(to_delete);
}

/**
 * @brief Deferred-delete for raw pointers (no automatic nulling)
 *
 * Same safety as the reference overload (hide, defocus, reparent, async delete)
 * but for local variables or cases where the caller manages the pointer.
 *
 * @param obj Raw pointer to the LVGL object (caller must null their copy)
 */
inline void safe_delete_deferred_raw(lv_obj_t* obj) {
    if (!obj)
        return;
    if (!lv_is_initialized() || !lv_display_get_next(nullptr) ||
        StaticPanelRegistry::is_destroying_all()) {
        return;
    }
    if (!lv_obj_is_valid(obj)) {
        return;
    }
    if (!has_sane_parent_chain(obj)) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    defocus_tree(obj);
    lv_obj_t* layer = lv_layer_top();
    if (layer) {
        lv_obj_set_parent(obj, layer);
    }
    lv_obj_delete_async(obj);
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
    if (!obj)
        return;
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(obj, static_cast<int32_t>(i));
        if (!child)
            continue;
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
    if (!obj)
        return;
    lv_obj_remove_state(obj, LV_STATE_PRESSED);
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        clear_pressed_state_recursive(lv_obj_get_child(obj, static_cast<int32_t>(i)));
    }
}

} // namespace helix::ui
