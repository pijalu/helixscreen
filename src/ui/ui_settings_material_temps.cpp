// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_material_temps.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "moonraker_api.h"
#include "ui_button.h"
#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"

#include "filament_database.h"
#include "material_settings_manager.h"
#include "static_panel_registry.h"
#include "theme_manager.h"
#include "ui_fonts.h"
#include "ui_toast_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <vector>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<MaterialTempsOverlay> g_material_temps_overlay;

MaterialTempsOverlay& get_material_temps_overlay() {
    if (!g_material_temps_overlay) {
        g_material_temps_overlay = std::make_unique<MaterialTempsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "MaterialTempsOverlay", []() { g_material_temps_overlay.reset(); });
    }
    return *g_material_temps_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

MaterialTempsOverlay::MaterialTempsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

MaterialTempsOverlay::~MaterialTempsOverlay() {
    if (subjects_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&has_macro_subject_);
        lv_subject_deinit(&editing_subject_);
        lv_subject_deinit(&edit_name_subject_);
        lv_subject_deinit(&edit_defaults_subject_);
    }
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void MaterialTempsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // View toggle subject: 0=list, 1=editing
    lv_subject_init_int(&editing_subject_, 0);
    lv_xml_register_subject(nullptr, "material_editing", &editing_subject_);

    // Edit view text subjects
    edit_name_buf_[0] = '\0';
    lv_subject_init_string(&edit_name_subject_, edit_name_buf_, nullptr, sizeof(edit_name_buf_),
                           edit_name_buf_);
    lv_xml_register_subject(nullptr, "material_edit_name", &edit_name_subject_);

    edit_defaults_buf_[0] = '\0';
    lv_subject_init_string(&edit_defaults_subject_, edit_defaults_buf_, nullptr,
                           sizeof(edit_defaults_buf_), edit_defaults_buf_);
    lv_xml_register_subject(nullptr, "material_edit_defaults", &edit_defaults_subject_);

    lv_subject_init_int(&has_macro_subject_, 0);
    lv_xml_register_subject(nullptr, "material_has_macro", &has_macro_subject_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void MaterialTempsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_material_save", on_material_save},
        {"on_material_reset_defaults", on_material_reset_defaults},
        {"on_open_macro_picker", on_open_macro_picker},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* MaterialTempsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "material_temps_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Cache view refs
    list_view_ = lv_obj_find_by_name(overlay_root_, "material_list_view");
    edit_view_ = lv_obj_find_by_name(overlay_root_, "material_edit_view");

    if (edit_view_) {
        macro_picker_btn_ = lv_obj_find_by_name(edit_view_, "macro_picker_btn");
        macro_heating_switch_ = lv_obj_find_by_name(edit_view_, "macro_heating_switch");
    }

    // Rewire back button to intercept when in edit view
    // Exception to "NO lv_obj_add_event_cb" rule: need to intercept back for view switching
    lv_obj_t* header = lv_obj_find_by_name(overlay_root_, "overlay_header");
    if (header) {
        lv_obj_t* back_button = lv_obj_find_by_name(header, "back_button");
        if (back_button) {
            uint32_t event_count = lv_obj_get_event_count(back_button);
            for (uint32_t i = event_count; i > 0; --i) {
                lv_obj_remove_event(back_button, i - 1);
            }
            lv_obj_add_event_cb(back_button, on_back_clicked, LV_EVENT_CLICKED, nullptr);
        }
    }

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void MaterialTempsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Lazy create overlay
    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Reset to list view
    show_list_view();

    // Register for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push onto navigation stack
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void MaterialTempsOverlay::on_activate() {
    OverlayBase::on_activate();
    populate_material_list();
}

void MaterialTempsOverlay::on_deactivate() {
    OverlayBase::on_deactivate();
}

// ============================================================================
// LIST VIEW
// ============================================================================

void MaterialTempsOverlay::populate_material_list() {
    if (!list_view_) {
        return;
    }

    // Clear existing children
    lv_obj_clean(list_view_);

    // Sort materials alphabetically by name
    std::vector<size_t> indices(filament::MATERIAL_COUNT);
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(), [](size_t a, size_t b) {
        return strcasecmp(filament::MATERIALS[a].name, filament::MATERIALS[b].name) < 0;
    });

    auto& mgr = MaterialSettingsManager::instance();

    for (size_t idx : indices) {
        const auto& mat = filament::MATERIALS[idx];

        // Look up effective values (with overrides applied)
        auto effective = filament::find_material(mat.name);
        int nozzle_min = effective ? effective->nozzle_min : mat.nozzle_min;
        int nozzle_max = effective ? effective->nozzle_max : mat.nozzle_max;
        int bed_temp = effective ? effective->bed_temp : mat.bed_temp;
        bool has_override = mgr.has_override(mat.name);

        // Material row
        auto* row = lv_obj_create(list_view_);
        lv_obj_set_name(row, mat.name);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_sm"), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        // Press feedback
        lv_obj_set_style_bg_color(row, theme_manager_get_color("primary"), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, 40, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 8, 0);
        // Exception: rows created programmatically, not from XML
        lv_obj_add_event_cb(row, on_material_row_clicked, LV_EVENT_CLICKED, nullptr);

        // Material name
        auto* name_label = lv_label_create(row);
        lv_label_set_text(name_label, mat.name);
        lv_obj_set_flex_grow(name_label, 1);
        lv_obj_set_style_text_color(name_label, theme_manager_get_color("text"), 0);
        lv_obj_add_flag(name_label, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(name_label, LV_OBJ_FLAG_CLICKABLE);

        // Override indicator (tune icon in primary color)
        if (has_override) {
            auto* indicator = lv_label_create(row);
            lv_label_set_text(indicator, "\xF3\xB0\x98\xAE"); // tune icon
            lv_obj_set_style_text_font(indicator, &mdi_icons_16, 0);
            lv_obj_set_style_text_color(indicator, theme_manager_get_color("primary"), 0);
            lv_obj_set_style_pad_right(indicator, theme_manager_get_spacing("space_xs"), 0);
            lv_obj_add_flag(indicator, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
        }

        // Temperature summary
        auto* temp_label = lv_label_create(row);
        char temp_buf[48];
        snprintf(temp_buf, sizeof(temp_buf), "%d-%d / %d°C", nozzle_min, nozzle_max, bed_temp);
        lv_label_set_text(temp_label, temp_buf);
        lv_obj_set_style_text_color(temp_label, theme_manager_get_color("text_muted"), 0);
        lv_obj_add_flag(temp_label, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(temp_label, LV_OBJ_FLAG_CLICKABLE);
    }

    spdlog::debug("[{}] Populated {} materials", get_name(), filament::MATERIAL_COUNT);
}

// ============================================================================
// EDIT VIEW
// ============================================================================

void MaterialTempsOverlay::show_edit_view(const std::string& material_name) {
    editing_material_ = material_name;

    // Get database defaults (from static array, NOT find_material which has overrides)
    int default_nozzle_min = 0, default_nozzle_max = 0, default_bed = 0;
    for (const auto& mat : filament::MATERIALS) {
        if (std::string_view(mat.name) == material_name) {
            default_nozzle_min = mat.nozzle_min;
            default_nozzle_max = mat.nozzle_max;
            default_bed = mat.bed_temp;
            break;
        }
    }

    // Get current effective values (with overrides if any)
    auto& mgr = MaterialSettingsManager::instance();
    const auto* ovr = mgr.get_override(material_name);
    int cur_nozzle_min = (ovr && ovr->nozzle_min) ? *ovr->nozzle_min : default_nozzle_min;
    int cur_nozzle_max = (ovr && ovr->nozzle_max) ? *ovr->nozzle_max : default_nozzle_max;
    int cur_bed = (ovr && ovr->bed_temp) ? *ovr->bed_temp : default_bed;

    // Update name subject
    snprintf(edit_name_buf_, sizeof(edit_name_buf_), "%s", material_name.c_str());
    lv_subject_copy_string(&edit_name_subject_, edit_name_buf_);

    // Update defaults hint
    snprintf(edit_defaults_buf_, sizeof(edit_defaults_buf_), "Default: %d-%d°C nozzle, %d°C bed",
             default_nozzle_min, default_nozzle_max, default_bed);
    lv_subject_copy_string(&edit_defaults_subject_, edit_defaults_buf_);

    // Populate input fields
    if (edit_view_) {
        lv_obj_t* nozzle_min_input = lv_obj_find_by_name(edit_view_, "edit_nozzle_min");
        lv_obj_t* nozzle_max_input = lv_obj_find_by_name(edit_view_, "edit_nozzle_max");
        lv_obj_t* bed_temp_input = lv_obj_find_by_name(edit_view_, "edit_bed_temp");

        char buf[8];
        if (nozzle_min_input) {
            snprintf(buf, sizeof(buf), "%d", cur_nozzle_min);
            lv_textarea_set_text(nozzle_min_input, buf);
        }
        if (nozzle_max_input) {
            snprintf(buf, sizeof(buf), "%d", cur_nozzle_max);
            lv_textarea_set_text(nozzle_max_input, buf);
        }
        if (bed_temp_input) {
            snprintf(buf, sizeof(buf), "%d", cur_bed);
            lv_textarea_set_text(bed_temp_input, buf);
        }
    }

    // Populate macro state
    selected_macro_.clear();
    bool switch_on = true; // Default: macro handles heating
    if (ovr && ovr->preheat_macro && !ovr->preheat_macro->empty()) {
        selected_macro_ = *ovr->preheat_macro;
        switch_on = ovr->macro_handles_heating.value_or(true);
    }
    populate_macro_picker_btn();

    if (macro_heating_switch_) {
        if (switch_on) {
            lv_obj_add_state(macro_heating_switch_, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(macro_heating_switch_, LV_STATE_CHECKED);
        }
    }

    // Switch to edit view
    lv_subject_set_int(&editing_subject_, 1);
    spdlog::debug("[{}] Editing material: {}", get_name(), material_name);
}

void MaterialTempsOverlay::show_list_view() {
    editing_material_.clear();
    lv_subject_set_int(&editing_subject_, 0);
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void MaterialTempsOverlay::handle_material_row_clicked(const std::string& material_name) {
    spdlog::debug("[{}] Material row clicked: {}", get_name(), material_name);
    show_edit_view(material_name);
}

void MaterialTempsOverlay::handle_save() {
    if (editing_material_.empty() || !edit_view_) {
        return;
    }

    // Read input values
    lv_obj_t* nozzle_min_input = lv_obj_find_by_name(edit_view_, "edit_nozzle_min");
    lv_obj_t* nozzle_max_input = lv_obj_find_by_name(edit_view_, "edit_nozzle_max");
    lv_obj_t* bed_temp_input = lv_obj_find_by_name(edit_view_, "edit_bed_temp");

    if (!nozzle_min_input || !nozzle_max_input || !bed_temp_input) {
        return;
    }

    const char* min_text = lv_textarea_get_text(nozzle_min_input);
    const char* max_text = lv_textarea_get_text(nozzle_max_input);
    const char* bed_text = lv_textarea_get_text(bed_temp_input);

    if (!min_text || !min_text[0] || !max_text || !max_text[0] || !bed_text || !bed_text[0]) {
        ToastManager::instance().show(ToastSeverity::WARNING, "All fields are required", 3000);
        return;
    }

    int nozzle_min = atoi(min_text);
    int nozzle_max = atoi(max_text);
    int bed_temp = atoi(bed_text);

    // Validate ranges
    if (nozzle_min < 100 || nozzle_max < 100 || nozzle_min > 500 || nozzle_max > 500) {
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      "Nozzle temp must be 100-500°C", 3000);
        return;
    }
    if (bed_temp < 0 || bed_temp > 200) {
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      "Bed temp must be 0-200°C", 3000);
        return;
    }
    if (nozzle_min > nozzle_max) {
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      "Nozzle min cannot exceed max", 3000);
        return;
    }

    // Get database defaults to compute sparse override
    filament::MaterialOverride ovr;
    for (const auto& mat : filament::MATERIALS) {
        if (std::string_view(mat.name) == editing_material_) {
            if (nozzle_min != mat.nozzle_min) ovr.nozzle_min = nozzle_min;
            if (nozzle_max != mat.nozzle_max) ovr.nozzle_max = nozzle_max;
            if (bed_temp != mat.bed_temp) ovr.bed_temp = bed_temp;
            break;
        }
    }

    // Add macro override if selected
    if (!selected_macro_.empty()) {
        ovr.preheat_macro = selected_macro_;
        bool switch_checked = macro_heating_switch_ &&
                              lv_obj_has_state(macro_heating_switch_, LV_STATE_CHECKED);
        // Only store macro_handles_heating if it differs from default (true)
        if (!switch_checked) {
            ovr.macro_handles_heating = false;
        }
    }

    // Only save if there are actual overrides
    if (ovr.nozzle_min || ovr.nozzle_max || ovr.bed_temp || ovr.preheat_macro) {
        MaterialSettingsManager::instance().set_override(editing_material_, ovr);
    } else {
        // All values match defaults — clear any existing override
        MaterialSettingsManager::instance().clear_override(editing_material_);
    }

    spdlog::info("[{}] Saved overrides for {}", get_name(), editing_material_);
    ToastManager::instance().show(ToastSeverity::SUCCESS, "Temperatures saved", 2000);

    // Return to list view and refresh, preserving scroll position
    int scroll_y = list_view_ ? lv_obj_get_scroll_y(list_view_) : 0;
    show_list_view();
    populate_material_list();
    if (list_view_ && scroll_y > 0) {
        lv_obj_scroll_to_y(list_view_, scroll_y, LV_ANIM_OFF);
    }
}

void MaterialTempsOverlay::handle_back_clicked() {
    if (lv_subject_get_int(&editing_subject_) != 0) {
        // In edit view — go back to list, preserving scroll position
        int scroll_y = list_view_ ? lv_obj_get_scroll_y(list_view_) : 0;
        show_list_view();
        populate_material_list();
        if (list_view_ && scroll_y > 0) {
            lv_obj_scroll_to_y(list_view_, scroll_y, LV_ANIM_OFF);
        }
    } else {
        // In list view — close overlay
        NavigationManager::instance().go_back();
    }
}

void MaterialTempsOverlay::handle_reset_defaults() {
    if (editing_material_.empty()) {
        return;
    }

    MaterialSettingsManager::instance().clear_override(editing_material_);
    spdlog::info("[{}] Reset {} to defaults", get_name(), editing_material_);

    // Return to list view and refresh, preserving scroll position
    int scroll_y = list_view_ ? lv_obj_get_scroll_y(list_view_) : 0;
    show_list_view();
    populate_material_list();
    if (list_view_ && scroll_y > 0) {
        lv_obj_scroll_to_y(list_view_, scroll_y, LV_ANIM_OFF);
    }
}

// ============================================================================
// MACRO PICKER
// ============================================================================

void MaterialTempsOverlay::populate_macro_picker_btn() {
    if (!macro_picker_btn_) return;

    if (selected_macro_.empty()) {
        ui_button_set_text(macro_picker_btn_, lv_tr("None"));
        lv_subject_set_int(&has_macro_subject_, 0);
    } else {
        ui_button_set_text(macro_picker_btn_, selected_macro_.c_str());
        lv_subject_set_int(&has_macro_subject_, 1);
    }
}

void MaterialTempsOverlay::show_macro_picker() {
    auto* api = get_moonraker_api();
    if (!api) {
        ToastManager::instance().show(ToastSeverity::WARNING,
                                      "Connect to a printer to see macros", 3000);
        return;
    }

    const auto& macros = api->hardware().macros();

    // Create a simple overlay with scrollable list
    auto* parent = overlay_root_ ? lv_obj_get_parent(overlay_root_) : lv_screen_active();
    auto* picker = lv_obj_create(parent);
    lv_obj_set_size(picker, lv_pct(90), lv_pct(80));
    lv_obj_center(picker);
    lv_obj_set_style_bg_color(picker, theme_manager_get_color("card_bg"), 0);
    lv_obj_set_style_bg_opa(picker, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(picker, 16, 0);
    lv_obj_set_style_pad_all(picker, theme_manager_get_spacing("space_md"), 0);
    lv_obj_set_flex_flow(picker, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_name(picker, "macro_picker_modal");

    // Title (lv_label_set_text justified: programmatic UI, no XML binding available)
    auto* title = lv_label_create(picker);
    lv_label_set_text(title, lv_tr("Select Preheat Macro"));
    lv_obj_set_style_text_color(title, theme_manager_get_color("text"), 0);
    lv_obj_set_style_text_font(title, theme_manager_get_font("font_heading"), 0);
    lv_obj_set_style_pad_bottom(title, theme_manager_get_spacing("space_sm"), 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    // Scrollable list
    auto* list = lv_obj_create(picker);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_gap(list, 0, 0);

    // "None" entry at top
    // Exception to "NO lv_obj_add_event_cb" rule: rows created programmatically, not from XML
    auto* none_row = lv_obj_create(list);
    lv_obj_set_name(none_row, "");  // Empty name = clear macro
    lv_obj_set_width(none_row, lv_pct(100));
    lv_obj_set_height(none_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(none_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(none_row, 0, 0);
    lv_obj_set_style_pad_all(none_row, theme_manager_get_spacing("space_sm"), 0);
    lv_obj_add_flag(none_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(none_row, theme_manager_get_color("primary"), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(none_row, 40, LV_STATE_PRESSED);
    lv_obj_set_style_radius(none_row, 8, 0);
    lv_obj_add_event_cb(none_row, on_macro_picker_row_clicked, LV_EVENT_CLICKED, nullptr);
    auto* none_label = lv_label_create(none_row);
    lv_label_set_text(none_label, lv_tr("None"));
    lv_obj_set_style_text_color(none_label, theme_manager_get_color("text_muted"), 0);
    lv_obj_add_flag(none_label, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(none_label, LV_OBJ_FLAG_CLICKABLE);

    // Sort macros alphabetically
    std::vector<std::string> sorted_macros(macros.begin(), macros.end());
    std::sort(sorted_macros.begin(), sorted_macros.end());

    for (const auto& macro_name : sorted_macros) {
        // Skip system macros (underscore prefix)
        if (!macro_name.empty() && macro_name[0] == '_') continue;

        auto* row = lv_obj_create(list);
        lv_obj_set_name(row, macro_name.c_str());
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_sm"), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, theme_manager_get_color("primary"), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, 40, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_add_event_cb(row, on_macro_picker_row_clicked, LV_EVENT_CLICKED, nullptr);

        auto* label = lv_label_create(row);
        std::string display = helix::get_display_name(macro_name, helix::DeviceType::MACRO);
        lv_label_set_text(label, display.c_str());
        lv_obj_set_style_text_color(label, theme_manager_get_color("text"), 0);
        lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);

        // Highlight currently selected
        if (macro_name == selected_macro_) {
            lv_obj_set_style_bg_color(row, theme_manager_get_color("primary"), 0);
            lv_obj_set_style_bg_opa(row, 30, 0);
        }
    }
}

void MaterialTempsOverlay::handle_macro_selected(const std::string& macro_name) {
    selected_macro_ = macro_name;
    populate_macro_picker_btn();

    // Close the picker modal
    lv_obj_t* picker = lv_obj_find_by_name(
        lv_obj_get_parent(overlay_root_), "macro_picker_modal");
    if (picker) {
        lv_obj_delete(picker);
    }

    spdlog::debug("[{}] Macro selected: '{}'", get_name(),
                  macro_name.empty() ? "(none)" : macro_name);
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void MaterialTempsOverlay::on_material_row_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MaterialTempsOverlay] on_material_row_clicked");
    auto* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const char* name = lv_obj_get_name(row);
    if (name) {
        get_material_temps_overlay().handle_material_row_clicked(name);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void MaterialTempsOverlay::on_material_save(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MaterialTempsOverlay] on_material_save");
    get_material_temps_overlay().handle_save();
    LVGL_SAFE_EVENT_CB_END();
}

void MaterialTempsOverlay::on_material_reset_defaults(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MaterialTempsOverlay] on_material_reset_defaults");
    get_material_temps_overlay().handle_reset_defaults();
    LVGL_SAFE_EVENT_CB_END();
}

void MaterialTempsOverlay::on_back_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MaterialTempsOverlay] on_back_clicked");
    get_material_temps_overlay().handle_back_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void MaterialTempsOverlay::on_open_macro_picker(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MaterialTempsOverlay] on_open_macro_picker");
    get_material_temps_overlay().show_macro_picker();
    LVGL_SAFE_EVENT_CB_END();
}

void MaterialTempsOverlay::on_macro_picker_row_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MaterialTempsOverlay] on_macro_picker_row_clicked");
    auto* row = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const char* name = lv_obj_get_name(row);
    get_material_temps_overlay().handle_macro_selected(name ? name : "");
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
