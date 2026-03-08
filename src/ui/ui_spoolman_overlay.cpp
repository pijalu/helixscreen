// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_spoolman_overlay.cpp
 * @brief Implementation of SpoolmanOverlay
 */

#include "ui_spoolman_overlay.h"

#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_settings_label_printer.h"
#include "ui_update_queue.h"

#include "ams_state.h"
#include "moonraker_api.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::ui {

// Database keys for settings persistence
static constexpr const char* DB_NAMESPACE = "helix-screen";
static constexpr const char* DB_KEY_SYNC_ENABLED = "spoolman_sync_enabled";
static constexpr const char* DB_KEY_REFRESH_INTERVAL = "spoolman_weight_refresh_interval";

// Legacy keys (pre-rename migration)
static constexpr const char* LEGACY_DB_KEY_SYNC_ENABLED = "ams_spoolman_sync_enabled";
static constexpr const char* LEGACY_DB_KEY_REFRESH_INTERVAL = "ams_weight_refresh_interval";

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<SpoolmanOverlay> g_spoolman_overlay;

SpoolmanOverlay& get_spoolman_overlay() {
    if (!g_spoolman_overlay) {
        g_spoolman_overlay = std::make_unique<SpoolmanOverlay>();
        StaticPanelRegistry::instance().register_destroy("SpoolmanOverlay",
                                                         []() { g_spoolman_overlay.reset(); });
    }
    return *g_spoolman_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

SpoolmanOverlay::SpoolmanOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

SpoolmanOverlay::~SpoolmanOverlay() {
    if (subjects_initialized_ && lv_is_initialized()) {
        lv_subject_deinit(&sync_enabled_subject_);
        lv_subject_deinit(&refresh_interval_subject_);
    }
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void SpoolmanOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Initialize sync enabled subject (default: true/enabled)
    lv_subject_init_int(&sync_enabled_subject_, DEFAULT_SYNC_ENABLED ? 1 : 0);
    lv_xml_register_subject(nullptr, "ams_spoolman_sync_enabled", &sync_enabled_subject_);

    // Initialize refresh interval subject (default: 30 seconds)
    lv_subject_init_int(&refresh_interval_subject_, DEFAULT_REFRESH_INTERVAL_SECONDS);
    lv_xml_register_subject(nullptr, "ams_spoolman_refresh_interval", &refresh_interval_subject_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void SpoolmanOverlay::register_callbacks() {
    // Register sync toggle callback
    lv_xml_register_event_cb(nullptr, "on_ams_spoolman_sync_toggled", on_sync_toggled);

    // Register interval dropdown callback
    lv_xml_register_event_cb(nullptr, "on_ams_spoolman_interval_changed", on_interval_changed);

    // Label printer sub-panel launcher
    lv_xml_register_event_cb(nullptr, "on_spoolman_label_printer_clicked", on_label_printer_clicked);

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* SpoolmanOverlay::create(lv_obj_t* parent) {
    if (overlay_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    // Create from XML component
    overlay_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "spoolman_settings", nullptr));
    if (!overlay_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Find control widgets for programmatic access
    sync_toggle_ = lv_obj_find_by_name(overlay_, "sync_toggle");
    interval_dropdown_ = lv_obj_find_by_name(overlay_, "interval_dropdown");

    // Initially hidden until show() pushes it
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_;
}

void SpoolmanOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Lazy create overlay
    if (!overlay_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    // Load settings from database
    load_from_database();

    // Update UI controls to match subject values
    update_ui_from_subjects();

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_, this);

    // Register close callback to destroy widget tree when overlay closes
    NavigationManager::instance().register_overlay_close_callback(overlay_, [this]() {
        // overlay_ is an alias for overlay_root_, so pass it directly
        destroy_overlay_ui(overlay_);
    });

    // Push onto navigation stack
    NavigationManager::instance().push_overlay(overlay_);
}

void SpoolmanOverlay::refresh() {
    if (!overlay_) {
        return;
    }

    load_from_database();
    update_ui_from_subjects();
}

// ============================================================================
// DATABASE OPERATIONS
// ============================================================================

void SpoolmanOverlay::load_from_database() {
    if (!api_) {
        spdlog::warn("[{}] No API available, using default values", get_name());
        return;
    }

    // Helper: apply sync_enabled value and update polling
    auto apply_sync = [this](bool enabled) {
        lv_subject_set_int(&sync_enabled_subject_, enabled ? 1 : 0);
        spdlog::debug("[{}] Loaded sync_enabled={} from database", get_name(), enabled);
        if (enabled) {
            AmsState::instance().start_spoolman_polling();
        } else {
            AmsState::instance().stop_spoolman_polling();
        }
    };

    auto parse_sync = [](const nlohmann::json& value, bool default_val) {
        if (value.is_boolean())
            return value.get<bool>();
        if (value.is_number())
            return value.get<int>() != 0;
        return default_val;
    };

    // Load sync enabled — try new key, fall back to legacy key
    api_->database_get_item(
        DB_NAMESPACE, DB_KEY_SYNC_ENABLED,
        [this, apply_sync, parse_sync](const nlohmann::json& value) {
            apply_sync(parse_sync(value, DEFAULT_SYNC_ENABLED));
        },
        [this, apply_sync, parse_sync](const MoonrakerError&) {
            // New key not found — try legacy key
            api_->database_get_item(
                DB_NAMESPACE, LEGACY_DB_KEY_SYNC_ENABLED,
                [this, apply_sync, parse_sync](const nlohmann::json& value) {
                    bool enabled = parse_sync(value, DEFAULT_SYNC_ENABLED);
                    apply_sync(enabled);
                    // Migrate to new key
                    save_sync_enabled(enabled);
                    spdlog::info("[{}] Migrated {} -> {}", get_name(), LEGACY_DB_KEY_SYNC_ENABLED,
                                 DB_KEY_SYNC_ENABLED);
                },
                [this, apply_sync](const MoonrakerError&) {
                    apply_sync(DEFAULT_SYNC_ENABLED);
                });
        });

    // Helper: apply refresh interval
    auto apply_interval = [this](int interval) {
        lv_subject_set_int(&refresh_interval_subject_, interval);
        spdlog::debug("[{}] Loaded refresh_interval={} from database", get_name(), interval);
    };

    auto parse_interval = [](const nlohmann::json& value, int default_val) {
        return value.is_number() ? value.get<int>() : default_val;
    };

    // Load refresh interval — try new key, fall back to legacy key
    api_->database_get_item(
        DB_NAMESPACE, DB_KEY_REFRESH_INTERVAL,
        [this, apply_interval, parse_interval](const nlohmann::json& value) {
            apply_interval(parse_interval(value, DEFAULT_REFRESH_INTERVAL_SECONDS));
        },
        [this, apply_interval, parse_interval](const MoonrakerError&) {
            // New key not found — try legacy key
            api_->database_get_item(
                DB_NAMESPACE, LEGACY_DB_KEY_REFRESH_INTERVAL,
                [this, apply_interval, parse_interval](const nlohmann::json& value) {
                    int interval = parse_interval(value, DEFAULT_REFRESH_INTERVAL_SECONDS);
                    apply_interval(interval);
                    // Migrate to new key
                    save_refresh_interval(interval);
                    spdlog::info("[{}] Migrated {} -> {}", get_name(),
                                 LEGACY_DB_KEY_REFRESH_INTERVAL, DB_KEY_REFRESH_INTERVAL);
                },
                [this, apply_interval](const MoonrakerError&) {
                    apply_interval(DEFAULT_REFRESH_INTERVAL_SECONDS);
                });
        });
}

void SpoolmanOverlay::save_sync_enabled(bool enabled) {
    if (!api_) {
        spdlog::warn("[{}] No API available, cannot save setting", get_name());
        return;
    }

    api_->database_post_item(
        DB_NAMESPACE, DB_KEY_SYNC_ENABLED, enabled,
        [this, enabled]() {
            spdlog::info("[{}] Saved sync_enabled={} to database", get_name(), enabled);
        },
        [this](const MoonrakerError& err) {
            spdlog::error("[{}] Failed to save sync_enabled: {}", get_name(), err.message);
        });
}

void SpoolmanOverlay::save_refresh_interval(int interval_seconds) {
    if (!api_) {
        spdlog::warn("[{}] No API available, cannot save setting", get_name());
        return;
    }

    api_->database_post_item(
        DB_NAMESPACE, DB_KEY_REFRESH_INTERVAL, interval_seconds,
        [this, interval_seconds]() {
            spdlog::info("[{}] Saved refresh_interval={} to database", get_name(),
                         interval_seconds);
        },
        [this](const MoonrakerError& err) {
            spdlog::error("[{}] Failed to save refresh_interval: {}", get_name(), err.message);
        });
}

// ============================================================================
// UTILITY METHODS
// ============================================================================

int SpoolmanOverlay::dropdown_index_to_seconds(int index) {
    // Dropdown options: "30s", "1 min", "2 min", "5 min"
    switch (index) {
    case 0:
        return 30;
    case 1:
        return 60;
    case 2:
        return 120;
    case 3:
        return 300;
    default:
        return 30;
    }
}

int SpoolmanOverlay::seconds_to_dropdown_index(int seconds) {
    switch (seconds) {
    case 30:
        return 0;
    case 60:
        return 1;
    case 120:
        return 2;
    case 300:
        return 3;
    default:
        return 0; // Default to 30s
    }
}

void SpoolmanOverlay::update_ui_from_subjects() {
    // Update dropdown to match current interval
    if (interval_dropdown_) {
        int interval_seconds = lv_subject_get_int(&refresh_interval_subject_);
        int dropdown_index = seconds_to_dropdown_index(interval_seconds);
        lv_dropdown_set_selected(interval_dropdown_, static_cast<uint32_t>(dropdown_index));
    }

    // Toggle state is handled by subject binding in XML
}

void SpoolmanOverlay::on_ui_destroyed() {
    sync_toggle_ = nullptr;
    interval_dropdown_ = nullptr;
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void SpoolmanOverlay::on_sync_toggled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_sync_toggled");

    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!toggle || !lv_obj_is_valid(toggle)) {
        spdlog::warn("[SpoolmanOverlay] Stale callback - toggle no longer valid");
    } else {
        bool is_checked = lv_obj_has_state(toggle, LV_STATE_CHECKED);

        spdlog::info("[SpoolmanOverlay] Sync toggle: {}", is_checked ? "enabled" : "disabled");

        // Update subject
        auto& overlay = get_spoolman_overlay();
        lv_subject_set_int(&overlay.sync_enabled_subject_, is_checked ? 1 : 0);

        // Save to database
        overlay.save_sync_enabled(is_checked);

        // Update AmsState polling
        if (is_checked) {
            AmsState::instance().start_spoolman_polling();
        } else {
            AmsState::instance().stop_spoolman_polling();
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}

void SpoolmanOverlay::on_interval_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_interval_changed");

    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (!dropdown || !lv_obj_is_valid(dropdown)) {
        spdlog::warn("[SpoolmanOverlay] Stale callback - dropdown no longer valid");
    } else {
        int selected = static_cast<int>(lv_dropdown_get_selected(dropdown));
        int interval_seconds = dropdown_index_to_seconds(selected);

        spdlog::info("[SpoolmanOverlay] Interval changed: {}s", interval_seconds);

        // Update subject
        auto& overlay = get_spoolman_overlay();
        lv_subject_set_int(&overlay.refresh_interval_subject_, interval_seconds);

        // Save to database
        overlay.save_refresh_interval(interval_seconds);

        // Note: The actual polling interval in AmsState is currently fixed at 30s.
        // This setting is stored for future use when configurable polling is implemented.
        // For now, we just persist the user's preference.
    }

    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// LABEL PRINTER SUB-PANEL
// ============================================================================

void SpoolmanOverlay::on_label_printer_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SpoolmanOverlay] on_label_printer_clicked");
    auto& overlay = helix::settings::get_label_printer_settings_overlay();
    auto& spoolman = get_spoolman_overlay();
    overlay.show(spoolman.parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
