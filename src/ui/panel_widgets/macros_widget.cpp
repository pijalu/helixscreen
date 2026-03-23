// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macros_widget.h"

#include "ui_event_safety.h"
#include "ui_panel_macros.h"

#include "panel_widget_registry.h"
#include "ui/ui_lazy_panel_helper.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_macros_widget() {
    register_widget_factory("macros",
                            [](const std::string&) { return std::make_unique<MacrosWidget>(); });

    lv_xml_register_event_cb(nullptr, "macros_widget_clicked_cb", MacrosWidget::clicked_cb);
}

MacrosWidget::MacrosWidget() = default;

MacrosWidget::~MacrosWidget() {
    detach();
}

void MacrosWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    btn_ = lv_obj_find_by_name(widget_obj_, "macros_button");
    if (btn_) {
        lv_obj_set_user_data(btn_, this);
    }
}

void MacrosWidget::detach() {
    if (btn_) {
        lv_obj_set_user_data(btn_, nullptr);
        btn_ = nullptr;
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void MacrosWidget::handle_click() {
    helix::ui::lazy_create_and_push_overlay<MacrosPanel>(
        get_global_macros_panel, macros_panel_, parent_screen_, "Macros", "MacrosWidget", true);
}

void MacrosWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacrosWidget] clicked_cb");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<MacrosWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->handle_click();
    } else {
        spdlog::warn("[MacrosWidget] clicked_cb: could not recover widget instance");
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
