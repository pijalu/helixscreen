// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led_controls_widget.h"

#include "app_globals.h"
#include "led/ui_led_control_overlay.h"
#include "moonraker_api.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"

#include <lvgl.h>
#include <spdlog/spdlog.h>

namespace helix {

void register_led_controls_widget() {
    register_widget_factory("led_controls", []() -> std::unique_ptr<PanelWidget> {
        auto& ps = get_printer_state();
        auto* api = PanelWidgetManager::instance().shared_resource<MoonrakerAPI>();
        return std::make_unique<LedControlsWidget>(ps, api);
    });
    lv_xml_register_event_cb(nullptr, "on_led_controls_clicked",
                             LedControlsWidget::on_led_controls_clicked);
}

LedControlsWidget::LedControlsWidget(PrinterState& printer_state, MoonrakerAPI* api)
    : printer_state_(printer_state), api_(api) {}

LedControlsWidget::~LedControlsWidget() { detach(); }

void LedControlsWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    lv_obj_set_user_data(widget_obj_, this);

    auto* button = lv_obj_find_by_name(widget_obj_, "led_controls_button");
    if (button) {
        lv_obj_set_user_data(button, this);
    }
}

void LedControlsWidget::detach() {
    if (widget_obj_) {
        auto* button = lv_obj_find_by_name(widget_obj_, "led_controls_button");
        if (button) {
            lv_obj_set_user_data(button, nullptr);
        }
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    led_control_panel_ = nullptr;
}

void LedControlsWidget::on_led_controls_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LedControlsWidget] on_led_controls_clicked");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<LedControlsWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->handle_clicked();
    } else {
        spdlog::warn("[LedControlsWidget] on_led_controls_clicked: no widget instance");
    }
    LVGL_SAFE_EVENT_CB_END();
}

void LedControlsWidget::handle_clicked() {
    spdlog::debug("[LedControlsWidget] Clicked - opening LED control overlay");

    if (!led_control_panel_ && parent_screen_) {
        auto& overlay = get_led_control_overlay();

        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();
        overlay.set_api(api_);

        led_control_panel_ = overlay.create(parent_screen_);
        if (!led_control_panel_) {
            spdlog::error("[LedControlsWidget] Failed to create LED control overlay");
            return;
        }
        NavigationManager::instance().register_overlay_instance(led_control_panel_, &overlay);
    }

    if (led_control_panel_) {
        get_led_control_overlay().set_api(api_);
        NavigationManager::instance().push_overlay(led_control_panel_);
    }
}

} // namespace helix
