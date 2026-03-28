// SPDX-License-Identifier: GPL-3.0-or-later

#include "nozzle_temps_widget.h"

#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "theme_manager.h"
#include "tool_state.h"
#include "ui_overlay_temp_graph.h"

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

    rebuild_rows();

    // Observe extruder version changes to rebuild rows when tools are discovered.
    // Capture current version to skip the initial immediate callback — rows already built.
    auto token = lifetime_.token();
    int initial_version = lv_subject_get_int(printer_state_.get_extruder_version_subject());
    version_observer_ = helix::ui::observe_int_sync<NozzleTempsWidget>(
        printer_state_.get_extruder_version_subject(), this,
        [token, initial_version](NozzleTempsWidget* self, int version) {
            if (token.expired())
                return;
            if (version == initial_version)
                return; // Skip initial callback — rows already built in attach()
            self->rebuild_rows();
        });

    spdlog::debug("[NozzleTempsWidget] Attached with {} extruder rows", extruder_rows_.size());
}

void NozzleTempsWidget::detach() {
    lifetime_.invalidate();
    version_observer_.reset();
    clear_rows();
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}

void NozzleTempsWidget::clear_rows() {
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();

    ++rebuild_gen_; // Bump generation to invalidate stale deferred callbacks
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
    clear_rows();

    auto* container = widget_obj_ ? lv_obj_find_by_name(widget_obj_, "nozzle_temps_container")
                                  : nullptr;
    if (!container) {
        spdlog::warn("[NozzleTempsWidget] Container not found in XML");
        return;
    }

    auto token = lifetime_.token();

    // Create extruder rows ordered by ToolState tools
    const auto& tools = ToolState::instance().tools();
    for (const auto& tool : tools) {
        if (!tool.extruder_name)
            continue;

        ExtruderRow row;
        row.name = *tool.extruder_name;
        create_extruder_row(container, row);

        // Observe per-extruder temp subject with lifetime token
        lv_subject_t* temp_subj =
            printer_state_.get_extruder_temp_subject(row.name, row.temp_lifetime);
        lv_subject_t* target_subj =
            printer_state_.get_extruder_target_subject(row.name, row.target_lifetime);

        if (temp_subj) {
            row.cached_temp = lv_subject_get_int(temp_subj);
            auto* temp_lbl = row.temp_label;
            auto* target_lbl = row.target_label;
            auto* bar = row.progress_bar;
            row.temp_observer = helix::ui::observe_int_sync<NozzleTempsWidget>(
                temp_subj, this,
                [token, idx = extruder_rows_.size(), temp_lbl, target_lbl,
                 bar](NozzleTempsWidget* self, int temp) {
                    if (token.expired())
                        return;
                    if (idx < self->extruder_rows_.size()) {
                        self->extruder_rows_[idx].cached_temp = temp;
                        self->update_row_display(temp_lbl, target_lbl, bar, temp,
                                                 self->extruder_rows_[idx].cached_target, false);
                    }
                },
                row.temp_lifetime);
        }

        if (target_subj) {
            row.cached_target = lv_subject_get_int(target_subj);
            auto* temp_lbl = row.temp_label;
            auto* target_lbl = row.target_label;
            auto* bar = row.progress_bar;
            row.target_observer = helix::ui::observe_int_sync<NozzleTempsWidget>(
                target_subj, this,
                [token, idx = extruder_rows_.size(), temp_lbl, target_lbl,
                 bar](NozzleTempsWidget* self, int target) {
                    if (token.expired())
                        return;
                    if (idx < self->extruder_rows_.size()) {
                        self->extruder_rows_[idx].cached_target = target;
                        self->update_row_display(temp_lbl, target_lbl, bar,
                                                 self->extruder_rows_[idx].cached_temp, target,
                                                 false);
                    }
                },
                row.target_lifetime);
        }

        // Initial display update
        update_row_display(row.temp_label, row.target_label, row.progress_bar, row.cached_temp,
                           row.cached_target, false);

        extruder_rows_.push_back(std::move(row));
    }

    // Bed row at the end
    create_bed_row(container);

    // Bed observers (static subjects, no lifetime token needed)
    lv_subject_t* bed_temp_subj = printer_state_.get_bed_temp_subject();
    lv_subject_t* bed_target_subj = printer_state_.get_bed_target_subject();

    if (bed_temp_subj) {
        cached_bed_temp_ = lv_subject_get_int(bed_temp_subj);
        bed_temp_observer_ = helix::ui::observe_int_sync<NozzleTempsWidget>(
            bed_temp_subj, this,
            [token](NozzleTempsWidget* self, int temp) {
                if (token.expired())
                    return;
                self->cached_bed_temp_ = temp;
                self->update_row_display(self->bed_temp_label_, self->bed_target_label_,
                                         self->bed_progress_bar_, temp, self->cached_bed_target_,
                                         true);
            });
    }

    if (bed_target_subj) {
        cached_bed_target_ = lv_subject_get_int(bed_target_subj);
        bed_target_observer_ = helix::ui::observe_int_sync<NozzleTempsWidget>(
            bed_target_subj, this,
            [token](NozzleTempsWidget* self, int target) {
                if (token.expired())
                    return;
                self->cached_bed_target_ = target;
                self->update_row_display(self->bed_temp_label_, self->bed_target_label_,
                                         self->bed_progress_bar_, self->cached_bed_temp_, target,
                                         true);
            });
    }

    update_row_display(bed_temp_label_, bed_target_label_, bed_progress_bar_, cached_bed_temp_,
                       cached_bed_target_, true);

    spdlog::debug("[NozzleTempsWidget] Rebuilt with {} extruder rows + bed", extruder_rows_.size());
}

void NozzleTempsWidget::create_extruder_row(lv_obj_t* container, ExtruderRow& row) {
    // Resolve display name for the tool
    std::string tool_name = ToolState::instance().tool_name_for_extruder(row.name);
    if (tool_name.empty())
        tool_name = row.name;

    // Create row from XML template — layout, fonts, colors are all declarative
    const char* attrs[] = {"tool_name", tool_name.c_str(), nullptr};
    lv_obj_t* row_obj = static_cast<lv_obj_t*>(lv_xml_create(container, "nozzle_temp_row", attrs));
    if (!row_obj) {
        spdlog::error("[NozzleTempsWidget] lv_xml_create('nozzle_temp_row') returned NULL for '{}'",
                      row.name);
        return;
    }

    row.row_obj = row_obj;
    row.temp_label = lv_obj_find_by_name(row_obj, "temp_label");
    row.target_label = lv_obj_find_by_name(row_obj, "target_label");
    row.progress_bar = lv_obj_find_by_name(row_obj, "progress_bar");

    // Tap row → open nozzle temp graph overlay
    lv_obj_add_flag(row_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* screen = parent_screen_;
    lv_obj_add_event_cb(
        row_obj,
        [](lv_event_t* e) {
            auto* scr = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
            if (scr) {
                get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Nozzle, scr);
            }
        },
        LV_EVENT_CLICKED, screen);
}

void NozzleTempsWidget::create_bed_row(lv_obj_t* container) {
    // Create bed row from XML template — divider, layout, colors are all declarative
    lv_obj_t* row_obj =
        static_cast<lv_obj_t*>(lv_xml_create(container, "nozzle_temp_bed_row", nullptr));
    if (!row_obj) {
        spdlog::error("[NozzleTempsWidget] lv_xml_create('nozzle_temp_bed_row') returned NULL");
        return;
    }

    bed_row_ = row_obj;
    bed_temp_label_ = lv_obj_find_by_name(row_obj, "bed_temp_label");
    bed_target_label_ = lv_obj_find_by_name(row_obj, "bed_target_label");
    bed_progress_bar_ = lv_obj_find_by_name(row_obj, "bed_progress_bar");

    // Tap bed row → open bed temp graph overlay
    lv_obj_add_flag(row_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* screen = parent_screen_;
    lv_obj_add_event_cb(
        row_obj,
        [](lv_event_t* e) {
            auto* scr = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
            if (scr) {
                get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, scr);
            }
        },
        LV_EVENT_CLICKED, screen);
}

void NozzleTempsWidget::update_row_display(lv_obj_t* temp_label, lv_obj_t* target_label,
                                           lv_obj_t* /* progress_bar */, int temp_centi,
                                           int target_centi, bool /* is_bed */) {
    if (!temp_label || !target_label)
        return;

    auto result = helix::ui::temperature::heater_display(temp_centi, target_centi);

    // Current temp with color coding (green=at-temp, red=heating, blue=cooling, gray=off)
    lv_label_set_text_fmt(temp_label, "%d\xC2\xB0", temp_centi / 10);
    lv_obj_set_style_text_color(temp_label, result.color, LV_PART_MAIN);

    if (target_centi > 0) {
        lv_label_set_text_fmt(target_label, "/ %d\xC2\xB0", target_centi / 10);
    } else {
        lv_label_set_text(target_label, "off");
    }
}
