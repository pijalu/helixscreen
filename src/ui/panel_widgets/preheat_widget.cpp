// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "preheat_widget.h"

#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_overlay_temp_graph.h"
#include "ui_split_button.h"

#include "app_globals.h"
#include "filament_database.h"
#include "moonraker_api.h"
#include "panel_widget_registry.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <cstdio>

static constexpr const char* PRESET_NAMES[] = {"PLA", "PETG", "ABS", "TPU"};
static constexpr int PRESET_COUNT = 4;

// Static instance for callback dispatch (one preheat widget at a time)
static helix::PreheatWidget* s_active_instance = nullptr;

namespace helix {

void register_preheat_widget() {
    register_widget_factory("preheat", [](const std::string&) {
        auto& ps = get_printer_state();
        return std::make_unique<PreheatWidget>(ps);
    });

    lv_xml_register_event_cb(nullptr, "preheat_widget_apply_cb", PreheatWidget::preheat_apply_cb);
    lv_xml_register_event_cb(nullptr, "preheat_widget_changed_cb",
                             PreheatWidget::preheat_changed_cb);
    lv_xml_register_event_cb(nullptr, "preheat_nozzle_tap_cb", PreheatWidget::nozzle_tap_cb);
    lv_xml_register_event_cb(nullptr, "preheat_bed_tap_cb", PreheatWidget::bed_tap_cb);
}

PreheatWidget::PreheatWidget(PrinterState& printer_state) : printer_state_(printer_state) {}

PreheatWidget::~PreheatWidget() {
    detach();
}

void PreheatWidget::set_config(const nlohmann::json& config) {
    config_ = config;
    if (config_.contains("material_index") && config_["material_index"].is_number_integer()) {
        selected_material_ = config_["material_index"].get<int>();
        if (selected_material_ < 0 || selected_material_ >= PRESET_COUNT) {
            selected_material_ = 0;
        }
    }
}

void PreheatWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    s_active_instance = this;

    split_btn_ = lv_obj_find_by_name(widget_obj_, "preheat_split_btn");
    if (split_btn_) {
        lv_obj_set_user_data(split_btn_, this);
        ui_split_button_set_selected(split_btn_, static_cast<uint32_t>(selected_material_));
        update_button_label();
    }

    // Update label when widget resizes (e.g. breakpoint change, initial layout)
    lv_obj_add_event_cb(
        widget_obj_,
        [](lv_event_t* e) {
            (void)e;
            if (s_active_instance) {
                s_active_instance->update_button_label();
            }
        },
        LV_EVENT_SIZE_CHANGED, nullptr);

    // Set user_data on temp tap rows for callback recovery
    lv_obj_t* nozzle_row = lv_obj_find_by_name(widget_obj_, "preheat_nozzle_row");
    if (nozzle_row)
        lv_obj_set_user_data(nozzle_row, this);
    lv_obj_t* bed_row = lv_obj_find_by_name(widget_obj_, "preheat_bed_row");
    if (bed_row)
        lv_obj_set_user_data(bed_row, this);

    spdlog::debug("[PreheatWidget] Attached (material={})", PRESET_NAMES[selected_material_]);
}

void PreheatWidget::detach() {
    if (s_active_instance == this) {
        s_active_instance = nullptr;
    }

    if (split_btn_) {
        lv_obj_set_user_data(split_btn_, nullptr);
        split_btn_ = nullptr;
    }

    lv_obj_t* nozzle_row =
        widget_obj_ ? lv_obj_find_by_name(widget_obj_, "preheat_nozzle_row") : nullptr;
    if (nozzle_row)
        lv_obj_set_user_data(nozzle_row, nullptr);
    lv_obj_t* bed_row = widget_obj_ ? lv_obj_find_by_name(widget_obj_, "preheat_bed_row") : nullptr;
    if (bed_row)
        lv_obj_set_user_data(bed_row, nullptr);

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    spdlog::debug("[PreheatWidget] Detached");
}

void PreheatWidget::update_button_label() {
    if (!split_btn_)
        return;

    auto mat = filament::find_material(PRESET_NAMES[selected_material_]);
    int nozzle = mat ? mat->nozzle_recommended() : 0;
    int bed = mat ? mat->bed_temp : 0;

    char label[64];
    int32_t w = widget_obj_ ? lv_obj_get_width(widget_obj_) : 0;
    if (w > 0 && w < 280) {
        // Narrow (2-col): just material name
        std::snprintf(label, sizeof(label), "%s", PRESET_NAMES[selected_material_]);
    } else if (nozzle > 0 && bed > 0) {
        // Wide (3-col+): material + target temps
        std::snprintf(label, sizeof(label), "Preheat %s (%d/%d)", PRESET_NAMES[selected_material_],
                      nozzle, bed);
    } else {
        std::snprintf(label, sizeof(label), "Preheat %s", PRESET_NAMES[selected_material_]);
    }
    ui_split_button_set_text(split_btn_, label);
}

void PreheatWidget::handle_selection_changed() {
    if (!split_btn_)
        return;

    uint32_t idx = ui_split_button_get_selected(split_btn_);
    if (idx < static_cast<uint32_t>(PRESET_COUNT)) {
        selected_material_ = static_cast<int>(idx);
        update_button_label();

        // Persist selection
        nlohmann::json new_config = config_;
        new_config["material_index"] = selected_material_;
        save_widget_config(new_config);
        config_ = new_config;

        spdlog::info("[PreheatWidget] Material changed to {}", PRESET_NAMES[selected_material_]);
    }
}

void PreheatWidget::handle_apply() {
    auto mat = filament::find_material(PRESET_NAMES[selected_material_]);
    if (!mat) {
        spdlog::error("[PreheatWidget] Material '{}' not found", PRESET_NAMES[selected_material_]);
        return;
    }

    int nozzle = mat->nozzle_recommended();
    int bed = mat->bed_temp;

    auto* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[PreheatWidget] No MoonrakerAPI available");
        return;
    }

    api->set_temperature(
        printer_state_.active_extruder_name(), static_cast<double>(nozzle),
        [nozzle]() { NOTIFY_SUCCESS("Nozzle target set to {}°C", nozzle); },
        [](const MoonrakerError& error) {
            NOTIFY_ERROR("Failed to set nozzle temp: {}", error.user_message());
        });
    api->set_temperature(
        "heater_bed", static_cast<double>(bed),
        [bed]() { NOTIFY_SUCCESS("Bed target set to {}°C", bed); },
        [](const MoonrakerError& error) {
            NOTIFY_ERROR("Failed to set bed temp: {}", error.user_message());
        });

    spdlog::info("[PreheatWidget] Preheat {} applied (nozzle={}°C, bed={}°C)",
                 PRESET_NAMES[selected_material_], nozzle, bed);
}

void PreheatWidget::handle_nozzle_tap() {
    spdlog::info("[PreheatWidget] Nozzle tapped - opening temp graph overlay");
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Nozzle, parent_screen_);
}

void PreheatWidget::handle_bed_tap() {
    spdlog::info("[PreheatWidget] Bed tapped - opening temp graph overlay");
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::Bed, parent_screen_);
}

// Static callbacks
void PreheatWidget::preheat_apply_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PreheatWidget] preheat_apply_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_apply();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PreheatWidget::preheat_changed_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PreheatWidget] preheat_changed_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_selection_changed();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PreheatWidget::nozzle_tap_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PreheatWidget] nozzle_tap_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_nozzle_tap();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void PreheatWidget::bed_tap_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PreheatWidget] bed_tap_cb");
    (void)e;
    if (s_active_instance) {
        s_active_instance->handle_bed_tap();
    }
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix
