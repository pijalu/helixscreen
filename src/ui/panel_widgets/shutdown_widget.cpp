// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shutdown_widget.h"

#include "ui_event_safety.h"
#include "ui_update_queue.h"

#include "moonraker_api.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

void register_shutdown_widget() {
    register_widget_factory("shutdown", [](const std::string&) {
        auto* api = PanelWidgetManager::instance().shared_resource<MoonrakerAPI>();
        return std::make_unique<ShutdownWidget>(api);
    });

    // Register XML event callback at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "shutdown_clicked_cb", ShutdownWidget::shutdown_clicked_cb);
}

ShutdownWidget::ShutdownWidget(MoonrakerAPI* api) : api_(api) {}

ShutdownWidget::~ShutdownWidget() {
    detach();
}

void ShutdownWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Set user_data on the root lv_obj, NOT on the ui_button child.
    // ui_button allocates its own UiButtonData in user_data — overwriting it
    // leaks memory and breaks button style/contrast auto-updates.
    lv_obj_set_user_data(widget_obj_, this);

    shutdown_btn_ = lv_obj_find_by_name(widget_obj_, "shutdown_button");
    if (shutdown_btn_) {
        lv_obj_add_event_cb(shutdown_btn_, shutdown_clicked_cb, LV_EVENT_CLICKED, this);
    }
}

void ShutdownWidget::detach() {
    lifetime_.invalidate();

    if (shutdown_modal_.is_visible()) {
        shutdown_modal_.hide();
    }

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
    shutdown_btn_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void ShutdownWidget::handle_click() {
    spdlog::info("[ShutdownWidget] Shutdown button clicked");

    if (!api_) {
        spdlog::warn("[ShutdownWidget] No API available");
        return;
    }

    shutdown_modal_.set_callbacks([this]() { execute_shutdown(); }, [this]() { execute_reboot(); });

    shutdown_modal_.show(lv_screen_active());
}

void ShutdownWidget::execute_shutdown() {
    spdlog::info("[ShutdownWidget] Executing machine shutdown");

    api_->machine_shutdown(
        []() {
            spdlog::info("[ShutdownWidget] Machine shutdown command sent successfully");
        },
        [](const MoonrakerError& err) {
            spdlog::error("[ShutdownWidget] Machine shutdown failed: {}", err.message);
        });
}

void ShutdownWidget::execute_reboot() {
    spdlog::info("[ShutdownWidget] Executing machine reboot");

    api_->machine_reboot(
        []() {
            spdlog::info("[ShutdownWidget] Machine reboot command sent successfully");
        },
        [](const MoonrakerError& err) {
            spdlog::error("[ShutdownWidget] Machine reboot failed: {}", err.message);
        });
}

void ShutdownWidget::shutdown_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ShutdownWidget] shutdown_clicked_cb");
    auto* self = static_cast<ShutdownWidget*>(lv_event_get_user_data(e));
    if (self) {
        self->record_interaction();
        self->handle_click();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
