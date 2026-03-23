// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "temp_graph_widget.h"

#include "app_globals.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "temperature_history_manager.h"
#include "temperature_sensor_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_temperature_utils.h"
#include "ui_update_queue.h"

#include "helix-xml/src/xml/lv_xml.h"

#include <spdlog/spdlog.h>

#include <chrono>

namespace helix {

// Color palette matching TempGraphOverlay for visual consistency
const lv_color_t TempGraphWidget::SERIES_COLORS[PALETTE_SIZE] = {
    lv_color_hex(0xFF4444), // Nozzle (red)
    lv_color_hex(0x88C0D0), // Bed (cyan / nord8)
    lv_color_hex(0xA3BE8C), // Chamber (green / nord14)
    lv_color_hex(0xEBCB8B), // Yellow / nord13
    lv_color_hex(0xB48EAD), // Purple / nord15
    lv_color_hex(0xD08770), // Orange / nord12
    lv_color_hex(0x5E81AC), // Blue / nord10
    lv_color_hex(0xBF616A), // Dark red / nord11
};

void register_temp_graph_widget() {
    register_widget_factory("temp_graph", [](const std::string& id) {
        return std::make_unique<TempGraphWidget>(id);
    });

    lv_xml_register_event_cb(nullptr, "on_temp_graph_widget_clicked",
                             TempGraphWidget::on_temp_graph_widget_clicked);
}

} // namespace helix

using namespace helix;
using helix::ui::observe_int_sync;
using helix::ui::temperature::centi_to_degrees_f;

// ============================================================================
// Construction / Destruction
// ============================================================================

TempGraphWidget::TempGraphWidget(const std::string& instance_id)
    : instance_id_(instance_id) {
    spdlog::debug("[TempGraphWidget] Created instance '{}'", instance_id_);
}

TempGraphWidget::~TempGraphWidget() {
    if (widget_obj_) {
        detach();
    }
}

// ============================================================================
// PanelWidget interface
// ============================================================================

std::string TempGraphWidget::get_component_name() const {
    return "panel_widget_temp_graph";
}

void TempGraphWidget::set_config(const nlohmann::json& config) {
    config_ = config;
}

void TempGraphWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    alive_ = std::make_shared<bool>(true);
    ++generation_;

    // Store self-pointer for click callback routing
    lv_obj_set_user_data(widget_obj_, this);

    // Create the temperature graph inside the widget container
    graph_ = ui_temp_graph_create(widget_obj_);
    if (!graph_) {
        spdlog::error("[TempGraphWidget] Failed to create temp graph for '{}'", instance_id_);
        return;
    }

    // Size chart to fill the container
    lv_obj_t* chart = ui_temp_graph_get_chart(graph_);
    if (chart) {
        lv_obj_set_size(chart, LV_PCT(100), LV_PCT(100));
    }

    // Set features based on current grid size
    uint32_t features = features_for_size(current_colspan_, current_rowspan_);
    ui_temp_graph_set_features(graph_, features);

    // Build default config if not yet configured
    if (config_.empty() || !config_.contains("sensors")) {
        build_default_config();
    }

    setup_series();
    setup_observers();
    backfill_history();
    apply_auto_range();

    spdlog::debug("[TempGraphWidget] Attached '{}' with {} series ({}x{})",
                  instance_id_, series_.size(), current_colspan_, current_rowspan_);
}

void TempGraphWidget::detach() {
    if (alive_) {
        *alive_ = false;
    }

    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze("TempGraphWidget::detach");
        helix::ui::UpdateQueue::instance().drain();

        // Reset all observers
        connection_observer_.reset();
        for (auto& s : series_) {
            s.temp_obs.reset();
            s.target_obs.reset();
        }
    }

    series_.clear();

    if (graph_) {
        ui_temp_graph_destroy(graph_);
        graph_ = nullptr;
    }

    widget_obj_ = nullptr;
    parent_screen_ = nullptr;

    spdlog::debug("[TempGraphWidget] Detached '{}'", instance_id_);
}

