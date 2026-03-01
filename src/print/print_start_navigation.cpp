// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_navigation.h"

#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"

#include "app_globals.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>

namespace helix {

// Track previous state to detect transitions TO printing
static PrintJobState prev_print_state = PrintJobState::STANDBY;

// Callback for print state changes - auto-navigates to print status on print start
static void on_print_state_changed_for_navigation(lv_observer_t* observer, lv_subject_t* subject) {
    (void)observer;
    auto current = static_cast<PrintJobState>(lv_subject_get_int(subject));

    spdlog::trace("[PrintStartNav] State change: {} -> {}", static_cast<int>(prev_print_state),
                  static_cast<int>(current));

    // Check for transition TO printing from a non-printing state
    bool was_not_printing =
        (prev_print_state != PrintJobState::PRINTING && prev_print_state != PrintJobState::PAUSED);
    bool is_now_printing = (current == PrintJobState::PRINTING);

    if (was_not_printing && is_now_printing) {
        // Don't auto-navigate while the setup wizard is running
        if (is_wizard_active()) {
            spdlog::debug(
                "[PrintStartNav] Wizard active, suppressing auto-navigation to print status");
            prev_print_state = current;
            return;
        }

        // A print just started - auto-navigate to print status from any panel
        // Use push_overlay() which handles lazy widget creation and duplicate-push guard
        spdlog::info("[PrintStartNav] Auto-navigating to print status (print started)");
        PrintStatusPanel::push_overlay(lv_display_get_screen_active(nullptr));
    }

    prev_print_state = current;
}

ObserverGuard init_print_start_navigation_observer() {
    // Initialize prev_print_state to current state to prevent false trigger on startup
    prev_print_state = static_cast<PrintJobState>(
        lv_subject_get_int(get_printer_state().get_print_state_enum_subject()));
    spdlog::debug("[PrintStartNav] Observer registered (initial state={})",
                  static_cast<int>(prev_print_state));
    return ObserverGuard(get_printer_state().get_print_state_enum_subject(),
                         on_print_state_changed_for_navigation, nullptr);
}

} // namespace helix
