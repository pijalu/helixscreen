// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_fans.h
 * @brief Fan Settings overlay - view and rename discovered fans
 *
 * This overlay displays all discovered fans grouped into two sections:
 * - Controllable fans (part cooling, fan_generic, output_pin)
 * - Auto fans (heater_fan, controller_fan, temperature_fan)
 *
 * Each fan row shows the display name, Moonraker object name, type pill,
 * and current speed. Tapping a fan name opens a rename dialog that persists
 * the custom name via Config.
 *
 * @pattern Overlay (lazy init, dynamic row creation)
 * @threading Main thread only
 *
 * @see PrinterFanState for fan discovery and state
 * @see Config for name persistence
 */

#pragma once

#include "overlay_base.h"

#include <lvgl.h>
#include <string>

namespace helix::settings {

/**
 * @class FanSettingsOverlay
 * @brief Overlay for viewing and renaming discovered fans
 *
 * ## Dynamic Row Creation:
 *
 * Fan rows are created dynamically at runtime using lv_xml_create().
 * This requires using lv_obj_add_event_cb() for the rename click
 * callbacks, which is an acceptable exception to the declarative UI rule.
 */
class FanSettingsOverlay : public OverlayBase {
  public:
    FanSettingsOverlay();
    ~FanSettingsOverlay() override;

    // Non-copyable
    FanSettingsOverlay(const FanSettingsOverlay&) = delete;
    FanSettingsOverlay& operator=(const FanSettingsOverlay&) = delete;

    /// Register event callbacks with lv_xml system
    void register_callbacks() override;

    /// Create the overlay UI (called lazily)
    lv_obj_t* create(lv_obj_t* parent) override;

    /// Show the overlay (populates fan lists first)
    void show(lv_obj_t* parent_screen);

    const char* get_name() const override {
        return "Fans";
    }

    void init_subjects() override {}

    /// Called when overlay becomes visible — repopulates fan lists
    void on_activate() override;

    /// Called when overlay is being hidden
    void on_deactivate() override;

    /// Handle fan rename via keyboard modal (callable from any overlay)
    void handle_fan_rename(const std::string& object_name, const std::string& current_name);

    /// Confirm the rename (called from static callback)
    void confirm_rename();

    /// Cancel/hide the rename modal
    void cancel_rename();

  private:
    /// Populate all fan sections from PrinterFanState
    void populate_fans();

    /// Populate a specific fan list container
    void populate_fan_list(lv_obj_t* list, bool controllable);

    /// Update badge count for a section
    void update_section_count(const char* badge_name, size_t count);

    lv_obj_t* controllable_list_ = nullptr;
    lv_obj_t* auto_list_ = nullptr;
    lv_obj_t* no_fans_placeholder_ = nullptr;

    // Rename modal state
    lv_obj_t* rename_modal_ = nullptr;
    std::string pending_rename_object_;
    char rename_old_name_buf_[64] = {};
    lv_subject_t fan_rename_old_name_{}; ///< Subject for bind_text in modal
    bool rename_subject_initialized_ = false;
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton FanSettingsOverlay
 */
FanSettingsOverlay& get_fan_settings_overlay();

} // namespace helix::settings