void TempGraphWidget::on_size_changed(int colspan, int rowspan, int /*width_px*/, int /*height_px*/) {
    current_colspan_ = colspan;
    current_rowspan_ = rowspan;

    if (graph_) {
        uint32_t features = features_for_size(colspan, rowspan);
        ui_temp_graph_set_features(graph_, features);
        spdlog::debug("[TempGraphWidget] '{}' resized to {}x{}, features=0x{:x}",
                      instance_id_, colspan, rowspan, features);
    }
}

void TempGraphWidget::on_activate() {
    paused_ = false;
    backfill_history();
}

void TempGraphWidget::on_deactivate() {
    paused_ = true;
}

bool TempGraphWidget::on_edit_configure() {
    std::weak_ptr<bool> weak = alive_;
    auto* saved_widget = widget_obj_;
    auto* saved_parent = parent_screen_;

    config_modal_ = std::make_unique<TempGraphConfigModal>(
        config_,
        [this, weak, saved_widget, saved_parent](const nlohmann::json& new_config) {
            if (weak.expired()) return;
            config_ = new_config;
            save_widget_config(config_);
            generation_++;
            detach();
            attach(saved_widget, saved_parent);
        });
    config_modal_->show(lv_screen_active());
    return true;
}

// ============================================================================
// Feature mapping
// ============================================================================

uint32_t TempGraphWidget::features_for_size(int colspan, int rowspan) {
    uint32_t features = TEMP_GRAPH_FEATURE_LINES;

    if (colspan >= 2 && rowspan >= 1) {
        // Wide: add target lines, legend, x-axis
        features |= TEMP_GRAPH_FEATURE_TARGET_LINES
                  | TEMP_GRAPH_FEATURE_LEGEND
                  | TEMP_GRAPH_FEATURE_X_AXIS;
    }

    if (colspan >= 1 && rowspan >= 2) {
        // Tall: add target lines, legend, y-axis
        features |= TEMP_GRAPH_FEATURE_TARGET_LINES
                  | TEMP_GRAPH_FEATURE_LEGEND
                  | TEMP_GRAPH_FEATURE_Y_AXIS;
    }

    if (colspan >= 2 && rowspan >= 2) {
        // Large: add gradients
        features |= TEMP_GRAPH_FEATURE_GRADIENTS;
    }

    if (colspan >= 3 && rowspan >= 2) {
        // Extra large: add readouts
        features |= TEMP_GRAPH_FEATURE_READOUTS;
    }

    return features;
}

// ============================================================================
// Series setup
// ============================================================================

void TempGraphWidget::setup_series() {
    if (!graph_ || !config_.contains("sensors")) return;

    int color_idx = 0;
    const auto& sensors = config_["sensors"];

    for (const auto& entry : sensors) {
        if (!entry.contains("name") || !entry.value("enabled", true)) continue;

        std::string name = entry["name"].get<std::string>();

        // Determine color from config or use palette
        lv_color_t color;
        if (entry.contains("color")) {
            color = lv_color_hex(entry["color"].get<uint32_t>());
        } else {
            color = SERIES_COLORS[color_idx % PALETTE_SIZE];
        }
        color_idx++;

        int series_id = ui_temp_graph_add_series(graph_, name.c_str(), color);
        if (series_id < 0) {
            spdlog::warn("[TempGraphWidget] Failed to add series '{}'", name);
            continue;
        }

        SeriesInfo info;
        info.name = name;
        info.series_id = series_id;
        info.color = color;

        // Determine if this is a heater (has target) or sensor-only
        if (name == "extruder" || name.find("extruder") == 0 || name == "heater_bed") {
            info.has_target = true;
        } else if (name == "chamber") {
            // Chamber may be sensor-only (no target) or have a heater
            // Check via the same subject the overlay uses
            lv_subject_t* chamber_gate = lv_xml_get_subject(nullptr, "printer_has_chamber_sensor");
            info.has_target = (chamber_gate && lv_subject_get_int(chamber_gate) != 0);
        }

        // Mark dynamic subjects
        auto& ps = get_printer_state();
        if (name.find("extruder") == 0 && ps.extruder_count() > 1) {
            info.is_dynamic = true;
        } else if (name.find("temperature_sensor") == 0 || name.find("temperature_fan") == 0) {
            info.is_dynamic = true;
        }

        series_.push_back(std::move(info));
    }

    spdlog::debug("[TempGraphWidget] Setup {} series for '{}'", series_.size(), instance_id_);
}

