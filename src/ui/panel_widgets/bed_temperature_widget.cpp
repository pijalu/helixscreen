// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bed_temperature_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_temp_control.h"
#include "ui_temperature_utils.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "observer_factory.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

namespace helix {
void register_bed_temperature_widget() {
    register_widget_factory("bed_temperature", [](const std::string&) {
        auto& ps = get_printer_state();
        auto* tcp = PanelWidgetManager::instance().shared_resource<TempControlPanel>();
        return std::make_unique<BedTemperatureWidget>(ps, tcp);
    });

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "bed_temp_clicked_cb",
                             BedTemperatureWidget::bed_temp_clicked_cb);
}
} // namespace helix

using namespace helix;
using helix::ui::temperature::centi_to_degrees;

BedTemperatureWidget::BedTemperatureWidget(PrinterState& printer_state,
                                           TempControlPanel* temp_panel)
    : printer_state_(printer_state), temp_control_panel_(temp_panel) {}

BedTemperatureWidget::~BedTemperatureWidget() {
    detach();
}

void BedTemperatureWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Set user_data on the root lv_obj, NOT on the ui_button child.
    // ui_button allocates its own UiButtonData in user_data — overwriting it
    // leaks memory and breaks button style/contrast auto-updates.
    lv_obj_set_user_data(widget_obj_, this);

    temp_btn_ = lv_obj_find_by_name(widget_obj_, "bed_temp_btn");
    if (temp_btn_) {
        lv_obj_add_event_cb(temp_btn_, bed_temp_clicked_cb, LV_EVENT_CLICKED, this);
    }

    // Set up temperature observers with lifetime guard
    using helix::ui::observe_int_sync;
    auto token = lifetime_.token();

    bed_temp_observer_ =
        observe_int_sync<BedTemperatureWidget>(printer_state_.get_bed_temp_subject(), this,
                                               [token](BedTemperatureWidget* self, int temp) {
                                                   if (token.expired())
                                                       return;
                                                   self->on_bed_temp_changed(temp);
                                               });
    bed_target_observer_ = observe_int_sync<BedTemperatureWidget>(
        printer_state_.get_bed_target_subject(), this,
        [token](BedTemperatureWidget* self, int target) {
            if (token.expired())
                return;
            self->on_bed_target_changed(target);
        });

    // Attach heating icon animator
    lv_obj_t* bed_icon = lv_obj_find_by_name(widget_obj_, "bed_icon_glyph");
    if (bed_icon) {
        temp_icon_animator_.attach(bed_icon);
        cached_bed_temp_ = lv_subject_get_int(printer_state_.get_bed_temp_subject());
        cached_bed_target_ = lv_subject_get_int(printer_state_.get_bed_target_subject());
        temp_icon_animator_.update(cached_bed_temp_, cached_bed_target_);
        spdlog::debug("[BedTemperatureWidget] Heating icon animator attached");
    }

    spdlog::debug("[BedTemperatureWidget] Attached");
}

void BedTemperatureWidget::detach() {
    lifetime_.invalidate();
    temp_icon_animator_.detach();
    bed_temp_observer_.reset();
    bed_target_observer_.reset();

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
    temp_btn_ = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    spdlog::debug("[BedTemperatureWidget] Detached");
}

void BedTemperatureWidget::on_bed_temp_changed(int temp_centi) {
    cached_bed_temp_ = temp_centi;
    update_temp_icon_animation();
    spdlog::trace("[BedTemperatureWidget] Bed temp: {}°C", centi_to_degrees(temp_centi));
}

void BedTemperatureWidget::on_bed_target_changed(int target_centi) {
    cached_bed_target_ = target_centi;
    update_temp_icon_animation();
    spdlog::trace("[BedTemperatureWidget] Bed target: {}°C", centi_to_degrees(target_centi));
}

void BedTemperatureWidget::update_temp_icon_animation() {
    temp_icon_animator_.update(cached_bed_temp_, cached_bed_target_);
}

void BedTemperatureWidget::handle_temp_clicked() {
    spdlog::info(
        "[BedTemperatureWidget] Bed temperature icon clicked - opening temp graph overlay");
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, parent_screen_);
}

void BedTemperatureWidget::bed_temp_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BedTemperatureWidget] bed_temp_clicked_cb");
    auto* self = static_cast<BedTemperatureWidget*>(lv_event_get_user_data(e));
    if (self) {
        self->record_interaction();
        self->handle_temp_clicked();
    }
    LVGL_SAFE_EVENT_CB_END();
}
