// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "ui_heating_animator.h"
#include "ui_observer_guard.h"

#include "panel_widget.h"

#include <memory>

class TemperatureService;

namespace helix {
class PrinterState;
}

namespace helix {

class TemperatureWidget : public PanelWidget {
  public:
    TemperatureWidget(PrinterState& printer_state, TemperatureService* temp_panel);
    ~TemperatureWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "temperature";
    }

    // XML event callback (public for early registration in register_temperature_widget)
    static void temp_clicked_cb(lv_event_t* e);

  private:
    PrinterState& printer_state_;
    TemperatureService* temp_control_panel_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* temp_btn_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    HeatingIconAnimator temp_icon_animator_;
    int cached_extruder_temp_ = 25;
    int cached_extruder_target_ = 0;

    helix::AsyncLifetimeGuard lifetime_;

    ObserverGuard extruder_temp_observer_;
    ObserverGuard extruder_target_observer_;

    void on_extruder_temp_changed(int temp_centi);
    void on_extruder_target_changed(int target_centi);
    void update_temp_icon_animation();
    void handle_temp_clicked();
};

} // namespace helix