void TempGraphWidget::setup_observers() {
    auto& ps = get_printer_state();
    std::weak_ptr<bool> weak_alive = alive_;
    uint32_t gen = generation_;

    for (size_t i = 0; i < series_.size(); ++i) {
        auto& s = series_[i];

        lv_subject_t* temp_subj = nullptr;
        lv_subject_t* target_subj = nullptr;

        if (s.name == "heater_bed") {
            temp_subj = ps.get_bed_temp_subject();
            target_subj = ps.get_bed_target_subject();
        } else if (s.name == "chamber") {
            temp_subj = ps.get_chamber_temp_subject();
            target_subj = ps.get_chamber_target_subject();
        } else if (s.name.find("extruder") == 0) {
            if (ps.extruder_count() <= 1) {
                // Single extruder: use active (static) subjects
                temp_subj = ps.get_active_extruder_temp_subject();
                target_subj = ps.get_active_extruder_target_subject();
            } else {
                // Multi-extruder: use per-extruder (dynamic) subjects
                temp_subj = ps.get_extruder_temp_subject(s.name, s.lifetime);
                target_subj = ps.get_extruder_target_subject(s.name, s.lifetime);
            }
        } else {
            // Auxiliary sensor from TemperatureSensorManager
            auto& sensor_mgr = sensors::TemperatureSensorManager::instance();
            temp_subj = sensor_mgr.get_temp_subject(s.name, s.lifetime);
        }

        if (temp_subj) {
            size_t idx = i;
            s.temp_obs = observe_int_sync<TempGraphWidget>(
                temp_subj, this,
                [weak_alive, gen, idx](TempGraphWidget* self, int temp_centi) {
                    if (weak_alive.expired() || gen != self->generation_) return;
                    if (self->paused_ || !self->graph_) return;

                    auto& si = self->series_[idx];
                    if (si.series_id < 0) return;

                    float temp_deg = centi_to_degrees_f(temp_centi);
                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
                    ui_temp_graph_update_series_with_time(self->graph_, si.series_id,
                                                          temp_deg, now_ms);
                },
                s.lifetime);
        }

        if (target_subj && s.has_target) {
            size_t idx = i;
            s.target_obs = observe_int_sync<TempGraphWidget>(
                target_subj, this,
                [weak_alive, gen, idx](TempGraphWidget* self, int target_centi) {
                    if (weak_alive.expired() || gen != self->generation_) return;
                    if (!self->graph_) return;

                    auto& si = self->series_[idx];
                    if (si.series_id < 0) return;

                    float target_deg = centi_to_degrees_f(target_centi);
                    bool show = target_deg > 0.0f;
                    ui_temp_graph_set_series_target(self->graph_, si.series_id, target_deg, show);
                },
                s.lifetime);
        }
    }
}

// ============================================================================
// History backfill
// ============================================================================

void TempGraphWidget::backfill_history() {
    auto* history_mgr = get_temperature_history_manager();
    if (!graph_ || !history_mgr) return;

    for (auto& s : series_) {
        if (s.series_id < 0) continue;

        auto samples = history_mgr->get_samples(s.name);
        if (samples.empty()) continue;

        for (const auto& sample : samples) {
            float temp_deg = centi_to_degrees_f(sample.temp_centi);
            ui_temp_graph_update_series_with_time(graph_, s.series_id,
                                                  temp_deg, sample.timestamp_ms);
        }

        // Set initial target from most recent sample
        if (s.has_target) {
            float target_deg = centi_to_degrees_f(samples.back().target_centi);
            if (target_deg > 0.0f) {
                ui_temp_graph_set_series_target(graph_, s.series_id, target_deg, true);
            }
        }
    }
}

// ============================================================================
// Auto-range
// ============================================================================

void TempGraphWidget::apply_auto_range() {
    if (!graph_) return;

    // Check if any series is a heater (needs higher range)
    bool has_heater = false;
    for (const auto& s : series_) {
        if (s.has_target) {
            has_heater = true;
            break;
        }
    }

    float max_temp = has_heater ? 300.0f : 100.0f;
    ui_temp_graph_set_temp_range(graph_, 0.0f, max_temp);
}

