// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_macros.h"

#include "macro_executor.h"
#include "ui_error_reporting.h"
#include "ui_event_safety.h"
#include "ui_global_panel_helper.h"
#include "ui_nav_manager.h"
#include "ui_panel_common.h"
#include "ui_subject_registry.h"
#include "ui_utils.h"

#include "app_globals.h"
#include "device_display_name.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "printer_state.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "ui_modal.h"
#include "ui_update_queue.h"

#include "macro_param_cache.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

// ============================================================================
// Global Instance
// ============================================================================

DEFINE_GLOBAL_PANEL(MacrosPanel, g_macros_panel, get_global_macros_panel)

// ============================================================================
// Constructor
// ============================================================================

MacrosPanel::MacrosPanel() {
    std::snprintf(status_buf_, sizeof(status_buf_), "Loading macros...");
    spdlog::debug("[MacrosPanel] Instance created");
}

MacrosPanel::~MacrosPanel() {
    deinit_subjects();
}

// ============================================================================
// Subject Initialization
// ============================================================================

void MacrosPanel::init_subjects() {
    init_subjects_guarded([this]() {
        // Initialize status subject for reactive binding
        UI_MANAGED_SUBJECT_STRING(status_subject_, status_buf_, status_buf_, "macros_status",
                                  subjects_);
    });
}

void MacrosPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

// ============================================================================
// Callback Registration
// ============================================================================

void MacrosPanel::register_callbacks() {
    if (callbacks_registered_) {
        spdlog::debug("[{}] Callbacks already registered", get_name());
        return;
    }

    spdlog::debug("[{}] Registering event callbacks", get_name());

    // Register XML event callback
    lv_xml_register_event_cb(nullptr, "on_macro_card_clicked", on_macro_card_clicked);

    callbacks_registered_ = true;
    spdlog::debug("[{}] Event callbacks registered", get_name());
}

// ============================================================================
// Create
// ============================================================================

lv_obj_t* MacrosPanel::create(lv_obj_t* parent) {
    if (!create_overlay_from_xml(parent, "macro_panel")) {
        return nullptr;
    }

    // Find widget references
    lv_obj_t* overlay_content = lv_obj_find_by_name(overlay_root_, "overlay_content");
    if (overlay_content) {
        macro_list_container_ = lv_obj_find_by_name(overlay_content, "macro_list");
        empty_state_container_ = lv_obj_find_by_name(overlay_content, "empty_state");
        status_label_ = lv_obj_find_by_name(overlay_content, "status_message");
        system_toggle_ = lv_obj_find_by_name(overlay_content, "show_system_toggle");
    }

    if (!macro_list_container_) {
        spdlog::error("[{}] macro_list container not found!", get_name());
        return nullptr;
    }

    // Populate macros from capabilities
    populate_macro_list();

    spdlog::info("[{}] Overlay created successfully", get_name());
    return overlay_root_;
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void MacrosPanel::on_activate() {
    // Call base class first
    OverlayBase::on_activate();
    *alive_ = true;

    spdlog::debug("[{}] on_activate()", get_name());

    // Refresh macro list when panel becomes visible
    populate_macro_list();
}

void MacrosPanel::on_deactivate() {
    spdlog::debug("[{}] on_deactivate()", get_name());
    *alive_ = false;

    // Call base class
    OverlayBase::on_deactivate();
}

// ============================================================================
// Macro List Management
// ============================================================================

void MacrosPanel::clear_macro_list() {
    if (macro_list_container_) {
        lv_obj_clean(macro_list_container_);
    }
    macro_entries_.clear();
}

void MacrosPanel::populate_macro_list() {
    clear_macro_list();

    // Get macros from capabilities
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No MoonrakerAPI available", get_name());
        std::snprintf(status_buf_, sizeof(status_buf_), "%s", lv_tr("Not connected to printer"));
        lv_subject_copy_string(&status_subject_, status_buf_);
        return;
    }

    const auto& macros = api->hardware().macros();

    // Sort macros alphabetically for consistent display
    std::vector<std::string> sorted_macros(macros.begin(), macros.end());
    std::sort(sorted_macros.begin(), sorted_macros.end());

    // Filter and create cards
    int visible_count = 0;
    for (const auto& macro_name : sorted_macros) {
        // Skip system macros if not showing them
        bool is_system = !macro_name.empty() && macro_name[0] == '_';
        if (is_system && !show_system_macros_) {
            continue;
        }

        create_macro_card(macro_name);
        visible_count++;
    }

    // Toggle visibility: show macro list OR empty state
    bool has_macros = visible_count > 0;
    helix::ui::toggle_list_empty_state(macro_list_container_, empty_state_container_, has_macros);

    // Update status
    if (has_macros) {
        status_buf_[0] = '\0'; // Clear status when macros are present
    } else {
        std::snprintf(status_buf_, sizeof(status_buf_), "%s", lv_tr("No macros found"));
    }
    lv_subject_copy_string(&status_subject_, status_buf_);

    spdlog::info("[{}] Displayed {} macros ({} total in capabilities)", get_name(), visible_count,
                 macros.size());
}

