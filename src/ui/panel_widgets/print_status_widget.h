// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_runout_guidance_modal.h"

#include "panel_widget.h"

namespace helix {

class PrinterState;
enum class PrintJobState;

class PrintStatusWidget : public PanelWidget {
  public:
    PrintStatusWidget();
    ~PrintStatusWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "print_status";
    }

    /// Re-check runout condition after wizard completion
    void trigger_idle_runout_check();

    /// XML event callback — opens print status panel or file browser
    static void print_card_clicked_cb(lv_event_t* e);

  private:
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Cached widget references (looked up after XML creation)
    lv_obj_t* print_card_thumb_ = nullptr;        // Idle state thumbnail
    lv_obj_t* print_card_active_thumb_ = nullptr; // Active print thumbnail
    lv_obj_t* print_card_label_ = nullptr;        // Dynamic text label

    // PrinterState reference for subject access
    PrinterState& printer_state_;

    // Observers (RAII cleanup via ObserverGuard)
    ObserverGuard print_state_observer_;
    ObserverGuard print_progress_observer_;
    ObserverGuard print_time_left_observer_;
    ObserverGuard print_thumbnail_path_observer_;
    ObserverGuard filament_runout_observer_;

    // Filament runout modal
    RunoutGuidanceModal runout_modal_;
    bool runout_modal_shown_ = false;

    // Print card update methods
    void handle_print_card_clicked();
    void on_print_state_changed(PrintJobState state);
    void on_print_progress_or_time_changed();
    void on_print_thumbnail_path_changed(const char* path);
    void update_print_card_from_state();
    void update_print_card_label(int progress, int time_left_secs);
    void reset_print_card_to_idle();

    // Filament runout handling
    void check_and_show_idle_runout_modal();
    void show_idle_runout_modal();

};

} // namespace helix
