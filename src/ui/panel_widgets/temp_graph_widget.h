// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"
#include "ui_observer_guard.h"
#include "ui_temp_graph.h"

#include <memory>
#include <string>
#include <vector>

namespace helix {

/// Dashboard widget displaying a real-time temperature graph.
/// Sizes adaptively based on grid span, showing more features at larger sizes.
/// Click opens the full TempGraphOverlay in GraphOnly mode.
class TempGraphWidget : public PanelWidget {
  public:
    explicit TempGraphWidget(const std::string& instance_id);
    ~TempGraphWidget() override;

    // PanelWidget interface
    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    std::string get_component_name() const override;
    const char* id() const override { return instance_id_.c_str(); }
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    void on_activate() override;
    void on_deactivate() override;
    bool has_edit_configure() const override { return true; }
    bool on_edit_configure() override;
    bool supports_reuse() const override { return true; }

    /// Map grid size to appropriate feature flags
    static uint32_t features_for_size(int colspan, int rowspan);

    /// Static click callback (XML-registered)
    static void on_temp_graph_widget_clicked(lv_event_t* e);

  private:
    /// Per-series metadata for the widget graph
    struct SeriesInfo {
        std::string name;        ///< Klipper heater/sensor key (e.g., "extruder", "heater_bed")
        int series_id = -1;      ///< Graph series ID from ui_temp_graph_add_series()
        lv_color_t color{};      ///< Series line color
        ObserverGuard temp_obs;  ///< Temperature observer
        ObserverGuard target_obs; ///< Target temperature observer
        SubjectLifetime lifetime; ///< Lifetime token for dynamic subjects
        bool is_dynamic = false;  ///< Whether this uses a dynamic subject
        bool has_target = false;  ///< Whether this heater has a controllable target
    };

    void setup_series();
    void setup_observers();
    void apply_auto_range();
    void build_default_config();
    void backfill_history();

    std::string instance_id_;
    nlohmann::json config_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    ui_temp_graph_t* graph_ = nullptr;

    std::vector<SeriesInfo> series_;
    ObserverGuard connection_observer_;

    std::shared_ptr<bool> alive_;
    uint32_t generation_ = 0;
    bool paused_ = false;

    int current_colspan_ = 2;
    int current_rowspan_ = 2;

    // Color palette (matches TempGraphOverlay)
    static constexpr int PALETTE_SIZE = 8;
    static const lv_color_t SERIES_COLORS[PALETTE_SIZE];
};

} // namespace helix
