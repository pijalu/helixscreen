// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_clog_meter.h"

#include "ams_state.h"
#include "observer_factory.h"
#include "theme_manager.h"
#include "ui_update_queue.h"

#include "lvgl/lvgl.h"

#include <cstdlib>
#include <spdlog/spdlog.h>

namespace helix::ui {

// Arc size = percentage of card width; stroke scales with arc size
constexpr int32_t ARC_WIDTH_PCT = 18;       // arc is 18% of card width
constexpr int32_t ARC_TO_STROKE_RATIO = 12;
constexpr int32_t MIN_ARC_SIZE = 24;
constexpr int32_t MIN_STROKE_WIDTH = 2;

UiClogMeter::UiClogMeter(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[ClogMeter] NULL parent");
        return;
    }

    root_ = lv_obj_find_by_name(parent, "clog_meter");
    if (!root_) {
        spdlog::warn("[ClogMeter] clog_meter not found in parent");
        return;
    }

    arc_container_ = lv_obj_find_by_name(root_, "clog_arc_container");
    arc_ = lv_obj_find_by_name(root_, "clog_arc");
    if (!arc_container_ || !arc_) {
        spdlog::warn("[ClogMeter] clog_arc_container or clog_arc not found");
        return;
    }

    // Attach SIZE_CHANGED callback on the loaded card (root_'s parent)
    // to dynamically size the arc to fill available height
    lv_obj_t* card = lv_obj_get_parent(root_);
    if (card) {
        // SIZE_CHANGED is a layout event — cannot be registered via XML <event_cb>
        lv_obj_add_event_cb(card, on_card_size_changed, LV_EVENT_SIZE_CHANGED, this);
        resize_arc();
    }

    setup_observers();
    spdlog::debug("[ClogMeter] Initialized");
}

UiClogMeter::~UiClogMeter() {
    // Freeze update queue around observer teardown to prevent race with
    // WebSocket thread enqueueing deferred callbacks between drain and destroy
    auto freeze = UpdateQueue::instance().scoped_freeze();
    UpdateQueue::instance().drain();

    // Remove SIZE_CHANGED callback to prevent dangling this pointer
    lv_obj_t* card = root_ ? lv_obj_get_parent(root_) : nullptr;
    if (card) {
        lv_obj_remove_event_cb_with_user_data(card, on_card_size_changed, this);
    }

    mode_obs_.reset();
    value_obs_.reset();
    warning_obs_.reset();
    root_ = nullptr;
    arc_ = nullptr;
    spdlog::debug("[ClogMeter] Destroyed");
}

void UiClogMeter::setup_observers() {
    auto& ams = AmsState::instance();

    mode_obs_ = observe_int_sync<UiClogMeter>(
        ams.get_clog_meter_mode_subject(), this,
        [](UiClogMeter* self, int mode) { self->on_mode_changed(mode); });

    value_obs_ = observe_int_sync<UiClogMeter>(
        ams.get_clog_meter_value_subject(), this,
        [](UiClogMeter* self, int value) { self->on_value_changed(value); });

    warning_obs_ = observe_int_sync<UiClogMeter>(
        ams.get_clog_meter_warning_subject(), this,
        [](UiClogMeter* self, int warning) { self->on_warning_changed(warning); });
}

void UiClogMeter::on_mode_changed(int mode) {
    current_mode_ = mode;

    if (!arc_) return;

    if (mode == 2) {
        // Flowguard: symmetrical mode, range -100..+100 mapped to 0..200
        lv_arc_set_range(arc_, 0, 200);
        lv_arc_set_mode(arc_, LV_ARC_MODE_SYMMETRICAL);
        lv_arc_set_value(arc_, 100); // Center
    } else {
        // Encoder/AFC/none: normal 0-100
        lv_arc_set_range(arc_, 0, 100);
        lv_arc_set_mode(arc_, LV_ARC_MODE_NORMAL);
        lv_arc_set_value(arc_, 0);
    }

    update_arc_color();
    spdlog::debug("[ClogMeter] Mode changed to {}", mode);
}

void UiClogMeter::on_value_changed(int value) {
    current_value_ = value;

    if (!arc_) return;

    if (current_mode_ == 2) {
        // Flowguard: -100..+100 → 0..200
        lv_arc_set_value(arc_, value + 100);
    } else {
        lv_arc_set_value(arc_, value);
    }

    update_arc_color();
}

void UiClogMeter::on_warning_changed(int warning) {
    current_warning_ = warning;
    update_arc_color();
}

void UiClogMeter::update_arc_color() {
    if (!arc_) return;

    lv_color_t color;
    int val = std::clamp(std::abs(current_value_), 0, 100);

    if (current_warning_) {
        // Warning/triggered state
        color = theme_manager_get_color("danger");
    } else if (current_mode_ == 1 || current_mode_ == 3) {
        // Encoder/AFC: gradient primary (safe) → warning (risky) → danger (clogged)
        // Dynamic arc color is an intentional exception to the "no C++ styling" rule
        if (val < 50) {
            color = lv_color_mix(theme_manager_get_color("warning"),
                                 theme_manager_get_color("primary"),
                                 static_cast<uint8_t>(val * 255 / 50));
        } else {
            color = lv_color_mix(theme_manager_get_color("danger"),
                                 theme_manager_get_color("warning"),
                                 static_cast<uint8_t>((val - 50) * 255 / 50));
        }
    } else {
        // Flowguard or default
        color = theme_manager_get_color("primary");
    }

    lv_obj_set_style_arc_color(arc_, color, LV_PART_INDICATOR);
}

void UiClogMeter::on_card_size_changed(lv_event_t* e) {
    auto* self = static_cast<UiClogMeter*>(lv_event_get_user_data(e));
    if (self) self->resize_arc();
}

void UiClogMeter::resize_arc() {
    if (!arc_ || !arc_container_ || !root_) return;

    // Re-entrancy guard: lv_obj_update_layout() can fire SIZE_CHANGED
    if (in_resize_) return;
    in_resize_ = true;

    lv_obj_t* card = lv_obj_get_parent(root_);
    if (!card) {
        in_resize_ = false;
        return;
    }

    lv_obj_update_layout(card);

    // Arc size = percentage of card width (responsive to breakpoint)
    int32_t card_w = lv_obj_get_content_width(card);
    int32_t arc_size = LV_MAX(card_w * ARC_WIDTH_PCT / 100, MIN_ARC_SIZE);

    // Skip if already at target size
    if (lv_obj_get_width(arc_) == arc_size && lv_obj_get_height(arc_) == arc_size) {
        in_resize_ = false;
        return;
    }

    // Size the container and arc to the computed square
    lv_obj_set_size(arc_container_, arc_size, arc_size);
    lv_obj_set_size(arc_, arc_size, arc_size);

    // Scale stroke width proportionally
    int32_t stroke = LV_MAX(arc_size / ARC_TO_STROKE_RATIO, MIN_STROKE_WIDTH);
    lv_obj_set_style_arc_width(arc_, stroke, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_, stroke, LV_PART_INDICATOR);

    spdlog::debug("[ClogMeter] card_w={} -> arc={}x{} stroke={}",
                  card_w, arc_size, arc_size, stroke);
    in_resize_ = false;
}

} // namespace helix::ui
