// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_fan_arc_resize.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

// Sizing constants for arc scaling
constexpr int32_t MIN_ARC_SIZE = 60;
constexpr int32_t ARC_TO_TRACK_RATIO = 11;
constexpr int32_t MIN_TRACK_WIDTH = 6;

void fan_arc_resize_to_fit(lv_obj_t* card_root) {
    if (!card_root)
        return;

    // Re-entrancy guard: lv_obj_update_layout() below can fire SIZE_CHANGED
    // which calls back into this function via the event callback
    static bool in_resize = false;
    if (in_resize)
        return;
    in_resize = true;

    lv_obj_t* container = lv_obj_find_by_name(card_root, "dial_container");
    lv_obj_t* arc = lv_obj_find_by_name(card_root, "dial_arc");
    if (!container || !arc) {
        in_resize = false;
        return;
    }

    // Force layout computation so flex_grow children have real sizes
    lv_obj_update_layout(card_root);

    int32_t content_w = lv_obj_get_content_width(card_root);
    int32_t container_h = lv_obj_get_content_height(container);

    // Arc must be square, fit in both dimensions
    int32_t arc_size = LV_MIN(content_w, container_h);
    arc_size = LV_MAX(arc_size, MIN_ARC_SIZE);

    // Skip if already at target size (avoids re-entrancy from child layout changes)
    if (lv_obj_get_width(arc) == arc_size && lv_obj_get_height(arc) == arc_size) {
        in_resize = false;
        return;
    }

    lv_obj_set_size(arc, arc_size, arc_size);

    // Scale arc track width (ratio matches all original breakpoints)
    int32_t track_w = LV_MAX(arc_size / ARC_TO_TRACK_RATIO, MIN_TRACK_WIDTH);
    lv_obj_set_style_arc_width(arc, track_w, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, track_w, LV_PART_INDICATOR);

    spdlog::trace("[FanArcResize] card_w={} container_h={} -> arc={}x{} track_w={}", content_w,
                  container_h, arc_size, arc_size, track_w);
    in_resize = false;
}

static void on_card_size_changed(lv_event_t* e) {
    auto* card_root = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    fan_arc_resize_to_fit(card_root);
}

void fan_arc_attach_auto_resize(lv_obj_t* card_root) {
    if (!card_root)
        return;
    lv_obj_add_event_cb(card_root, on_card_size_changed, LV_EVENT_SIZE_CHANGED, nullptr);

    // Trigger initial resize
    fan_arc_resize_to_fit(card_root);
}

} // namespace helix::ui
