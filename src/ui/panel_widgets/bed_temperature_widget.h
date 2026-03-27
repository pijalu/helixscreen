// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "ui_heating_animator.h"
#include "ui_observer_guard.h"

#include "panel_widget.h"

#include <memory>

class TempControlPanel;

namespace helix {
class PrinterState;
}

namespace helix {

class BedTemperatureWidget : public PanelWidget {
  public:
    BedTemperatureWidget(PrinterState& printer_state, TempControlPanel* temp_panel);
    ~BedTemperatureWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "bed_temperature";
    }

    // XML event callback (public for early registration in register_bed_temperature_widget)
    static void bed_temp_clicked_cb(lv_event_t* e);

  private:
    PrinterState& printer_state_;
    TempControlPanel* temp_control_panel_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* temp_btn_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    HeatingIconAnimator temp_icon_animator_;
    int cached_bed_temp_ = 25;
    int cached_bed_target_ = 0;

    helix::AsyncLifetimeGuard lifetime_;

    ObserverGuard bed_temp_observer_;
    ObserverGuard bed_target_observer_;

    void on_bed_temp_changed(int temp_centi);
    void on_bed_target_changed(int target_centi);
    void update_temp_icon_animation();
    void handle_temp_clicked();
};

} // namespace helix
