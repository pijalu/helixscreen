// SPDX-License-Identifier: GPL-3.0-or-later

#include "nozzle_temps_widget.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "tool_state.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace helix {

void register_nozzle_temps_widget() {
    register_widget_factory("nozzle_temps", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<NozzleTempsWidget>(ps);
    });
}

} // namespace helix

using namespace helix;

NozzleTempsWidget::NozzleTempsWidget(PrinterState& printer_state)
    : printer_state_(printer_state) {}

NozzleTempsWidget::~NozzleTempsWidget() {
    detach();
}

void NozzleTempsWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;
    spdlog::debug("[NozzleTempsWidget] Attached (skeleton)");
}

void NozzleTempsWidget::detach() {
    *alive_ = false;
    clear_rows();
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void NozzleTempsWidget::clear_rows() {
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();

    version_observer_.reset();
    extruder_rows_.clear();
    bed_temp_observer_.reset();
    bed_target_observer_.reset();

    auto* container = widget_obj_ ? lv_obj_find_by_name(widget_obj_, "nozzle_temps_container")
                                  : nullptr;
    if (container)
        lv_obj_clean(container);

    bed_row_ = nullptr;
    bed_temp_label_ = nullptr;
    bed_target_label_ = nullptr;
    bed_progress_bar_ = nullptr;
    cached_bed_temp_ = 0;
    cached_bed_target_ = 0;
}

void NozzleTempsWidget::rebuild_rows() {
    // Placeholder — implemented in next commit
}

void NozzleTempsWidget::create_extruder_row(lv_obj_t* /*container*/, ExtruderRow& /*row*/) {
    // Placeholder — implemented in next commit
}

void NozzleTempsWidget::create_bed_row(lv_obj_t* /*container*/) {
    // Placeholder — implemented in next commit
}

void NozzleTempsWidget::update_row_display(lv_obj_t* /*temp_label*/, lv_obj_t* /*target_label*/,
                                           lv_obj_t* /*progress_bar*/, int /*temp_centi*/,
                                           int /*target_centi*/, bool /*is_bed*/) {
    // Placeholder — implemented in next commit
}
