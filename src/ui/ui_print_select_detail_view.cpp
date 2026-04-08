// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_detail_view.h"

#include "ui_callback_helpers.h"
#include "ui_error_reporting.h"
#include "ui_filename_utils.h"
#include "ui_gcode_viewer.h"
#include "ui_icon.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_print_preparation_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "color_utils.h"
#include "config.h"
#include "display_settings_manager.h"
#include "gcode_parser.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_utils.h"
#include "moonraker_api.h"
#include "runtime_config.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <fstream>

namespace helix::ui {

// ============================================================================
// Static instance pointer for callback access
// ============================================================================

// Static instance pointer for XML event callbacks to access the PrintSelectDetailView
// Only one detail view exists at a time, set during init_subjects() / cleared in destructor
static PrintSelectDetailView* s_detail_view_instance = nullptr;

// Static flag to track if callbacks have been registered (idempotent registration)
static bool s_callbacks_registered = false;

// ============================================================================
// Static callback declarations
// ============================================================================

static void on_preprint_bed_mesh_toggled(lv_event_t* e);
static void on_preprint_qgl_toggled(lv_event_t* e);
static void on_preprint_z_tilt_toggled(lv_event_t* e);
static void on_preprint_nozzle_clean_toggled(lv_event_t* e);
static void on_preprint_purge_line_toggled(lv_event_t* e);
static void on_preprint_timelapse_toggled(lv_event_t* e);
static void update_prep_time_label();

// ============================================================================
// Lifecycle
// ============================================================================

PrintSelectDetailView::~PrintSelectDetailView() {
    // Clear static instance pointer
    if (s_detail_view_instance == this) {
        s_detail_view_instance = nullptr;
    }

    // lifetime_ destructor auto-invalidates all outstanding tokens

    // Clean up temp gcode file
    if (!temp_gcode_path_.empty()) {
        std::remove(temp_gcode_path_.c_str());
        temp_gcode_path_.clear();
    }

    // CRITICAL: During static destruction (app exit), LVGL may already be gone.
    // We check if LVGL is still initialized before calling any LVGL functions.
    if (!lv_is_initialized()) {
        spdlog::trace("[DetailView] Destroyed (LVGL already deinit)");
        return;
    }

    spdlog::trace("[DetailView] Destroyed");

    // Unregister from NavigationManager (fallback if cleanup() wasn't called)
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

    // Deinitialize subjects to disconnect observers before widgets are deleted
    // This prevents dangling pointers and frees observer linked lists
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // Clean up confirmation dialog if open
    if (confirmation_dialog_widget_) {
        helix::ui::modal_hide(confirmation_dialog_widget_);
        confirmation_dialog_widget_ = nullptr;
    }

    // Clean up main widget if created
    helix::ui::safe_delete(overlay_root_);
}

// ============================================================================
// Setup
// ============================================================================

void PrintSelectDetailView::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[DetailView] Subjects already initialized, skipping");
        return;
    }

    // Set static instance pointer for callbacks (must be before callback registration)
    s_detail_view_instance = this;

    // Register XML event callbacks BEFORE subjects (per [L013])
    // Callbacks must be registered before XML is created
    if (!s_callbacks_registered) {
        register_xml_callbacks({
            {"on_preprint_bed_mesh_toggled", on_preprint_bed_mesh_toggled},
            {"on_preprint_qgl_toggled", on_preprint_qgl_toggled},
            {"on_preprint_z_tilt_toggled", on_preprint_z_tilt_toggled},
            {"on_preprint_nozzle_clean_toggled", on_preprint_nozzle_clean_toggled},
            {"on_preprint_purge_line_toggled", on_preprint_purge_line_toggled},
            {"on_preprint_timelapse_toggled", on_preprint_timelapse_toggled},
        });
        s_callbacks_registered = true;
        spdlog::debug("[DetailView] Registered pre-print toggle callbacks");
    }

    // G-code viewer visibility mode (0=thumbnail, 1=3D, 2=2D)
    UI_MANAGED_SUBJECT_INT(detail_gcode_viewer_mode_, 0, "detail_gcode_viewer_mode", subjects_);
    // G-code loading indicator (0=hidden, 1=visible)
    UI_MANAGED_SUBJECT_INT(detail_gcode_loading_, 0, "detail_gcode_loading", subjects_);

    // Enable switches default ON (1) - "perform this operation"
    // Subject=1 means switch is checked, operation is enabled
    UI_MANAGED_SUBJECT_INT(preprint_bed_mesh_, 1, "preprint_bed_mesh", subjects_);
    UI_MANAGED_SUBJECT_INT(preprint_qgl_, 1, "preprint_qgl", subjects_);
    UI_MANAGED_SUBJECT_INT(preprint_z_tilt_, 1, "preprint_z_tilt", subjects_);
    UI_MANAGED_SUBJECT_INT(preprint_nozzle_clean_, 1, "preprint_nozzle_clean", subjects_);
    UI_MANAGED_SUBJECT_INT(preprint_purge_line_, 1, "preprint_purge_line", subjects_);

    // Add-on switches default OFF (0) - "don't add extras by default"
    UI_MANAGED_SUBJECT_INT(preprint_timelapse_, 0, "preprint_timelapse", subjects_);

    // Filament mismatch warning (0=hidden, 1=visible)
    UI_MANAGED_SUBJECT_INT(filament_mismatch_, 0, "filament_mismatch", subjects_);

    // Pre-print time estimate (formatted string for bind_text)
    UI_MANAGED_SUBJECT_STRING(prep_time_estimate_subject_, prep_time_estimate_buf_, "",
                              "preprint_estimate_text", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[DetailView] Initialized pre-print option subjects");
}