void MacrosPanel::create_macro_card(const std::string& macro_name) {
    if (!macro_list_container_) {
        return;
    }

    // Prettify the macro name for display
    std::string display_name = prettify_macro_name(macro_name);

    // Look up description from cache
    auto cached = helix::MacroParamCache::instance().get(macro_name);
    bool has_desc = !cached.description.empty();

    // Create card using XML component
    const char* attrs[] = {
        "macro_name",        display_name.c_str(),
        "macro_description", has_desc ? cached.description.c_str() : "",
        "hide_description",  has_desc ? "false" : "true",
        nullptr,             nullptr};
    lv_obj_t* card =
        static_cast<lv_obj_t*>(lv_xml_create(macro_list_container_, "macro_card", attrs));

    if (!card) {
        spdlog::error("[{}] Failed to create macro_card for '{}'", get_name(), macro_name);
        return;
    }

    bool is_dangerous = helix::is_dangerous_macro(macro_name);

    // Store entry info -- card pointer used for lookup in click callback
    MacroEntry entry;
    entry.card = card;
    entry.name = macro_name;
    entry.display_name = display_name;
    entry.is_system = !macro_name.empty() && macro_name[0] == '_';
    entry.is_dangerous = is_dangerous;
    macro_entries_.push_back(entry);

    spdlog::debug("[{}] Created card for macro '{}' (dangerous: {})", get_name(), macro_name,
                  is_dangerous);
}

std::string MacrosPanel::prettify_macro_name(const std::string& name) {
    return helix::get_display_name(name, helix::DeviceType::MACRO);
}

void MacrosPanel::execute_macro(const std::string& macro_name) {
    execute_with_params(macro_name, {});
}

void MacrosPanel::fetch_params_and_execute(const std::string& macro_name) {
    MoonrakerAPI* api = get_moonraker_api();
    if (!api) {
        spdlog::warn("[{}] No MoonrakerAPI available - cannot fetch params", get_name());
        return;
    }

    bool dangerous = helix::is_dangerous_macro(macro_name);

    // For dangerous macros, show confirmation before doing anything else
    if (dangerous) {
        spdlog::warn("[{}] Dangerous macro requested: {}", get_name(), macro_name);

        // Store pending macro name for the confirmation callback
        pending_dangerous_macro_ = macro_name;

        std::string msg = macro_name + " may cause unintended changes. Are you sure?";
        helix::ui::modal_show_confirmation(
            lv_tr("Run Dangerous Macro?"), msg.c_str(), ModalSeverity::Warning, lv_tr("Run"),
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[MacrosPanel] dangerous_confirm_cb");
                auto* self = static_cast<MacrosPanel*>(lv_event_get_user_data(e));
                std::string macro = self->pending_dangerous_macro_;
                self->pending_dangerous_macro_.clear();
                Modal::hide(Modal::get_top());
                self->fetch_params_and_run(macro);
                LVGL_SAFE_EVENT_CB_END();
            },
            [](lv_event_t* e) {
                LVGL_SAFE_EVENT_CB_BEGIN("[MacrosPanel] dangerous_cancel_cb");
                auto* self = static_cast<MacrosPanel*>(lv_event_get_user_data(e));
                self->pending_dangerous_macro_.clear();
                Modal::hide(Modal::get_top());
                spdlog::debug("[MacrosPanel] Dangerous macro cancelled");
                LVGL_SAFE_EVENT_CB_END();
            },
            this);
        return;
    }

    fetch_params_and_run(macro_name);
}

void MacrosPanel::fetch_params_and_run(const std::string& macro_name) {
    auto cached = helix::MacroParamCache::instance().get(macro_name);

    switch (cached.knowledge) {
    case helix::MacroParamKnowledge::KNOWN_NO_PARAMS:
        execute_macro(macro_name);
        break;
    case helix::MacroParamKnowledge::KNOWN_PARAMS: {
        std::weak_ptr<bool> weak = alive_;
        std::string name = macro_name;
        param_modal_.show_for_macro(
            lv_screen_active(), macro_name, cached.params,
            [this, weak, name](const helix::MacroParamResult& result) {
                if (weak.expired())
                    return;
                execute_with_params(name, result);
            });
        break;
    }
    case helix::MacroParamKnowledge::UNKNOWN: {
        std::weak_ptr<bool> weak = alive_;
        std::string name = macro_name;
        param_modal_.show_for_unknown_params(
            lv_screen_active(), macro_name,
            [this, weak, name](const helix::MacroParamResult& result) {
                if (weak.expired())
                    return;
                execute_with_params(name, result);
            });
        break;
    }
    }
}

void MacrosPanel::execute_with_params(const std::string& macro_name,
                                      const helix::MacroParamResult& result) {
    MoonrakerAPI* api = get_moonraker_api();
    helix::execute_macro_gcode(api, macro_name, result, "[MacrosPanel]");
}

void MacrosPanel::set_show_system_macros(bool show_system) {
    if (show_system_macros_ != show_system) {
        show_system_macros_ = show_system;
        populate_macro_list(); // Refresh list
    }
}

// ============================================================================
// Static Callbacks
// ============================================================================

void MacrosPanel::on_macro_card_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacrosPanel] on_macro_card_clicked");

    auto& self = get_global_macros_panel();

    // Use current_target (the object the callback is registered on = macro_card root),
    // NOT target (which could be a child icon/label). L069: never assume user_data
    // ownership on XML-created objects — lv_button sets its own user_data internally.
    lv_obj_t* card = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (!card) {
        spdlog::warn("[MacrosPanel] No target in click event");
    } else {
        // Find macro entry by matching card pointer
        for (const auto& entry : self.macro_entries_) {
            if (entry.card == card) {
                self.fetch_params_and_execute(entry.name);
                break;
            }
        }
    }

    LVGL_SAFE_EVENT_CB_END();
}
