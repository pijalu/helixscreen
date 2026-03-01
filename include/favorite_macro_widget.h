// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "panel_widget.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

class MoonrakerAPI;

namespace helix {

/// Parsed macro parameter with optional default value
struct MacroParam {
    std::string name;          ///< Parameter name (uppercase, e.g., "EXTRUDER_TEMP")
    std::string default_value; ///< Default value from |default(VALUE), empty if none
};

/// Parse macro parameters from a Klipper gcode_macro template.
/// Detects params.NAME, params['NAME'], params["NAME"] references and
/// extracts |default(VALUE) when present. Deduplicates by name.
[[nodiscard]] std::vector<MacroParam> parse_macro_params(const std::string& gcode_template);

/// Home panel widget for one-tap macro execution.
/// Two instances registered: favorite_macro_1 and favorite_macro_2.
/// Tap executes assigned macro; configure button opens macro picker.
/// When unconfigured, tap also opens picker.
class FavoriteMacroWidget : public PanelWidget {
  public:
    /// @param widget_id "favorite_macro_1" or "favorite_macro_2"
    explicit FavoriteMacroWidget(const std::string& widget_id);
    ~FavoriteMacroWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool has_edit_configure() const override { return true; }
    bool on_edit_configure() override;
    const char* id() const override {
        return widget_id_.c_str();
    }

    /// Event handlers routed from static callbacks
    void handle_clicked();

    // Static event callbacks (XML-registered)
    static void clicked_1_cb(lv_event_t* e);
    static void clicked_2_cb(lv_event_t* e);
    static void picker_backdrop_cb(lv_event_t* e);

  private:
    std::string widget_id_; ///< "favorite_macro_1" or "favorite_macro_2"

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* icon_label_ = nullptr;
    lv_obj_t* name_label_ = nullptr;

    std::string macro_name_;                ///< Assigned macro (e.g., "CLEAN_NOZZLE")
    std::vector<MacroParam> cached_params_; ///< Cached parsed parameters
    bool params_cached_ = false;            ///< Whether params have been fetched

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(false);

    // Picker context menu
    lv_obj_t* picker_backdrop_ = nullptr;

    MoonrakerAPI* get_api() const;

    void update_display();
    void load_config();
    void save_config();

    void fetch_and_execute();
    void execute_with_params(const std::map<std::string, std::string>& params);
    void show_macro_picker();
    void dismiss_macro_picker();
    void select_macro(const std::string& name);

    // Static active instance for picker event routing
    static FavoriteMacroWidget* s_active_picker_;
};

} // namespace helix
