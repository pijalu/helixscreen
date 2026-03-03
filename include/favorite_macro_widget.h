// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "macro_param_cache.h"
#include "macro_param_modal.h"
#include "ui_observer_guard.h"

#include "panel_widget.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

class MoonrakerAPI;

namespace helix {

static constexpr int kMaxFavoriteMacroSlots = 5;

/// Home panel widget for one-tap macro execution.
/// Up to kMaxFavoriteMacroSlots instances registered: favorite_macro_1 through _5.
/// All share a single XML component and catalog entry via catalog_group.
/// Tap executes assigned macro; configure button opens macro picker.
/// When unconfigured, tap also opens picker.
class FavoriteMacroWidget : public PanelWidget {
  public:
    /// @param widget_id "favorite_macro_1" through "favorite_macro_5"
    explicit FavoriteMacroWidget(const std::string& widget_id);
    ~FavoriteMacroWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool has_edit_configure() const override { return true; }
    bool on_edit_configure() override;
    std::string get_component_name() const override {
        return "panel_widget_favorite_macro";
    }
    const char* id() const override {
        return widget_id_.c_str();
    }

    /// Event handlers routed from static callbacks
    void handle_clicked();

    // Static event callbacks (XML-registered)
    static void clicked_cb(lv_event_t* e);
    static void picker_backdrop_cb(lv_event_t* e);
    static void picker_done_cb(lv_event_t* e);

  private:
    std::string widget_id_; ///< "favorite_macro_1" through "favorite_macro_5"

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* icon_label_ = nullptr;
    lv_obj_t* name_label_ = nullptr;

    std::string macro_name_;                ///< Assigned macro (e.g., "CLEAN_NOZZLE")
    std::string icon_name_;                 ///< Custom icon name, empty = "play" default
    uint32_t icon_color_ = 0;              ///< Custom icon color (RGB), 0 = theme secondary
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(false);

    // Picker context menu
    lv_obj_t* picker_backdrop_ = nullptr;
    lv_obj_t* picker_icon_grid_ = nullptr;
    lv_obj_t* picker_color_grid_ = nullptr;
    lv_obj_t* picker_macro_list_ = nullptr;

    MoonrakerAPI* get_api() const;

    void update_display();
    void save_config();

    void fetch_and_execute();
    void execute_with_params(const std::map<std::string, std::string>& params);
    void show_macro_picker();
    void dismiss_macro_picker();
    void select_macro(const std::string& name);
    void select_icon(const std::string& name);
    void select_color(uint32_t color);

    void populate_macro_list(lv_obj_t* list, const std::vector<std::string>& macros);
    void populate_icon_grid(lv_obj_t* grid);
    void populate_color_grid(lv_obj_t* grid);
    void refresh_picker_highlights();

    // Static active instance for picker event routing
    static FavoriteMacroWidget* s_active_picker_;
};

} // namespace helix
