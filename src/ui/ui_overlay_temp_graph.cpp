// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_overlay_temp_graph.h"

#include "app_globals.h"
#include "moonraker_api.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "printer_temperature_state.h"
#include "static_panel_registry.h"
#include "temperature_sensor_manager.h"
#include "temperature_sensor_types.h"
#include "theme_manager.h"
#include "ui_error_reporting.h"
#include "ui_heater_config.h"
#include "ui_component_keypad.h"
#include "ui_nav_manager.h"
#include "temperature_service.h"
#include "ui_utils.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Map overlay Mode to HeaterType for temperature control.
 * Returns false for GraphOnly mode (no heater controls).
 */
static bool mode_to_heater_type(TempGraphOverlay::Mode mode, helix::HeaterType& out) {
    switch (mode) {
    case TempGraphOverlay::Mode::Nozzle:
        out = helix::HeaterType::Nozzle;
        return true;
    case TempGraphOverlay::Mode::Bed:
        out = helix::HeaterType::Bed;
        return true;
    case TempGraphOverlay::Mode::Chamber:
        out = helix::HeaterType::Chamber;
        return true;
    default:
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Color palette: nozzle=red, bed=cyan, chamber=green, then 5 more
// ─────────────────────────────────────────────────────────────────────────────

const lv_color_t TempGraphOverlay::SERIES_COLORS[PALETTE_SIZE] = {
    lv_color_hex(0xFF4444), // Nozzle (red)
    lv_color_hex(0x88C0D0), // Bed (cyan / nord8)
    lv_color_hex(0xA3BE8C), // Chamber (green / nord14)
    lv_color_hex(0xEBCB8B), // Yellow / nord13
    lv_color_hex(0xB48EAD), // Purple / nord15
    lv_color_hex(0xD08770), // Orange / nord12
    lv_color_hex(0x5E81AC), // Blue / nord10
    lv_color_hex(0xBF616A), // Dark red / nord11
};

// ─────────────────────────────────────────────────────────────────────────────
// Global instance
// ─────────────────────────────────────────────────────────────────────────────

static std::unique_ptr<TempGraphOverlay> g_temp_graph_overlay;

TempGraphOverlay& get_global_temp_graph_overlay() {
    if (!g_temp_graph_overlay) {
        g_temp_graph_overlay = std::make_unique<TempGraphOverlay>();
        StaticPanelRegistry::instance().register_destroy("TempGraphOverlay",
                                                          []() { g_temp_graph_overlay.reset(); });
    }
    return *g_temp_graph_overlay;
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction / Destruction
// ─────────────────────────────────────────────────────────────────────────────

TempGraphOverlay::TempGraphOverlay() = default;

TempGraphOverlay::~TempGraphOverlay() {
    controller_.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// OverlayBase interface
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::init_subjects() {
    init_subjects_guarded([]() {});
}

void TempGraphOverlay::register_callbacks() {
    // Callbacks registered in xml_registration.cpp at startup (before XML parsing)
}

lv_obj_t* TempGraphOverlay::create(lv_obj_t* parent) {
    if (!create_overlay_from_xml(parent, "temp_graph_overlay")) {
        return nullptr;
    }

    chip_row_ = lv_obj_find_by_name(overlay_root_, "chip_row");
    graph_container_ = lv_obj_find_by_name(overlay_root_, "graph_container");
    nozzle_strip_ = lv_obj_find_by_name(overlay_root_, "nozzle_control_strip");
    bed_strip_ = lv_obj_find_by_name(overlay_root_, "bed_control_strip");
    chamber_strip_ = lv_obj_find_by_name(overlay_root_, "chamber_control_strip");
    extruder_selector_row_ = lv_obj_find_by_name(overlay_root_, "extruder_selector_row");
    graph_outer_ = lv_obj_find_by_name(overlay_root_, "graph_outer_container");

    return overlay_root_;
}

void TempGraphOverlay::on_activate() {
    OverlayBase::on_activate();

    // Resolve dependencies
    printer_state_ = &get_printer_state();
    api_ = get_moonraker_api();
    temp_control_panel_ =
        helix::PanelWidgetManager::instance().shared_resource<TemperatureService>();

    // Discover series metadata (populates series_ with display info)
    discover_series();

    // Build TempGraphSeriesSpec vector from discovered series
    std::vector<helix::TempGraphSeriesSpec> specs;
    specs.reserve(series_.size());
    for (const auto& s : series_) {
        specs.push_back({s.klipper_name, s.color, s.has_target});
    }

    // Create controller (handles graph creation, observers, history, auto-range)
    if (graph_container_) {
        helix::TempGraphControllerConfig cfg;
        cfg.axis_size = "sm";
        cfg.series = std::move(specs);
        controller_ = std::make_unique<helix::TempGraphController>(graph_container_, cfg);

        // Size chart to fill container (default height is LV_DPI_DEF*2)
        if (controller_->is_valid()) {
            lv_obj_t* chart = ui_temp_graph_get_chart(controller_->graph());
            if (chart) {
                lv_obj_set_size(chart, lv_pct(100), lv_pct(100));
                lv_color_t card_bg = theme_manager_get_color("card_bg");
                lv_obj_set_style_bg_color(chart, card_bg, LV_PART_MAIN);
                controller_->graph()->cached_graph_bg = card_bg;
            }
        }
    }

    // Map series IDs back from controller
    if (controller_ && controller_->is_valid()) {
        for (auto& s : series_) {
            s.series_id = controller_->series_id_for(s.klipper_name);
        }
    }

    // Apply mode-specific visibility and chips (every activation)
    apply_default_visibility();
    create_chips();
    configure_control_strip();

    spdlog::debug("[TempGraphOverlay] Activated with {} series, mode={}",
                  series_.size(), static_cast<int>(mode_));
}

void TempGraphOverlay::on_deactivate() {
    OverlayBase::on_deactivate();

    // Destroy controller (tears down observers, destroys graph)
    controller_.reset();

    // Clear series
    series_.clear();

    // Clear chip row
    if (chip_row_) {
        lv_obj_clean(chip_row_);
    }

    spdlog::debug("[TempGraphOverlay] Deactivated");
}

void TempGraphOverlay::cleanup() {
    controller_.reset();
    series_.clear();
    OverlayBase::cleanup();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::open(Mode mode, lv_obj_t* parent_screen) {
    mode_ = mode;

    // Lazy create
    if (!cached_overlay_ && parent_screen) {
        if (!are_subjects_initialized()) {
            init_subjects();
        }

        cached_overlay_ = create(parent_screen);
        if (!cached_overlay_) {
            spdlog::error("[TempGraphOverlay] Failed to create overlay from XML");
            NOTIFY_ERROR(lv_tr("Failed to open temperature graph"));
            return;
        }

        NavigationManager::instance().register_overlay_instance(cached_overlay_, this, true);
        spdlog::info("[TempGraphOverlay] Overlay created");
    }

    if (cached_overlay_) {
        NavigationManager::instance().push_overlay(cached_overlay_);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Series discovery
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::discover_series() {
    series_.clear();
    int color_idx = 0;

    if (!printer_state_) return;

    const auto& temp_state = printer_state_->temperature_state();

    // 1. Nozzle(s)
    const auto& extruders = temp_state.extruders();
    if (extruders.empty()) {
        // Fallback: always add at least one nozzle
        SeriesInfo s;
        s.display_name = "Nozzle";
        s.heater_name = "extruder";
        s.klipper_name = "extruder";
        s.color = SERIES_COLORS[color_idx++ % PALETTE_SIZE];
        s.has_target = true;
        s.is_dynamic = false;
        series_.push_back(std::move(s));
    } else {
        // Sort extruders by name for consistent ordering
        std::vector<const helix::ExtruderInfo*> sorted_extruders;
        for (const auto& [name, info] : extruders) {
            sorted_extruders.push_back(&info);
        }
        std::sort(sorted_extruders.begin(), sorted_extruders.end(),
                  [](const auto* a, const auto* b) { return a->name < b->name; });

        for (const auto* ext : sorted_extruders) {
            SeriesInfo s;
            s.display_name = ext->display_name;
            s.heater_name = ext->name;
            s.klipper_name = ext->name;
            s.color = SERIES_COLORS[color_idx++ % PALETTE_SIZE];
            s.has_target = true;
            s.is_dynamic = (extruders.size() > 1); // Dynamic if multi-extruder
            series_.push_back(std::move(s));
        }
    }

    // 2. Bed
    {
        SeriesInfo s;
        s.display_name = "Bed";
        s.heater_name = "heater_bed";
        s.klipper_name = "heater_bed";
        s.color = SERIES_COLORS[color_idx++ % PALETTE_SIZE];
        s.has_target = true;
        s.is_dynamic = false;
        series_.push_back(std::move(s));
    }

    // 3. Chamber (if present)
    {
        lv_subject_t* chamber_gate = lv_xml_get_subject(nullptr, "printer_has_chamber_sensor");
        if (chamber_gate && lv_subject_get_int(chamber_gate) != 0) {
            SeriesInfo s;
            s.display_name = "Chamber";
            s.heater_name = "chamber"; // TemperatureHistoryManager might not track this
            s.klipper_name = temp_state.chamber_heater_name();
            s.color = SERIES_COLORS[color_idx++ % PALETTE_SIZE];
            s.has_target = !s.klipper_name.empty();
            s.is_dynamic = false;
            series_.push_back(std::move(s));
        }
    }

    // 4. Custom sensors from TemperatureSensorManager
    auto& sensor_mgr = helix::sensors::TemperatureSensorManager::instance();
    auto sensors = sensor_mgr.get_sensors_sorted();
    for (const auto& sensor : sensors) {
        // Skip sensors with chamber role (already handled above)
        if (sensor.role == helix::sensors::TemperatureSensorRole::CHAMBER) continue;
        // Skip disabled sensors
        if (!sensor.enabled) continue;

        SeriesInfo s;
        s.display_name = sensor.display_name;
        s.heater_name = sensor.klipper_name; // May not have history
        s.klipper_name = sensor.klipper_name;
        s.color = SERIES_COLORS[color_idx++ % PALETTE_SIZE];
        s.has_target = (sensor.type == helix::sensors::TemperatureSensorType::TEMPERATURE_FAN);
        s.is_dynamic = true;
        series_.push_back(std::move(s));
    }

    spdlog::debug("[TempGraphOverlay] Discovered {} series", series_.size());
}

void TempGraphOverlay::apply_default_visibility() {
    // In heater-specific modes, only the primary sensor is visible by default.
    // In GraphOnly mode, all sensors are visible.
    for (auto& s : series_) {
        switch (mode_) {
        case Mode::GraphOnly:
            s.visible = true;
            break;
        case Mode::Nozzle:
            // Match any extruder (extruder, extruder1, etc.)
            s.visible = (s.heater_name.find("extruder") == 0);
            break;
        case Mode::Bed:
            s.visible = (s.heater_name == "heater_bed");
            break;
        case Mode::Chamber:
            s.visible = (s.heater_name == "chamber");
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Chip creation
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::create_chips() {
    if (!chip_row_) return;

    lv_obj_clean(chip_row_);

    for (size_t i = 0; i < series_.size(); ++i) {
        auto& s = series_[i];

        // Create a chip button: colored dot + label
        lv_obj_t* chip = lv_obj_create(chip_row_);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(chip, theme_manager_get_spacing("space_sm"), 0);
        lv_obj_set_style_pad_ver(chip, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(chip, theme_manager_get_color("elevated_bg"), 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, theme_manager_get_color("border"), 0);
        lv_obj_set_style_shadow_width(chip, 4, 0);
        lv_obj_set_style_shadow_opa(chip, LV_OPA_20, 0);
        lv_obj_set_style_shadow_offset_y(chip, 2, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(chip, theme_manager_get_spacing("space_xxs"), 0);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);

        // Color dot
        lv_obj_t* dot = lv_obj_create(chip);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, s.color, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_remove_flag(dot, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_add_flag(dot, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Label — strip redundant " Temperature" suffix for chip brevity
        std::string chip_label = s.display_name;
        const std::string suffix = " Temperature";
        if (chip_label.size() > suffix.size() &&
            chip_label.compare(chip_label.size() - suffix.size(), suffix.size(), suffix) == 0) {
            chip_label.erase(chip_label.size() - suffix.size());
        }
        lv_obj_t* label = lv_label_create(chip);
        lv_label_set_text(label, chip_label.c_str());
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_small"), 0);
        lv_obj_set_style_text_color(label, theme_manager_get_color("text_primary"), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Store index as user data for click handler
        lv_obj_set_user_data(chip, reinterpret_cast<void*>(i));
        // Exception: programmatic widget, can't use XML event_cb
        lv_obj_add_event_cb(chip, on_chip_clicked, LV_EVENT_CLICKED, this);

        s.chip = chip;

        // Apply initial visibility state (set by apply_default_visibility)
        update_chip_style(i);
        if (controller_ && controller_->is_valid() && s.series_id >= 0) {
            ui_temp_graph_show_series(controller_->graph(), s.series_id, s.visible);
        }
    }
}

void TempGraphOverlay::on_chip_clicked(lv_event_t* e) {
    auto* self = static_cast<TempGraphOverlay*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!self || !target) return;

    auto idx = reinterpret_cast<size_t>(lv_obj_get_user_data(target));
    if (idx < self->series_.size()) {
        self->toggle_series_visibility(idx);
    }
}

void TempGraphOverlay::toggle_series_visibility(size_t series_idx) {
    if (series_idx >= series_.size()) return;
    auto& s = series_[series_idx];

    s.visible = !s.visible;
    if (controller_ && controller_->is_valid() && s.series_id >= 0) {
        auto* graph = controller_->graph();
        ui_temp_graph_show_series(graph, s.series_id, s.visible);
        if (s.has_target) {
            // Only show target line if series is visible AND target is non-zero
            bool show = s.visible && graph->series_meta[s.series_id].target_temp > 0.0f;
            ui_temp_graph_show_target(graph, s.series_id, show);
        }
    }
    update_chip_style(series_idx);

    spdlog::debug("[TempGraphOverlay] {} series '{}' (idx={})",
                  s.visible ? "Showed" : "Hid", s.display_name, series_idx);
}

void TempGraphOverlay::update_chip_style(size_t series_idx) {
    if (series_idx >= series_.size()) return;
    auto& s = series_[series_idx];
    if (!s.chip) return;

    lv_opa_t opa = s.visible ? LV_OPA_COVER : LV_OPA_40;
    lv_obj_set_style_opa(s.chip, opa, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Control strip
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::configure_control_strip() {
    // Hide all strips first
    if (nozzle_strip_) lv_obj_add_flag(nozzle_strip_, LV_OBJ_FLAG_HIDDEN);
    if (bed_strip_) lv_obj_add_flag(bed_strip_, LV_OBJ_FLAG_HIDDEN);
    if (chamber_strip_) lv_obj_add_flag(chamber_strip_, LV_OBJ_FLAG_HIDDEN);

    if (mode_ == Mode::GraphOnly) {
        // Graph takes full width — no right column
        if (graph_outer_) lv_obj_set_width(graph_outer_, lv_pct(100));
        return;
    }

    // Graph gets 66% width with control column visible
    if (graph_outer_) lv_obj_set_width(graph_outer_, lv_pct(66));

    // Determine which strip to show and heater type
    helix::HeaterType heater_type;
    if (!mode_to_heater_type(mode_, heater_type)) return;

    lv_obj_t* active_strip = nullptr;
    switch (mode_) {
    case Mode::Nozzle:  active_strip = nozzle_strip_;  break;
    case Mode::Bed:     active_strip = bed_strip_;      break;
    case Mode::Chamber: active_strip = chamber_strip_;  break;
    default: break;
    }

    if (!active_strip) return;
    lv_obj_remove_flag(active_strip, LV_OBJ_FLAG_HIDDEN);

    // Get preset config from TemperatureService
    if (!temp_control_panel_) return;
    auto& heater = temp_control_panel_->heater(heater_type);

    // Configure preset values for the callback
    int preset_values[] = {heater.config.presets.off, heater.config.presets.pla,
                           heater.config.presets.petg, heater.config.presets.abs};

    // Store preset values indexed by name suffix for lookup in callback
    // (cannot use lv_obj_set_user_data — ui_button owns that slot, L069)
    for (int i = 0; i < MAX_PRESETS; ++i) {
        preset_data_[i] = {this, preset_values[i]};
    }

    // Extruder selector: show only in nozzle mode with multiple extruders
    if (extruder_selector_row_) {
        auto& temp_state = printer_state_->temperature_state();
        if (mode_ == Mode::Nozzle && temp_state.extruder_count() > 1) {
            lv_obj_remove_flag(extruder_selector_row_, LV_OBJ_FLAG_HIDDEN);
            rebuild_extruder_selector();
        } else {
            lv_obj_add_flag(extruder_selector_row_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Preset / Custom callbacks
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::on_temp_graph_preset_clicked(lv_event_t* e) {
    auto* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!btn) return;

    // Derive preset index from button name (L069: ui_button owns user_data)
    const char* name = lv_obj_get_name(btn);
    if (!name) return;

    auto* self = &get_global_temp_graph_overlay();
    if (!self->temp_control_panel_) return;

    // Map button name suffix to preset index
    int preset_idx = -1;
    std::string name_str(name);
    if (name_str.find("preset_off") != std::string::npos) preset_idx = 0;
    else if (name_str.find("preset_1") != std::string::npos) preset_idx = 1;
    else if (name_str.find("preset_2") != std::string::npos) preset_idx = 2;
    else if (name_str.find("preset_3") != std::string::npos) preset_idx = 3;

    if (preset_idx < 0 || preset_idx >= MAX_PRESETS) return;
    auto& data = self->preset_data_[preset_idx];

    helix::HeaterType type;
    if (!mode_to_heater_type(self->mode_, type)) return;

    spdlog::debug("[TempGraphOverlay] Preset clicked: {}°C for heater {}",
                  data.preset_value, static_cast<int>(type));

    // Update local state
    self->temp_control_panel_->set_heater(type, self->temp_control_panel_->heater(type).current,
                                           data.preset_value * 10);

    // Send the temperature command
    if (self->api_) {
        auto& heater = self->temp_control_panel_->heater(type);
        const std::string& klipper_name =
            (type == helix::HeaterType::Nozzle) ? self->active_extruder_name_
                                                 : heater.klipper_name;
        self->api_->set_temperature(
            klipper_name, static_cast<double>(data.preset_value),
            []() {},
            [](const MoonrakerError& error) {
                NOTIFY_ERROR(lv_tr("Failed to set temperature: {}"), error.user_message());
            });
    }
}

void TempGraphOverlay::on_temp_graph_custom_clicked(lv_event_t* e) {
    (void)e;
    auto& overlay = get_global_temp_graph_overlay();
    if (!overlay.temp_control_panel_) return;

    helix::HeaterType type;
    if (!mode_to_heater_type(overlay.mode_, type)) return;

    auto& heater = overlay.temp_control_panel_->heater(type);

    // Store context for keypad callback (static because keypad outlives this scope)
    // lifetime token protects against overlay destruction while keypad is open
    static struct KeypadCtxStatic {
        TempGraphOverlay* overlay = nullptr;
        helix::HeaterType type{};
        std::optional<helix::LifetimeToken> token;
    } s_keypad_ctx;
    s_keypad_ctx.overlay = &overlay;
    s_keypad_ctx.type = type;
    s_keypad_ctx.token = overlay.lifetime_.token();

    ui_keypad_config_t keypad_config = {
        .initial_value = static_cast<float>(heater.target / 10),
        .min_value = heater.config.keypad_range.min,
        .max_value = heater.config.keypad_range.max,
        .title_label = heater.config.title,
        .unit_label = "°C",
        .allow_decimal = false,
        .allow_negative = false,
        .callback = keypad_value_cb,
        .user_data = &s_keypad_ctx,
    };

    ui_keypad_show(&keypad_config);
}

void TempGraphOverlay::keypad_value_cb(float value, void* user_data) {
    struct KeypadCtx {
        TempGraphOverlay* overlay;
        helix::HeaterType type;
        std::optional<helix::LifetimeToken> token;
    };
    auto* ctx = static_cast<KeypadCtx*>(user_data);
    if (!ctx || !ctx->overlay || !ctx->token || ctx->token->expired() || !ctx->overlay->api_) return;

    int temp = static_cast<int>(value);
    auto& heater = ctx->overlay->temp_control_panel_->heater(ctx->type);
    const std::string& klipper_name =
        (ctx->type == helix::HeaterType::Nozzle) ? ctx->overlay->active_extruder_name_
                                                   : heater.klipper_name;

    spdlog::debug("[TempGraphOverlay] Custom temperature: {}°C for {}", temp, klipper_name);

    ctx->overlay->api_->set_temperature(
        klipper_name, static_cast<double>(temp),
        []() {},
        [](const MoonrakerError& error) {
            NOTIFY_ERROR(lv_tr("Failed to set temperature: {}"), error.user_message());
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// Extruder selector
// ─────────────────────────────────────────────────────────────────────────────

void TempGraphOverlay::rebuild_extruder_selector() {
    if (!extruder_selector_row_ || !printer_state_) return;

    lv_obj_clean(extruder_selector_row_);

    auto& temp_state = printer_state_->temperature_state();
    const auto& extruders = temp_state.extruders();

    // Sort for consistent ordering
    std::vector<const helix::ExtruderInfo*> sorted;
    for (const auto& [name, info] : extruders) {
        sorted.push_back(&info);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto* a, const auto* b) { return a->name < b->name; });

    for (const auto* ext : sorted) {
        lv_obj_t* btn = lv_obj_create(extruder_selector_row_);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(btn, theme_manager_get_spacing("space_xs"), 0);
        lv_obj_set_style_radius(btn, theme_manager_get_spacing("space_xs"), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        bool is_active = (ext->name == active_extruder_name_);
        lv_obj_set_style_bg_color(btn, is_active ? theme_manager_get_color("primary")
                                                  : theme_manager_get_color("card_bg"), 0);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, ext->display_name.c_str());
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_small"), 0);
        lv_obj_set_style_text_color(label, is_active ? theme_manager_get_color("on_primary")
                                                      : theme_manager_get_color("text_primary"), 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);

        // Store name as obj name for lookup in callback
        lv_obj_set_name(btn, ext->name.c_str());
        // Exception: programmatic widget, can't use XML event_cb
        lv_obj_add_event_cb(btn, on_extruder_selected, LV_EVENT_CLICKED, this);
    }
}

void TempGraphOverlay::on_extruder_selected(lv_event_t* e) {
    auto* self = static_cast<TempGraphOverlay*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!self || !target) return;

    const char* name = lv_obj_get_name(target);
    if (!name) return;

    self->active_extruder_name_ = name;
    self->printer_state_->set_active_extruder(name);
    self->rebuild_extruder_selector();

    spdlog::debug("[TempGraphOverlay] Selected extruder: {}", name);
}