lv_obj_t* PrintSelectDetailView::create(lv_obj_t* parent_screen) {
    if (!parent_screen) {
        spdlog::error("[DetailView] Cannot create: parent_screen is null");
        return nullptr;
    }

    if (overlay_root_) {
        spdlog::warn("[DetailView] Detail view already exists");
        return overlay_root_;
    }

    parent_screen_ = parent_screen;

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "print_file_detail", nullptr));

    if (!overlay_root_) {
        LOG_ERROR_INTERNAL("[DetailView] Failed to create detail view from XML");
        NOTIFY_ERROR(lv_tr("Failed to load file details"));
        return nullptr;
    }

    // Swap gradient images to match current theme (XML hardcodes -dark.bin)
    theme_manager_swap_gradients(overlay_root_);

    // Set width to fill space after nav bar
    ui_set_overlay_width(overlay_root_, parent_screen_);

    // Set responsive padding for content area
    lv_obj_t* content_container = lv_obj_find_by_name(overlay_root_, "content_container");
    if (content_container) {
        lv_coord_t padding = ui_get_header_content_padding(lv_obj_get_height(parent_screen_));
        lv_obj_set_style_pad_all(content_container, padding, 0);
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    // Store reference to print button for enable/disable state management
    print_button_ = lv_obj_find_by_name(overlay_root_, "print_button");

    // Find and configure G-code viewer widget
    gcode_viewer_ = lv_obj_find_by_name(overlay_root_, "detail_gcode_viewer");
    if (gcode_viewer_) {
        spdlog::debug("[DetailView] G-code viewer widget found");
        ui_gcode_viewer_disable_streaming(gcode_viewer_);

        // Apply render mode - priority: cmdline > env var > settings
        const auto* config = get_runtime_config();
        const char* env_mode = std::getenv("HELIX_GCODE_MODE");

        if (config && config->gcode_render_mode >= 0) {
            auto render_mode = static_cast<helix::GcodeViewerRenderMode>(config->gcode_render_mode);
            ui_gcode_viewer_set_render_mode(gcode_viewer_, render_mode);
            spdlog::debug("[DetailView] Set G-code render mode: {} (cmdline)",
                          config->gcode_render_mode);
        } else if (env_mode) {
            spdlog::debug("[DetailView] G-code render mode: {} (env var)",
                          ui_gcode_viewer_is_using_2d_mode(gcode_viewer_) ? "2D" : "3D");
        } else {
            int render_mode_val = DisplaySettingsManager::instance().get_gcode_render_mode();
            if (render_mode_val == 3) {
                // Thumbnail Only mode - skip render mode setup, viewer won't be used
                spdlog::debug("[DetailView] G-code render mode: Thumbnail Only (settings)");
            } else {
                auto render_mode = static_cast<helix::GcodeViewerRenderMode>(render_mode_val);
                ui_gcode_viewer_set_render_mode(gcode_viewer_, render_mode);
                spdlog::debug("[DetailView] Set G-code render mode: {} (settings)",
                              render_mode_val);
            }
        }

        // Vertical offset to match thumbnail positioning
        ui_gcode_viewer_set_content_offset_y(gcode_viewer_, -0.10f);

        // Start paused — will resume in on_activate()
        ui_gcode_viewer_set_paused(gcode_viewer_, true);
    }

    // Look up pre-print option checkboxes
    bed_mesh_checkbox_ = lv_obj_find_by_name(overlay_root_, "bed_mesh_checkbox");
    qgl_checkbox_ = lv_obj_find_by_name(overlay_root_, "qgl_checkbox");
    z_tilt_checkbox_ = lv_obj_find_by_name(overlay_root_, "z_tilt_checkbox");
    nozzle_clean_checkbox_ = lv_obj_find_by_name(overlay_root_, "nozzle_clean_checkbox");
    purge_line_checkbox_ = lv_obj_find_by_name(overlay_root_, "purge_line_checkbox");
    timelapse_checkbox_ = lv_obj_find_by_name(overlay_root_, "timelapse_checkbox");

    // Look up color requirements display
    color_requirements_card_ = lv_obj_find_by_name(overlay_root_, "color_requirements_card");
    color_swatches_row_ = lv_obj_find_by_name(overlay_root_, "color_swatches_row");

    // Look up and initialize filament mapping card
    lv_obj_t* mapping_card = lv_obj_find_by_name(overlay_root_, "filament_mapping_card");
    lv_obj_t* mapping_rows = lv_obj_find_by_name(overlay_root_, "filament_mapping_rows");
    lv_obj_t* mapping_warning = lv_obj_find_by_name(overlay_root_, "filament_mapping_warning");
    filament_mapping_card_.create(mapping_card, mapping_rows, mapping_warning);
    filament_mapping_card_.set_on_mappings_changed([this]() {
        apply_mapped_tool_colors();
        lv_subject_set_int(&filament_mismatch_, filament_mapping_card_.has_mismatch() ? 1 : 0);
    });

    // Look up history status display
    history_status_row_ = lv_obj_find_by_name(overlay_root_, "history_status_row");
    history_status_icon_ = lv_obj_find_by_name(overlay_root_, "history_status_icon");
    history_status_label_ = lv_obj_find_by_name(overlay_root_, "history_status_label");

    // Initialize print preparation manager (only if not already created —
    // survives destroy-on-close so callbacks set by PrintSelectPanel persist)
    if (!prep_manager_) {
        prep_manager_ = std::make_unique<PrintPreparationManager>();
    }

    spdlog::debug("[DetailView] Detail view created");
    return overlay_root_;
}

