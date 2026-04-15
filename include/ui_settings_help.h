// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_help.h
 * @brief Help & About overlay - debug bundle, Discord, docs, about
 *
 * This overlay provides quick access to:
 * - Debug bundle upload for support
 * - Discord community link
 * - Documentation link
 * - About/version info overlay
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see AboutSettingsOverlay for version/update details
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

/**
 * @class HelpSettingsOverlay
 * @brief Overlay for help, support, and about actions
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_help_settings_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class HelpSettingsOverlay : public OverlayBase {
  public:
    HelpSettingsOverlay();
    ~HelpSettingsOverlay() override;

    // Non-copyable
    HelpSettingsOverlay(const HelpSettingsOverlay&) = delete;
    HelpSettingsOverlay& operator=(const HelpSettingsOverlay&) = delete;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Help & About";
    }

    void on_activate() override;
    void on_deactivate() override;

    //
    // === UI Creation ===
    //

    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);

    bool is_created() const {
        return overlay_root_ != nullptr;
    }

  private:
    //
    // === Static Callbacks ===
    //

    static void on_replay_tour_clicked(lv_event_t* e);
    static void on_debug_bundle_clicked(lv_event_t* e);
    static void on_discord_clicked(lv_event_t* e);
    static void on_docs_clicked(lv_event_t* e);
    static void on_about_clicked(lv_event_t* e);
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton HelpSettingsOverlay
 */
HelpSettingsOverlay& get_help_settings_overlay();

} // namespace helix::settings
