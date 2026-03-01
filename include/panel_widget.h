// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"

#include <string>

#include "hv/json.hpp"

namespace helix {

/// Base class for home widgets that need C++ behavioral wiring.
/// Widgets that are pure XML binding (filament, probe, humidity, etc.) don't need this.
class PanelWidget {
  public:
    virtual ~PanelWidget() = default;

    /// Called BEFORE lv_xml_create() — create and register any LVGL subjects
    /// that XML bindings depend on. Default is no-op.
    virtual void init_subjects() {}

    /// Set per-widget config from PanelWidgetEntry. Called after factory
    /// creation, before get_component_name() and attach().
    virtual void set_config(const nlohmann::json& config) {
        (void)config;
    }

    /// Return the XML component name to use for this widget. Allows widgets
    /// to select different XML layouts based on their config (e.g. carousel
    /// vs stack mode). Default returns "panel_widget_<id>".
    virtual std::string get_component_name() const {
        return std::string("panel_widget_") + id();
    }

    /// Called after XML obj is created. Wire observers, animators, callbacks.
    /// @param widget_obj  The root lv_obj from lv_xml_create()
    /// @param parent_screen  Screen for lazy overlay creation
    virtual void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) = 0;

    /// Called before widget destruction. Clean up observers and state.
    virtual void detach() = 0;

    /// Called when the owning panel becomes visible.
    virtual void on_activate() {}

    /// Called when the owning panel goes offscreen.
    virtual void on_deactivate() {}

    /// Called after grid cell placement and whenever the widget is resized.
    /// Widgets can adapt their content layout based on available space.
    /// @param colspan  Grid columns spanned
    /// @param rowspan  Grid rows spanned
    /// @param width_px  Actual pixel width of the widget
    /// @param height_px  Actual pixel height of the widget
    virtual void on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
        (void)colspan;
        (void)rowspan;
        (void)width_px;
        (void)height_px;
    }

    /// Whether this widget supports configuration in edit mode.
    /// Override to return true to show the configure (gear) button.
    virtual bool has_edit_configure() const {
        return false;
    }

    /// Called when the configure button is pressed in edit mode. Return true
    /// if handled (triggers rebuild). Widgets can toggle display modes, open
    /// config modals, etc.
    virtual bool on_edit_configure() {
        return false;
    }

    /// Stable identifier matching PanelWidgetDef::id
    virtual const char* id() const = 0;
};

/// Safe recovery of PanelWidget pointer from event callback.
/// Returns nullptr if widget was detached or obj has no user_data.
template <typename T> T* panel_widget_from_event(lv_event_t* e) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!obj)
        return nullptr;
    auto* raw = lv_obj_get_user_data(obj);
    if (!raw)
        return nullptr;
    return static_cast<T*>(raw);
}

} // namespace helix