void PrintSelectDetailView::set_dependencies(MoonrakerAPI* api, PrinterState* printer_state) {
    api_ = api;
    printer_state_ = printer_state;

    if (prep_manager_) {
        prep_manager_->set_dependencies(api_, printer_state_);

        // LT2: Wire up subjects for declarative state reading
        prep_manager_->set_preprint_subjects(
            get_preprint_bed_mesh_subject(), get_preprint_qgl_subject(),
            get_preprint_z_tilt_subject(), get_preprint_nozzle_clean_subject(),
            get_preprint_purge_line_subject(), get_preprint_timelapse_subject());

        // Wire up visibility subjects from PrinterState
        if (printer_state_) {
            prep_manager_->set_preprint_visibility_subjects(
                printer_state_->get_can_show_bed_mesh_subject(),
                printer_state_->get_can_show_qgl_subject(),
                printer_state_->get_can_show_z_tilt_subject(),
                printer_state_->get_can_show_nozzle_clean_subject(),
                printer_state_->get_can_show_purge_line_subject(),
                printer_state_->get_printer_has_timelapse_subject());
        }
    }
}

// ============================================================================
// Visibility
// ============================================================================

void PrintSelectDetailView::show(const std::string& filename, const std::string& current_path,
                                 const std::string& filament_type,
                                 const std::vector<std::string>& filament_colors,
                                 const std::vector<std::string>& filament_materials,
                                 size_t file_size_bytes) {
    // Lazy re-create widget tree if it was destroyed by destroy-on-close
    if (!overlay_root_ && parent_screen_) {
        spdlog::info("[DetailView] Re-creating widget tree (destroy-on-close recovery)");
        if (!create(parent_screen_)) {
            spdlog::error("[DetailView] Failed to re-create detail view");
            return;
        }
        // Re-wire dependencies (subjects need re-binding to new widgets)
        if (api_ || printer_state_) {
            set_dependencies(api_, printer_state_);
        }
    }

    if (!overlay_root_) {
        spdlog::warn("[DetailView] Cannot show: widget not created");
        return;
    }

    // Cache parameters for on_activate() to use
    current_filename_ = filename;
    current_path_ = current_path;
    current_filament_type_ = filament_type;
    current_filament_colors_ = filament_colors;
    current_filament_materials_ = filament_materials;
    current_file_size_bytes_ = file_size_bytes;

    // Update filament mapping card (shown when AMS is available)
    filament_mapping_card_.update(filament_colors, filament_materials);

    // Update mismatch warning icon via subject binding
    lv_subject_set_int(&filament_mismatch_, filament_mapping_card_.has_mismatch() ? 1 : 0);

    // Show either the interactive mapping card OR the simple color swatches, never both
    if (filament_mapping_card_.is_visible()) {
        // Mapping card is active — hide legacy color swatches
        if (color_requirements_card_) {
            lv_obj_add_flag(color_requirements_card_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        // No AMS — fall back to simple color swatches display
        update_color_swatches(filament_colors);
    }

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Register close callback to destroy widget tree when overlay closes.
    // Frees memory when detail view is dismissed. Subjects survive;
    // next show() call re-creates widgets via lazy creation above.
    NavigationManager::instance().register_overlay_close_callback(
        overlay_root_, [this]() { destroy_overlay_ui(overlay_root_); });

    // Push onto navigation stack - on_activate() will be called by NavigationManager
    NavigationManager::instance().push_overlay(overlay_root_);

    if (visible_subject_) {
        lv_subject_set_int(visible_subject_, 1);
    }

    spdlog::debug("[DetailView] Showing detail view for: {} ({} colors)", filename,
                  filament_colors.size());
}

void PrintSelectDetailView::hide() {
    if (!overlay_root_) {
        return;
    }

    // Pop from navigation stack - on_deactivate() will be called by NavigationManager
    NavigationManager::instance().go_back();

    if (visible_subject_) {
        lv_subject_set_int(visible_subject_, 0);
    }

    spdlog::debug("[DetailView] Detail view hidden");
}

// ============================================================================
// Lifecycle Hooks (called by NavigationManager)
// ============================================================================

void PrintSelectDetailView::on_activate() {
    // Call base class first
    OverlayBase::on_activate();

    spdlog::debug("[DetailView] on_activate() for file: {}", current_filename_);

    // Reset pre-print option subjects to defaults for new file
    // Skip switches default ON (don't skip = preserve file's original behavior)
    lv_subject_set_int(&preprint_bed_mesh_, 1);
    lv_subject_set_int(&preprint_qgl_, 1);
    lv_subject_set_int(&preprint_z_tilt_, 1);
    lv_subject_set_int(&preprint_nozzle_clean_, 1);
    lv_subject_set_int(&preprint_purge_line_, 1);
    // Timelapse stays OFF by default (it's an add-on feature)
    lv_subject_set_int(&preprint_timelapse_, 0);

    // Cache file size for safety checks (before modification attempts)
    if (prep_manager_ && current_file_size_bytes_ > 0) {
        prep_manager_->set_cached_file_size(current_file_size_bytes_);
    }

    // Trigger async scan for embedded G-code operations (for conflict detection)
    // The scan happens NOW after registration, so if user navigates away,
    // on_deactivate() will be called and we can check cleanup_called()
    if (!current_filename_.empty() && prep_manager_) {
        prep_manager_->scan_file_for_operations(current_filename_, current_path_);
    }

    // Calculate initial pre-print time estimate
    update_prep_time_label();

    // Load gcode for 3D/2D preview (viewer stays paused until load completes)
    load_gcode_for_preview();
}

void PrintSelectDetailView::on_deactivate() {
    spdlog::debug("[DetailView] on_deactivate()");

    // Clear and pause gcode viewer immediately so the old model doesn't
    // linger when the user selects a different file
    if (gcode_viewer_) {
        ui_gcode_viewer_clear(gcode_viewer_);
        ui_gcode_viewer_set_paused(gcode_viewer_, true);
    }

    // Reset viewer mode to thumbnail so next open starts clean
    show_gcode_viewer(false);
    gcode_loaded_ = false;

    // Hide any open delete confirmation modal
    hide_delete_confirmation();

    // Note: We don't cancel scans here because PrintPreparationManager
    // has its own lifetime guard. Async callbacks in prep_manager_
    // will check cleanup_called() if needed.

    // Call base class
    OverlayBase::on_deactivate();
}

void PrintSelectDetailView::cleanup() {
    spdlog::debug("[DetailView] cleanup()");

    // Pause viewer before subject cleanup to avoid rendering with freed subjects
    if (gcode_viewer_) {
        ui_gcode_viewer_set_paused(gcode_viewer_, true);
    }

    // Expire all outstanding async tokens
    lifetime_.invalidate();

    // Unregister from NavigationManager before cleaning up
    if (overlay_root_) {
        NavigationManager::instance().unregister_overlay_instance(overlay_root_);
    }

    // Deinitialize subjects to disconnect observers
    if (subjects_initialized_) {
        subjects_.deinit_all();
        subjects_initialized_ = false;
    }

    // Call base class to set cleanup_called_ flag
    OverlayBase::cleanup();
}

// ============================================================================
// Destroy-on-close support
// ============================================================================

void PrintSelectDetailView::on_ui_destroyed() {
    spdlog::debug("[DetailView] on_ui_destroyed() - nulling widget pointers");

    // Invalidate outstanding tokens so in-flight async callbacks (gcode download,
    // metadata fetch, load callbacks) bail out — they captured pointers to
    // widgets that no longer exist (e.g. gcode_viewer_).
    // New tokens from lifetime_.token() will be valid for the next create() cycle.
    lifetime_.invalidate();

    // Pause and clear gcode viewer state (widget is already deleted by base)
    gcode_loaded_ = false;

    // Clean up temp gcode file so stale cached data doesn't persist
    if (!temp_gcode_path_.empty()) {
        std::remove(temp_gcode_path_.c_str());
        temp_gcode_path_.clear();
    }

    // Null all child widget pointers (widget tree already deleted by base class)
    // Note: parent_screen_ is NOT nulled — it's the parent screen (not a child
    // widget) and is needed for lazy re-creation in show().
    confirmation_dialog_widget_ = nullptr;
    print_button_ = nullptr;
    gcode_viewer_ = nullptr;

    // Pre-print option checkboxes
    bed_mesh_checkbox_ = nullptr;
    qgl_checkbox_ = nullptr;
    z_tilt_checkbox_ = nullptr;
    nozzle_clean_checkbox_ = nullptr;
    purge_line_checkbox_ = nullptr;
    timelapse_checkbox_ = nullptr;

    // Color requirements display
    color_requirements_card_ = nullptr;
    color_swatches_row_ = nullptr;

    // Filament mapping card
    filament_mapping_card_.on_ui_destroyed();

    // History status display
    history_status_row_ = nullptr;
    history_status_icon_ = nullptr;
    history_status_label_ = nullptr;

    // Note: prep_manager_ is NOT reset — it holds no widget references and
    // retains its callbacks (scan_complete, macro_analysis) set by PrintSelectPanel.
    // It will be re-wired with dependencies in show() -> set_dependencies().
}

// ============================================================================
// Delete Confirmation
// ============================================================================

void PrintSelectDetailView::show_delete_confirmation(const std::string& filename) {
    // Create message with current filename
    char msg_buf[256];
    snprintf(msg_buf, sizeof(msg_buf),
             "Are you sure you want to delete '%s'? This action cannot be undone.",
             filename.c_str());

    confirmation_dialog_widget_ = helix::ui::modal_show_confirmation(
        lv_tr("Delete File?"), msg_buf, ModalSeverity::Warning, lv_tr("Delete"),
        on_confirm_delete_static, on_cancel_delete_static, this);

    if (!confirmation_dialog_widget_) {
        spdlog::error("[DetailView] Failed to create confirmation dialog");
        return;
    }

    spdlog::info("[DetailView] Delete confirmation dialog shown for: {}", filename);
}

void PrintSelectDetailView::hide_delete_confirmation() {
    if (confirmation_dialog_widget_) {
        helix::ui::modal_hide(confirmation_dialog_widget_);
        confirmation_dialog_widget_ = nullptr;
    }
}

// ============================================================================
// Resize Handling
// ============================================================================

void PrintSelectDetailView::handle_resize(lv_obj_t* parent_screen) {
    if (!overlay_root_ || !parent_screen) {
        return;
    }

    lv_obj_t* content_container = lv_obj_find_by_name(overlay_root_, "content_container");
    if (content_container) {
        lv_coord_t padding = ui_get_header_content_padding(lv_obj_get_height(parent_screen));
        lv_obj_set_style_pad_all(content_container, padding, 0);
    }
}

// ============================================================================
// Internal Methods
// ============================================================================

void PrintSelectDetailView::on_confirm_delete_static(lv_event_t* e) {
    auto* self = static_cast<PrintSelectDetailView*>(lv_event_get_user_data(e));
    if (self) {
        self->hide_delete_confirmation();
        if (self->on_delete_confirmed_) {
            self->on_delete_confirmed_();
        }
    }
}

void PrintSelectDetailView::on_cancel_delete_static(lv_event_t* e) {
    auto* self = static_cast<PrintSelectDetailView*>(lv_event_get_user_data(e));
    if (self) {
        self->hide_delete_confirmation();
    }
}

void PrintSelectDetailView::update_color_swatches(const std::vector<std::string>& colors) {
    if (!color_requirements_card_ || !color_swatches_row_) {
        return;
    }

    // Hide card if no colors or single color (single-color prints don't need this display)
    if (colors.size() <= 1) {
        lv_obj_add_flag(color_requirements_card_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // Clear existing swatches
    lv_obj_clean(color_swatches_row_);

    // Create swatches for each color
    for (size_t i = 0; i < colors.size(); ++i) {
        const std::string& hex_color = colors[i];

        // Create swatch container with tool number label
        lv_obj_t* swatch = lv_obj_create(color_swatches_row_);
        lv_obj_remove_style_all(swatch);
        lv_obj_set_flex_grow(swatch, 1);
        lv_obj_set_height(swatch, LV_PCT(100));
        lv_obj_set_style_radius(swatch, 4, 0);
        lv_obj_set_style_border_width(swatch, 1, 0);
        lv_obj_set_style_border_color(swatch, lv_color_white(), 0);
        lv_obj_set_style_border_opa(swatch, 30, 0);
        lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);

        // Parse and set background color
        if (!hex_color.empty()) {
            lv_color_t color = theme_manager_parse_hex_color(hex_color.c_str());
            lv_obj_set_style_bg_color(swatch, color, 0);
            lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        } else {
            // Empty color - show gray placeholder
            lv_obj_set_style_bg_color(swatch, theme_manager_get_color("text_muted"), 0);
            lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        }

        // Add tool number label (T0, T1, etc.)
        lv_obj_t* label = lv_label_create(swatch);
        char tool_str[8];
        snprintf(tool_str, sizeof(tool_str), "T%zu", i);
        lv_label_set_text(label, tool_str);
        lv_obj_center(label);
        lv_obj_set_style_text_font(label, theme_manager_get_font("font_small"), 0);

        // Use contrasting text color based on background brightness
        auto parsed_color = helix::parse_hex_color(hex_color);
        if (parsed_color) {
            uint32_t rgb = *parsed_color;
            int r = (rgb >> 16) & 0xFF;
            int g = (rgb >> 8) & 0xFF;
            int b = rgb & 0xFF;
            // Simple brightness check using luminance weights
            int brightness = (r * 299 + g * 587 + b * 114) / 1000;
            lv_color_t text_color = brightness > 128 ? lv_color_black() : lv_color_white();
            lv_obj_set_style_text_color(label, text_color, 0);
        }
    }

    // Show the card
    lv_obj_remove_flag(color_requirements_card_, LV_OBJ_FLAG_HIDDEN);

    spdlog::debug("[DetailView] Updated color swatches: {} colors", colors.size());
}

void PrintSelectDetailView::update_history_status(FileHistoryStatus status, int success_count) {
    if (!history_status_row_ || !history_status_icon_ || !history_status_label_) {
        return;
    }

    switch (status) {
    case FileHistoryStatus::NEVER_PRINTED:
        // Hide the row entirely for files with no history
        lv_obj_add_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        break;

    case FileHistoryStatus::CURRENTLY_PRINTING:
        lv_obj_remove_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        ui_icon_set_source(history_status_icon_, "clock");
        ui_icon_set_variant(history_status_icon_, "accent");
        lv_label_set_text(history_status_label_, lv_tr("Currently printing"));
        break;

    case FileHistoryStatus::COMPLETED: {
        lv_obj_remove_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        ui_icon_set_source(history_status_icon_, "check");
        ui_icon_set_variant(history_status_icon_, "success");
        // Format: "Printed N time(s)"
        char buf[64];
        snprintf(buf, sizeof(buf),
                 lv_tr(success_count == 1 ? "Printed %d time" : "Printed %d times"), success_count);
        lv_label_set_text(history_status_label_, buf);
        break;
    }

    case FileHistoryStatus::FAILED:
        lv_obj_remove_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        ui_icon_set_source(history_status_icon_, "alert");
        ui_icon_set_variant(history_status_icon_, "error");
        lv_label_set_text(history_status_label_, lv_tr("Last print failed"));
        break;

    case FileHistoryStatus::CANCELLED:
        lv_obj_remove_flag(history_status_row_, LV_OBJ_FLAG_HIDDEN);
        ui_icon_set_source(history_status_icon_, "cancel");
        ui_icon_set_variant(history_status_icon_, "warning");
        lv_label_set_text(history_status_label_, lv_tr("Last print cancelled"));
        break;
    }
}

// ============================================================================
// G-code Viewer
// ============================================================================

void PrintSelectDetailView::show_gcode_viewer(bool show) {
    // Mode 0 = thumbnail, 1 = 3D
    // Detail panel only supports 3D viewer — fall back to thumbnail if 2D
    int mode = 0;
    if (show) {
        bool is_2d = gcode_viewer_ && ui_gcode_viewer_is_using_2d_mode(gcode_viewer_);
        if (!is_2d) {
            mode = 1;
        }
    }
    lv_subject_set_int(&detail_gcode_viewer_mode_, mode);

    // Hide loading spinner now that viewer state is resolved
    lv_subject_set_int(&detail_gcode_loading_, 0);

    spdlog::trace("[DetailView] G-code viewer mode: {} ({})", mode, mode == 0 ? "thumbnail" : "3D");
}

void PrintSelectDetailView::apply_tool_colors() {
    if (!gcode_viewer_ || !gcode_loaded_) {
        return;
    }

    // Try AMS slot colors first
    if (ui_gcode_viewer_apply_ams_tool_colors(gcode_viewer_)) {
        return;
    }

    // Fallback: use file metadata colors (from slicer)
    if (!current_filament_colors_.empty()) {
        std::vector<uint32_t> tool_colors;
        for (const auto& hex : current_filament_colors_) {
            auto parsed = helix::parse_hex_color(hex);
            if (parsed) {
                tool_colors.push_back(*parsed);
            }
        }
        if (!tool_colors.empty()) {
            ui_gcode_viewer_set_tool_colors(gcode_viewer_, tool_colors);
        }
    }
}

void PrintSelectDetailView::apply_mapped_tool_colors() {
    if (!gcode_viewer_ || !gcode_loaded_) {
        return;
    }

    auto colors = filament_mapping_card_.get_mapped_colors();
    if (!colors.empty()) {
        ui_gcode_viewer_set_tool_colors(gcode_viewer_, colors);
        lv_obj_invalidate(gcode_viewer_);
        spdlog::debug("[DetailView] Applied {} mapped tool colors to gcode viewer", colors.size());
    }
}

void PrintSelectDetailView::try_extract_gcode_colors(lv_obj_t* viewer) {
    // Only needed when metadata didn't provide colors
    if (!current_filament_colors_.empty()) {
        return;
    }

    auto* parsed = ui_gcode_viewer_get_parsed_file(viewer);
    if (!parsed || parsed->tool_color_palette.empty()) {
        return;
    }

    spdlog::info("[DetailView] Metadata lacked filament colors — extracted {} from parsed gcode",
                 parsed->tool_color_palette.size());
    current_filament_colors_ = parsed->tool_color_palette;

    // Update the mapping card with extracted colors
    filament_mapping_card_.update(current_filament_colors_, current_filament_materials_);
    lv_subject_set_int(&filament_mismatch_, filament_mapping_card_.has_mismatch() ? 1 : 0);

    // Hide legacy color swatches if mapping card is now visible
    if (filament_mapping_card_.is_visible() && color_requirements_card_) {
        lv_obj_add_flag(color_requirements_card_, LV_OBJ_FLAG_HIDDEN);
    }
}

void PrintSelectDetailView::load_gcode_for_preview() {
    // Skip if no viewer widget
    if (!gcode_viewer_) {
        spdlog::debug("[DetailView] No gcode_viewer_ widget - skipping G-code preview");
        return;
    }

    // Skip if no API available
    if (!api_) {
        spdlog::debug("[DetailView] No API available - skipping G-code preview");
        return;
    }

    // Skip if no filename
    if (current_filename_.empty()) {
        spdlog::debug("[DetailView] No filename - skipping G-code preview");
        return;
    }

    // Clear previous model so stale frames don't flash when viewer becomes visible
    ui_gcode_viewer_clear(gcode_viewer_);

    // Show loading spinner over thumbnail
    lv_subject_set_int(&detail_gcode_loading_, 1);

    // Check "Thumbnail Only" render mode - skip all gcode downloading/parsing
    if (DisplaySettingsManager::instance().get_gcode_render_mode() == 3) {
        spdlog::info("[DetailView] G-code render mode is Thumbnail Only - skipping G-code load");
        lv_subject_set_int(&detail_gcode_loading_, 0);
        show_gcode_viewer(false);
        return;
    }

    // Detail page only shows the 3D viewer — skip download/parse on 2D-only platforms
    if (ui_gcode_viewer_is_using_2d_mode(gcode_viewer_)) {
        spdlog::debug("[DetailView] 2D-only platform — skipping G-code preview (thumbnail only)");
        lv_subject_set_int(&detail_gcode_loading_, 0);
        show_gcode_viewer(false);
        return;
    }

    // Generate temp file path with caching
    std::string cache_dir = get_helix_cache_dir("gcode_temp");
    if (cache_dir.empty()) {
        spdlog::warn("[DetailView] No writable cache directory - skipping G-code preview");
        lv_subject_set_int(&detail_gcode_loading_, 0);
        show_gcode_viewer(false);
        return;
    }
    std::string temp_path = cache_dir + "/detail_preview_" +
                            std::to_string(std::hash<std::string>{}(current_filename_)) + ".gcode";

    // Check if file already exists and is non-empty (cached from previous session)
    std::ifstream cached_file(temp_path, std::ios::binary | std::ios::ate);
    if (cached_file && cached_file.tellg() > 0) {
        size_t cached_size = static_cast<size_t>(cached_file.tellg());
        cached_file.close();

        if (helix::is_gcode_2d_streaming_safe(cached_size)) {
            spdlog::info("[DetailView] Using cached G-code file ({} bytes): {}", cached_size,
                         temp_path);
            temp_gcode_path_ = temp_path;

            // Set up load callback and load the file
            ui_gcode_viewer_set_load_callback(
                gcode_viewer_,
                [](lv_obj_t* viewer, void* user_data, bool success) {
                    auto* self = static_cast<PrintSelectDetailView*>(user_data);
                    if (!success) {
                        spdlog::warn("[DetailView] G-code load failed from cache");
                        self->show_gcode_viewer(false);
                        return;
                    }
                    self->gcode_loaded_ = true;

                    // Show all layers, no ghost (preview = full model)
                    ui_gcode_viewer_set_print_progress(viewer, -1);

                    // Apply AMS or slicer tool colors, then override with mapped colors
                    self->apply_tool_colors();
                    self->apply_mapped_tool_colors();

                    // Extract colors from parsed gcode when metadata lacked them
                    self->try_extract_gcode_colors(viewer);

                    // Unpause, show, then reset camera (must be visible for layout)
                    ui_gcode_viewer_set_paused(viewer, false);
                    self->show_gcode_viewer(true);
                    lv_obj_update_layout(viewer);
                    ui_gcode_viewer_reset_camera(viewer);

                    spdlog::debug("[DetailView] G-code preview loaded from cache");
                },
                this);
            ui_gcode_viewer_load_file(gcode_viewer_, temp_path.c_str());
            return;
        } else {
            spdlog::debug("[DetailView] Cached file too large for streaming, removing");
            std::remove(temp_path.c_str());
        }
    }

    // Build full relative path for metadata lookup and download
    std::string file_path =
        current_path_.empty() ? current_filename_ : current_path_ + "/" + current_filename_;
    std::string metadata_filename = file_path;

    auto tok = lifetime_.token();

    api_->files().get_file_metadata(
        metadata_filename,
        [this, tok, temp_path, file_path](const FileMetadata& metadata) {
            if (tok.expired()) {
                return;
            }

            // Check if file is safe to render given available RAM
            if (!helix::is_gcode_2d_streaming_safe(metadata.size)) {
                auto mem = helix::get_system_memory_info();
                spdlog::warn("[DetailView] G-code too large for streaming: file={} bytes, "
                             "available RAM={}MB - using thumbnail",
                             metadata.size, mem.available_mb());
                show_gcode_viewer(false);
                return;
            }

            spdlog::debug("[DetailView] G-code size {} bytes - safe to render, downloading...",
                          metadata.size);

            // Clean up previous temp file if different
            if (!temp_gcode_path_.empty() && temp_gcode_path_ != temp_path) {
                std::remove(temp_gcode_path_.c_str());
                temp_gcode_path_.clear();
            }

            // Stream download to disk
            api_->transfers().download_file_to_path(
                "gcodes", file_path, temp_path,
                [this, tok, temp_path](const std::string& path) {
                    if (tok.expired()) {
                        return;
                    }
                    // Marshal to main thread — this callback runs on HTTP thread
                    helix::ui::queue_update([this, tok, path]() {
                        if (tok.expired()) {
                            return;
                        }
                        temp_gcode_path_ = path;

                        spdlog::debug("[DetailView] G-code downloaded, loading into viewer: {}",
                                      path);

                        // Set up load callback
                        ui_gcode_viewer_set_load_callback(
                            gcode_viewer_,
                            [](lv_obj_t* viewer, void* user_data, bool success) {
                                auto* self = static_cast<PrintSelectDetailView*>(user_data);
                                if (!success) {
                                    spdlog::warn("[DetailView] G-code load failed after download");
                                    self->show_gcode_viewer(false);
                                    return;
                                }
                                self->gcode_loaded_ = true;

                                // Show all layers, no ghost (preview = full model)
                                ui_gcode_viewer_set_print_progress(viewer, -1);

                                // Apply AMS or slicer tool colors, then override with mapped colors
                                self->apply_tool_colors();
                                self->apply_mapped_tool_colors();

                                // Extract colors from parsed gcode when metadata lacked them
                                self->try_extract_gcode_colors(viewer);

                                // Unpause, show, then reset camera (must be visible for layout)
                                ui_gcode_viewer_set_paused(viewer, false);
                                self->show_gcode_viewer(true);
                                lv_obj_update_layout(viewer);
                                ui_gcode_viewer_reset_camera(viewer);

                                spdlog::debug("[DetailView] G-code preview loaded successfully");
                            },
                            this);

                        // Load into viewer
                        ui_gcode_viewer_load_file(gcode_viewer_, path.c_str());
                    });
                },
                [this, tok](const MoonrakerError& err) {
                    if (tok.expired()) {
                        return;
                    }
                    spdlog::warn("[DetailView] Failed to download G-code: {}", err.message);
                    // Marshal to main thread — this callback runs on HTTP thread
                    helix::ui::queue_update([this, tok]() {
                        if (tok.expired())
                            return;
                        show_gcode_viewer(false);
                    });
                });
        },
        [this, tok](const MoonrakerError& err) {
            if (tok.expired()) {
                return;
            }
            spdlog::debug("[DetailView] Failed to get G-code metadata: {} - skipping preview",
                          err.message);
            show_gcode_viewer(false);
        },
        true // silent
    );
}

// ============================================================================
// Pre-print Estimate Label Update
// ============================================================================

static void update_prep_time_label() {
    if (!s_detail_view_instance || !s_detail_view_instance->get_prep_manager()) {
        return;
    }
    auto* mgr = s_detail_view_instance->get_prep_manager();
    mgr->recalculate_estimate();

    int estimate_s = lv_subject_get_int(mgr->get_preprint_estimate_subject());

    if (estimate_s <= 0) {
        lv_subject_copy_string(s_detail_view_instance->get_prep_time_estimate_subject(), "");
        return;
    }

    // Round: >120s to nearest 30s, <=120s to nearest 10s
    int rounded = estimate_s > 120 ? ((estimate_s + 15) / 30) * 30 : ((estimate_s + 5) / 10) * 10;
    int mins = rounded / 60;
    int secs = rounded % 60;
    char buf[48];
    if (mins > 0 && secs > 0) {
        snprintf(buf, sizeof(buf), "~%d:%02d prep time", mins, secs);
    } else if (mins > 0) {
        snprintf(buf, sizeof(buf), "~%d min prep time", mins);
    } else {
        snprintf(buf, sizeof(buf), "~%d sec prep time", secs);
    }
    lv_subject_copy_string(s_detail_view_instance->get_prep_time_estimate_subject(), buf);
}

// ============================================================================
// Static Callbacks for Pre-print Switch Toggles (LT2 Phase 4)
// ============================================================================

static void on_preprint_bed_mesh_toggled(lv_event_t* e) {
    if (!s_detail_view_instance) {
        return;
    }
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lv_subject_set_int(s_detail_view_instance->get_preprint_bed_mesh_subject(), checked ? 1 : 0);
    spdlog::debug("[DetailView] Bed mesh toggled: {}", checked);
    update_prep_time_label();
}

static void on_preprint_qgl_toggled(lv_event_t* e) {
    if (!s_detail_view_instance) {
        return;
    }
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lv_subject_set_int(s_detail_view_instance->get_preprint_qgl_subject(), checked ? 1 : 0);
    spdlog::debug("[DetailView] QGL toggled: {}", checked);
    update_prep_time_label();
}

static void on_preprint_z_tilt_toggled(lv_event_t* e) {
    if (!s_detail_view_instance) {
        return;
    }
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lv_subject_set_int(s_detail_view_instance->get_preprint_z_tilt_subject(), checked ? 1 : 0);
    spdlog::debug("[DetailView] Z-tilt toggled: {}", checked);
    update_prep_time_label();
}

static void on_preprint_nozzle_clean_toggled(lv_event_t* e) {
    if (!s_detail_view_instance) {
        return;
    }
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lv_subject_set_int(s_detail_view_instance->get_preprint_nozzle_clean_subject(),
                       checked ? 1 : 0);
    spdlog::debug("[DetailView] Nozzle clean toggled: {}", checked);
    update_prep_time_label();
}

static void on_preprint_purge_line_toggled(lv_event_t* e) {
    if (!s_detail_view_instance) {
        return;
    }
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lv_subject_set_int(s_detail_view_instance->get_preprint_purge_line_subject(), checked ? 1 : 0);
    spdlog::debug("[DetailView] Purge line toggled: {}", checked);
    update_prep_time_label();
}

static void on_preprint_timelapse_toggled(lv_event_t* e) {
    if (!s_detail_view_instance) {
        return;
    }
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lv_subject_set_int(s_detail_view_instance->get_preprint_timelapse_subject(), checked ? 1 : 0);
    spdlog::debug("[DetailView] Timelapse toggled: {}", checked);
    update_prep_time_label();
}

} // namespace helix::ui
