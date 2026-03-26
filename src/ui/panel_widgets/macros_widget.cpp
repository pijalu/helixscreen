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

    // Set user_data on the root lv_obj, NOT on the ui_button child.
    // ui_button allocates its own UiButtonData in user_data — overwriting it
    // leaks memory and breaks button style/contrast auto-updates.
    lv_obj_set_user_data(widget_obj_, this);

    btn_ = lv_obj_find_by_name(widget_obj_, "macros_button");
}

void MacrosWidget::detach() {
    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
    btn_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void MacrosWidget::handle_click() {
    helix::ui::lazy_create_and_push_overlay<MacrosPanel>(
        get_global_macros_panel, macros_panel_, parent_screen_, "Macros", "MacrosWidget", true);
}

void MacrosWidget::clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacrosWidget] clicked_cb");
    // Walk up from the event target to find the root widget_obj_ that holds
    // our user_data. The event_cb is on a ui_button child, but user_data is
    // on the parent lv_obj to avoid colliding with ui_button's UiButtonData.
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    MacrosWidget* self = nullptr;
    while (obj) {
        self = static_cast<MacrosWidget*>(lv_obj_get_user_data(obj));
        if (self) break;
        obj = lv_obj_get_parent(obj);
    }
    if (self) {
        self->record_interaction();
        self->handle_click();
    } else {
        spdlog::warn("[MacrosWidget] clicked_cb: could not recover widget instance");
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