// ============================================================================
// Default config
// ============================================================================

void TempGraphWidget::build_default_config() {
    nlohmann::json sensors = nlohmann::json::array();

    // Always include extruder and bed
    sensors.push_back({{"name", "extruder"}, {"enabled", true}, {"color", 0xFF4444}});
    sensors.push_back({{"name", "heater_bed"}, {"enabled", true}, {"color", 0x88C0D0}});

    // Check for chamber
    lv_subject_t* chamber_gate = lv_xml_get_subject(nullptr, "printer_has_chamber_sensor");
    if (chamber_gate && lv_subject_get_int(chamber_gate) != 0) {
        sensors.push_back({{"name", "chamber"}, {"enabled", false}, {"color", 0xA3BE8C}});
    }

    // Add discovered auxiliary sensors (disabled by default)
    int color_idx = 3; // Start after nozzle/bed/chamber colors
    auto& sensor_mgr = sensors::TemperatureSensorManager::instance();
    auto discovered = sensor_mgr.get_sensors_sorted();
    for (const auto& sensor : discovered) {
        if (!sensor.enabled) continue;
        uint32_t color_hex = 0;
        // Extract hex from palette color
        lv_color_t c = SERIES_COLORS[color_idx % PALETTE_SIZE];
        color_hex = (static_cast<uint32_t>(c.red) << 16)
                  | (static_cast<uint32_t>(c.green) << 8)
                  | static_cast<uint32_t>(c.blue);
        color_idx++;

        sensors.push_back({
            {"name", sensor.klipper_name},
            {"enabled", false},
            {"color", color_hex},
        });
    }

    config_["sensors"] = sensors;
    spdlog::debug("[TempGraphWidget] Built default config with {} sensors for '{}'",
                  sensors.size(), instance_id_);
}

// ============================================================================
// TempGraphConfigModal
// ============================================================================

TempGraphWidget::TempGraphConfigModal::TempGraphConfigModal(
    const nlohmann::json& config, SaveCallback on_save)
    : config_(config), on_save_(std::move(on_save)) {}

void TempGraphWidget::TempGraphConfigModal::on_show() {
    wire_ok_button("btn_primary");
    wire_cancel_button("btn_secondary");

    populate_sensor_list();

    spdlog::debug("[TempGraphConfigModal] Opened with {} sensor rows", rows_.size());
}

void TempGraphWidget::TempGraphConfigModal::on_ok() {
    // Collect current toggle/color states back into config
    nlohmann::json sensors = nlohmann::json::array();
    for (auto& row : rows_) {
        // Read current switch state
        if (row.sw) {
            row.enabled = lv_obj_has_state(row.sw, LV_STATE_CHECKED);
        }

        lv_color_t c = SERIES_COLORS[row.color_idx % PALETTE_SIZE];
        uint32_t color_hex = (static_cast<uint32_t>(c.red) << 16)
                           | (static_cast<uint32_t>(c.green) << 8)
                           | static_cast<uint32_t>(c.blue);

        sensors.push_back({
            {"name", row.name},
            {"enabled", row.enabled},
            {"color", color_hex},
        });
    }

    nlohmann::json new_config = config_;
    new_config["sensors"] = sensors;

    if (on_save_) {
        on_save_(new_config);
    }

    spdlog::info("[TempGraphConfigModal] Saved config with {} sensors", sensors.size());
    hide();
}

std::string TempGraphWidget::TempGraphConfigModal::sensor_display_name(
    const std::string& klipper_name) {
    if (klipper_name == "extruder") return "Nozzle";
    if (klipper_name == "heater_bed") return "Bed";
    if (klipper_name == "chamber") return "Chamber";

    // Strip common prefixes for auxiliary sensors
    std::string display = klipper_name;
    const char* prefixes[] = {"temperature_sensor ", "temperature_fan "};
    for (const char* prefix : prefixes) {
        if (display.find(prefix) == 0) {
            display = display.substr(strlen(prefix));
            break;
        }
    }

    // Capitalize first letter
    if (!display.empty()) {
        display[0] = static_cast<char>(toupper(static_cast<unsigned char>(display[0])));
    }

    // Replace underscores with spaces
    for (auto& ch : display) {
        if (ch == '_') ch = ' ';
    }

    return display;
}

