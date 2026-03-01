// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"

namespace helix {

class PrinterImageWidget : public PanelWidget {
  public:
    PrinterImageWidget();
    ~PrinterImageWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "printer_image";
    }

    /// Called when panel activates — re-check if printer image changed in settings
    void on_activate();

    /// Reload printer image and printer info subjects from config
    void reload_from_config();

    /// Re-check printer image setting and update the displayed image
    void refresh_printer_image();

    /// XML event callback — opens printer manager overlay
    static void printer_manager_clicked_cb(lv_event_t* e);

  private:
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    // Pre-scaled printer image snapshot — eliminates per-frame bilinear scaling
    lv_draw_buf_t* cached_printer_snapshot_ = nullptr;
    lv_timer_t* snapshot_timer_ = nullptr;

    void schedule_printer_image_snapshot();
    void take_printer_image_snapshot();

    void handle_printer_manager_clicked();
};

} // namespace helix
