// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "overlay_base.h"
#include "ui_heater_config.h"
#include "ui_observer_guard.h"
#include "ui_temp_graph.h"

#include <string>
#include <vector>

// Forward declarations
namespace helix {
class PrinterState;
} // namespace helix
class MoonrakerAPI;
class TempControlPanel;
class TemperatureHistoryManager;

namespace helix::sensors {
class TemperatureSensorManager;
} // namespace helix::sensors

/**
 * @brief Unified temperature graph overlay
 *
 * Replaces the 3 separate nozzle/bed/chamber overlays with a single overlay
 * that graphs ALL temperature sensors with toggle chips and optional controls.
 *
 * ## Modes
 * - GraphOnly: Full-height graph, no heater controls (opened from mini graph tap)
 * - Nozzle: Graph + nozzle preset controls (opened from nozzle temp click)
 * - Bed: Graph + bed preset controls
 * - Chamber: Graph + chamber preset controls (hidden if sensor-only)
 */
class TempGraphOverlay : public OverlayBase {
  public:
    enum class Mode { GraphOnly, Nozzle, Bed, Chamber };

    TempGraphOverlay();
    ~TempGraphOverlay() override;

    // OverlayBase interface
    void init_subjects() override;
    void register_callbacks() override;
    lv_obj_t* create(lv_obj_t* parent) override;
    const char* get_name() const override {
        return "Temperature Graph";
    }
    void on_activate() override;
    void on_deactivate() override;
    void cleanup() override;

    /**
     * @brief Open the overlay in a specific mode
     *
     * Sets the mode and pushes the overlay via NavigationManager.
     * Must be called after init_subjects/create on first use.
     *
     * @param mode The display mode (determines which controls are shown)
     * @param parent_screen Parent screen for lazy creation
     */
    void open(Mode mode, lv_obj_t* parent_screen);

    // Static event callbacks (for XML registration)
    static void on_temp_graph_preset_clicked(lv_event_t* e);
    static void on_temp_graph_custom_clicked(lv_event_t* e);

  private:
    /**
     * @brief Per-series metadata for the unified graph
     */
    struct SeriesInfo {
        std::string display_name;  ///< UI label (e.g., "Nozzle", "Bed", "MCU")
        std::string heater_name;   ///< History manager key (e.g., "extruder", "heater_bed")
        std::string klipper_name;  ///< Full Klipper object name for API calls
        int series_id = -1;        ///< Graph series ID
        lv_color_t color{};        ///< Series line color
        lv_obj_t* chip = nullptr;  ///< Toggle chip widget
        bool visible = true;       ///< Current visibility state
        bool has_target = false;   ///< Whether this heater has a controllable target
        bool is_dynamic = false;   ///< Dynamic sensor (needs SubjectLifetime)
        ObserverGuard temp_observer;
        ObserverGuard target_observer;
        SubjectLifetime lifetime;
    };

    // Series management
    void discover_series();
    void apply_default_visibility();
    void create_chips();
    void setup_observers();
    void teardown_observers();
    void replay_history();

    // Graph updates
    void on_series_temp_changed(size_t series_idx, int temp_centi);
    void on_series_target_changed(size_t series_idx, int target_centi);
    void update_y_axis_range();

    // Chip interaction
    void toggle_series_visibility(size_t series_idx);
    void update_chip_style(size_t series_idx);
    static void on_chip_clicked(lv_event_t* e);

    // Control strip
    void configure_control_strip();

    // Keypad callback
    static void keypad_value_cb(float value, void* user_data);

    // Extruder selector
    void rebuild_extruder_selector();
    static void on_extruder_selected(lv_event_t* e);

    // Preset helpers
    struct PresetData {
        TempGraphOverlay* overlay;
        int preset_value;
    };
    static constexpr int MAX_PRESETS = 4;
    std::array<PresetData, MAX_PRESETS> preset_data_{};

    // State
    Mode mode_ = Mode::GraphOnly;
    ui_temp_graph_t* graph_ = nullptr;
    lv_obj_t* chip_row_ = nullptr;
    lv_obj_t* graph_container_ = nullptr;
    lv_obj_t* graph_outer_ = nullptr;
    lv_obj_t* nozzle_strip_ = nullptr;
    lv_obj_t* bed_strip_ = nullptr;
    lv_obj_t* chamber_strip_ = nullptr;
    lv_obj_t* extruder_selector_row_ = nullptr;
    std::vector<SeriesInfo> series_;

    // Y-axis auto-scaling state
    float y_axis_max_ = 100.0f;
    static constexpr float Y_AXIS_MIN = 0.0f;
    static constexpr float Y_AXIS_STEP = 50.0f;
    static constexpr float Y_AXIS_FLOOR = 100.0f;
    static constexpr float Y_AXIS_CEILING = 400.0f;
    static constexpr float Y_EXPAND_THRESHOLD = 0.85f;
    static constexpr float Y_SHRINK_THRESHOLD = 0.55f;

    // Dependencies (resolved on open)
    helix::PrinterState* printer_state_ = nullptr;
    MoonrakerAPI* api_ = nullptr;
    TempControlPanel* temp_control_panel_ = nullptr;

    // Active extruder name (for nozzle mode)
    std::string active_extruder_name_ = "extruder";

    // Subject management
    SubjectManager subjects_;



    // Cached panel for lazy creation
    lv_obj_t* cached_overlay_ = nullptr;

    // Color palette for series
    static constexpr int PALETTE_SIZE = 8;
    static const lv_color_t SERIES_COLORS[PALETTE_SIZE];
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers cleanup with StaticPanelRegistry.
 */
TempGraphOverlay& get_global_temp_graph_overlay();