void TempGraphWidget::TempGraphConfigModal::populate_sensor_list() {
    lv_obj_t* list = find_widget("sensor_list");
    if (!list) {
        spdlog::warn("[TempGraphConfigModal] sensor_list container not found");
        return;
    }

    rows_.clear();

    if (!config_.contains("sensors")) return;

    const auto& sensors = config_["sensors"];
    for (size_t i = 0; i < sensors.size(); ++i) {
        const auto& entry = sensors[i];
        if (!entry.contains("name")) continue;

        SensorRow row;
        row.name = entry["name"].get<std::string>();
        row.display = sensor_display_name(row.name);
        row.enabled = entry.value("enabled", true);

        // Find the matching color index from the palette
        if (entry.contains("color")) {
            uint32_t cfg_hex = entry["color"].get<uint32_t>();
            lv_color_t cfg_color = lv_color_hex(cfg_hex);
            row.color_idx = static_cast<int>(i) % PALETTE_SIZE; // default
            for (int ci = 0; ci < PALETTE_SIZE; ++ci) {
                if (SERIES_COLORS[ci].red == cfg_color.red &&
                    SERIES_COLORS[ci].green == cfg_color.green &&
                    SERIES_COLORS[ci].blue == cfg_color.blue) {
                    row.color_idx = ci;
                    break;
                }
            }
        } else {
            row.color_idx = static_cast<int>(i) % PALETTE_SIZE;
        }

        // Build the row container: [color swatch] [name label] [spacer] [switch]
        lv_obj_t* row_obj = lv_obj_create(list);
        lv_obj_set_size(row_obj, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row_obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(row_obj, 6, 0);
        lv_obj_set_style_pad_gap(row_obj, 12, 0);

        // Color swatch — small colored square, clickable to cycle
        lv_obj_t* swatch = lv_obj_create(row_obj);
        lv_obj_set_size(swatch, 24, 24);
        lv_obj_set_style_radius(swatch, 4, 0);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(swatch, SERIES_COLORS[row.color_idx % PALETTE_SIZE], 0);
        lv_obj_set_style_border_width(swatch, 0, 0);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        row.swatch = swatch;

        // Store row index in user data for the click callback
        lv_obj_set_user_data(swatch, reinterpret_cast<void*>(static_cast<uintptr_t>(rows_.size())));
        lv_obj_add_event_cb(swatch, color_swatch_clicked, LV_EVENT_CLICKED, this);

        // Sensor name label
        lv_obj_t* label = lv_label_create(row_obj);
        lv_label_set_text(label, row.display.c_str());
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);

        // Toggle switch
        lv_obj_t* sw = lv_switch_create(row_obj);
        lv_obj_set_size(sw, 44, 24);
        if (row.enabled) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        row.sw = sw;

        rows_.push_back(std::move(row));
    }
}

void TempGraphWidget::TempGraphConfigModal::color_swatch_clicked(lv_event_t* e) {
    auto* modal = static_cast<TempGraphConfigModal*>(lv_event_get_user_data(e));
    auto* swatch = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto idx = static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(swatch)));

    if (!modal || idx >= modal->rows_.size()) return;

    auto& row = modal->rows_[idx];
    row.color_idx = (row.color_idx + 1) % PALETTE_SIZE;
    lv_obj_set_style_bg_color(swatch, SERIES_COLORS[row.color_idx % PALETTE_SIZE], 0);

    spdlog::debug("[TempGraphConfigModal] Cycled '{}' to color index {}", row.name, row.color_idx);
}

// ============================================================================
// Click callback
// ============================================================================

void TempGraphWidget::on_temp_graph_widget_clicked(lv_event_t* e) {
    auto* self = panel_widget_from_event<TempGraphWidget>(e);
    if (!self || !self->parent_screen_) return;

    spdlog::debug("[TempGraphWidget] Clicked '{}', opening overlay", self->instance_id_);
    get_global_temp_graph_overlay().open(TempGraphOverlay::Mode::GraphOnly,
                                         self->parent_screen_);
}
