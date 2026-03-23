// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_panel_base.h"

#include "grid_edit_mode.h"
#include "panel_widget.h"
#include "subject_managed_panel.h"

#include <memory>
#include <string>
#include <vector>

/**
 * @brief Home panel - Main dashboard showing printer status and quick actions
 *
 * Pure grid container: all visible elements (printer image, tips, print status,
 * temperature, network, LED, power, etc.) are placed as PanelWidgets by
 * PanelWidgetManager. Widget-specific behavior lives in PanelWidget subclasses
 * which self-register their own XML callbacks, observers, and lifecycle.
 */

class HomePanel : public PanelBase {
  public:
    HomePanel(helix::PrinterState& printer_state, MoonrakerAPI* api);
    ~HomePanel() override;

    void init_subjects() override;
    void deinit_subjects();
    void setup(lv_obj_t* panel, lv_obj_t* parent_screen) override;
    void on_activate() override;
    void on_deactivate() override;
    const char* get_name() const override {
        return "Home Panel";
    }
    const char* get_xml_component_name() const override {
        return "home_panel";
    }

    /// Rebuild the widget list from current PanelWidgetConfig.
    /// @param force  If false (gate observer path), skip rebuild when the
    ///               visible widget ID list hasn't changed. If true (config
    ///               change, grid edit mode), always rebuild.
    void populate_widgets(bool force = true);

    /// Apply printer-level config (delegates to PrinterImageWidget)
    void apply_printer_config();

    /// Delegate printer image refresh to PrinterImageWidget if active
    void refresh_printer_image();

    /// Trigger a deferred runout check (delegates to PrintStatusWidget)
    void trigger_idle_runout_check();

    /// Exit grid edit mode (called by navbar done button)
    void exit_grid_edit_mode();

    /// Open widget catalog overlay (called by navbar + button)
    void open_widget_catalog();

  private:
    SubjectManager subjects_;
    bool populating_widgets_ = false; // Reentrancy guard for populate_widgets()
    bool panel_active_ = false;        // Whether on_activate() has been called

    // Cached image path for skipping redundant refresh_printer_image() calls
    std::string last_printer_image_path_;

    // Grid edit mode state machine (long-press to rearrange widgets)
    helix::GridEditMode grid_edit_mode_;

    // Image change observer (triggers printer image refresh)
    ObserverGuard image_changed_observer_;

    // Multi-page carousel state
    lv_obj_t* carousel_ = nullptr;
    lv_obj_t* carousel_host_ = nullptr;
    lv_obj_t* add_page_tile_ = nullptr;
    lv_obj_t* arrow_left_ = nullptr;
    lv_obj_t* arrow_right_ = nullptr;
    std::vector<std::vector<std::unique_ptr<helix::PanelWidget>>> page_widgets_;
    std::vector<lv_obj_t*> page_containers_;
    std::vector<std::vector<std::string>> page_visible_ids_;
    int active_page_index_ = 0;
    lv_subject_t page_subject_{};
    ObserverGuard page_observer_;

    static constexpr int kMaxPages = 8;

    void build_carousel();
    void rebuild_carousel();
    void on_page_changed(int new_page);
    void on_add_page_clicked();
    void update_arrow_visibility(int page);
    void populate_page(int page_index, bool force);

    // Grid and widget lifecycle
    void setup_widget_gate_observers();

    // Panel-level click handlers (not widget-delegated)
    void handle_printer_status_clicked();
    void handle_ams_clicked();

    // Panel-level static callbacks
    static void printer_status_clicked_cb(lv_event_t* e);
    static void ams_clicked_cb(lv_event_t* e);
    static void on_home_grid_long_press(lv_event_t* e);
    static void on_home_grid_clicked(lv_event_t* e);
    static void on_home_grid_pressing(lv_event_t* e);
    static void on_home_grid_released(lv_event_t* e);
};

// Global instance accessor (needed by main.cpp)
HomePanel& get_global_home_panel();
