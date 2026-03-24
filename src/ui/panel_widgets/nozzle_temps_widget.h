// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "panel_widget.h"
#include "ui_observer_guard.h"

#include <memory>
#include <string>
#include <vector>

namespace helix {

class PrinterState;

/// Panel widget showing per-extruder temperature rows with progress bars.
/// Gated on show_tool_badge (multi-tool printers only). Displays each
/// extruder's current/target temperature and a colored progress bar,
/// plus a bed temperature row at the bottom.
class NozzleTempsWidget : public PanelWidget {
  public:
    explicit NozzleTempsWidget(PrinterState& printer_state);
    ~NozzleTempsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override { return "nozzle_temps"; }

  private:
    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(false);

    struct ExtruderRow {
        std::string name;
        lv_obj_t* row_obj = nullptr;
        lv_obj_t* temp_label = nullptr;
        lv_obj_t* target_label = nullptr;
        lv_obj_t* progress_bar = nullptr;
        ObserverGuard temp_observer;
        ObserverGuard target_observer;
        SubjectLifetime temp_lifetime;
        SubjectLifetime target_lifetime;
        int cached_temp = 0;
        int cached_target = 0;
    };

    std::vector<ExtruderRow> extruder_rows_;

    lv_obj_t* bed_row_ = nullptr;
    lv_obj_t* bed_temp_label_ = nullptr;
    lv_obj_t* bed_target_label_ = nullptr;
    lv_obj_t* bed_progress_bar_ = nullptr;
    ObserverGuard bed_temp_observer_;
    ObserverGuard bed_target_observer_;
    int cached_bed_temp_ = 0;
    int cached_bed_target_ = 0;

    ObserverGuard version_observer_;

    void rebuild_rows();
    void clear_rows();
    void create_extruder_row(lv_obj_t* container, ExtruderRow& row);
    void create_bed_row(lv_obj_t* container);
    void update_row_display(lv_obj_t* temp_label, lv_obj_t* target_label,
                            lv_obj_t* progress_bar, int temp_centi, int target_centi, bool is_bed);
};

void register_nozzle_temps_widget();

} // namespace helix
