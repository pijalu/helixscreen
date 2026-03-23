// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_console_widget.h"

#include "ui_event_safety.h"
#include "ui_panel_console.h"

#include "panel_widget_registry.h"
#include "ui/ui_lazy_panel_helper.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_gcode_console_widget() {
    register_widget_factory(
        "gcode_console", [](const std::string&) { return std::make_unique<GCodeConsoleWidget>(); });

    lv_xml_register_event_cb(nullptr, "gcode_console_clicked_cb", GCodeConsoleWidget::clicked_cb);
}

GCodeConsoleWidget::GCodeConsoleWidget() = default;

GCodeConsoleWidget::~GCodeConsoleWidget() {
    detach();
}

void GCodeConsoleWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    btn_ = lv_obj_find_by_name(widget_obj_, "gcode_console_button");
    if (btn_) {
        lv_obj_set_user_data(btn_, this);
    }
}

void GCodeConsoleWidget::detach() {
    if (btn_) {
        lv_obj_set_user_data(btn_, nullptr);
        btn_ = nullptr;
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void GCodeConsoleWidget::handle_click() {
    helix::ui::lazy_create_and_push_overlay<ConsolePanel>(get_global_console_panel, console_panel_,
                                                          parent_screen_, "Console",
                                                          "GCodeConsoleWidget", true);
}

void GCodeConsoleWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[GCodeConsoleWidget] clicked_cb");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<GCodeConsoleWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->handle_click();
    } else {
        spdlog::warn("[GCodeConsoleWidget] clicked_cb: could not recover widget instance");
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
