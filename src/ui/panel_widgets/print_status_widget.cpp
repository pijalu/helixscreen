// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_status_widget.h"

#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"
#include "ui_update_queue.h"

#include "app_globals.h"
#include "filament_sensor_manager.h"
#include "observer_factory.h"
#include "panel_widget_registry.h"
#include "printer_state.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <cstdio>

namespace helix {
void register_print_status_widget() {
    register_widget_factory("print_status", []() { return std::make_unique<PrintStatusWidget>(); });
    // No init_subjects needed — this widget uses subjects owned by PrinterState

    // Register XML event callbacks at startup (before any XML is parsed)
    lv_xml_register_event_cb(nullptr, "print_card_clicked_cb", PrintStatusWidget::print_card_clicked_cb);
}
} // namespace helix

using namespace helix;

PrintStatusWidget::PrintStatusWidget() : printer_state_(get_printer_state()) {}

PrintStatusWidget::~PrintStatusWidget() {
    detach();
}

void PrintStatusWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    using helix::ui::observe_int_sync;
    using helix::ui::observe_print_state;
    using helix::ui::observe_string;

    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;

    // Store this pointer for event callback recovery
    lv_obj_set_user_data(widget_obj_, this);

    // Cache widget references from XML
    print_card_thumb_ = lv_obj_find_by_name(widget_obj_, "print_card_thumb");
    print_card_active_thumb_ = lv_obj_find_by_name(widget_obj_, "print_card_active_thumb");
    print_card_label_ = lv_obj_find_by_name(widget_obj_, "print_card_label");

    // Set up observers (after widget references are cached and widget_obj_ is set)
    print_state_observer_ =
        observe_print_state<PrintStatusWidget>(printer_state_.get_print_state_enum_subject(), this,
                                               [](PrintStatusWidget* self, PrintJobState state) {
                                                   if (!self->widget_obj_)
                                                       return;
                                                   self->on_print_state_changed(state);
                                               });

    print_progress_observer_ =
        observe_int_sync<PrintStatusWidget>(printer_state_.get_print_progress_subject(), this,
                                            [](PrintStatusWidget* self, int /*progress*/) {
                                                if (!self->widget_obj_)
                                                    return;
                                                self->on_print_progress_or_time_changed();
                                            });

    print_time_left_observer_ =
        observe_int_sync<PrintStatusWidget>(printer_state_.get_print_time_left_subject(), this,
                                            [](PrintStatusWidget* self, int /*time*/) {
                                                if (!self->widget_obj_)
                                                    return;
                                                self->on_print_progress_or_time_changed();
                                            });

    print_thumbnail_path_observer_ =
        observe_string<PrintStatusWidget>(printer_state_.get_print_thumbnail_path_subject(), this,
                                          [](PrintStatusWidget* self, const char* path) {
                                              if (!self->widget_obj_)
                                                  return;
                                              self->on_print_thumbnail_path_changed(path);
                                          });

    auto& fsm = helix::FilamentSensorManager::instance();
    filament_runout_observer_ = observe_int_sync<PrintStatusWidget>(
        fsm.get_any_runout_subject(), this, [](PrintStatusWidget* self, int any_runout) {
            if (!self->widget_obj_)
                return;
            spdlog::debug("[PrintStatusWidget] Filament runout subject changed: {}", any_runout);
            if (any_runout == 1) {
                self->check_and_show_idle_runout_modal();
            } else {
                self->runout_modal_shown_ = false;
            }
        });

    spdlog::debug("[PrintStatusWidget] Subscribed to print state/progress/time/thumbnail/runout");

    // Check initial print state
    if (print_card_thumb_ && print_card_active_thumb_ && print_card_label_) {
        auto state = static_cast<PrintJobState>(
            lv_subject_get_int(printer_state_.get_print_state_enum_subject()));
        if (state == PrintJobState::PRINTING || state == PrintJobState::PAUSED) {
            on_print_state_changed(state);
        }
        spdlog::debug("[PrintStatusWidget] Found print card widgets for dynamic updates");
    } else {
        spdlog::warn("[PrintStatusWidget] Could not find all print card widgets "
                     "(thumb={}, active_thumb={}, label={})",
                     print_card_thumb_ != nullptr, print_card_active_thumb_ != nullptr,
                     print_card_label_ != nullptr);
    }

    spdlog::debug("[PrintStatusWidget] Attached");
}

void PrintStatusWidget::detach() {
    // Release observers
    print_state_observer_.reset();
    print_progress_observer_.reset();
    print_time_left_observer_.reset();
    print_thumbnail_path_observer_.reset();
    filament_runout_observer_.reset();

    // Clear widget references
    print_card_thumb_ = nullptr;
    print_card_active_thumb_ = nullptr;
    print_card_label_ = nullptr;

    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
        widget_obj_ = nullptr;
    }
    parent_screen_ = nullptr;

    spdlog::debug("[PrintStatusWidget] Detached");
}

// ============================================================================
// Print Card Click Handler
// ============================================================================

void PrintStatusWidget::handle_print_card_clicked() {
    if (!printer_state_.can_start_new_print()) {
        // Print in progress - show print status overlay
        spdlog::info(
            "[PrintStatusWidget] Print card clicked - showing print status (print in progress)");

        if (!PrintStatusPanel::push_overlay(parent_screen_)) {
            spdlog::error("[PrintStatusWidget] Failed to push print status overlay");
        }
    } else {
        // No print in progress - navigate to print select panel
        spdlog::info("[PrintStatusWidget] Print card clicked - navigating to print select panel");
        NavigationManager::instance().set_active(PanelId::PrintSelect);
    }
}

// ============================================================================
// Observer Callbacks
// ============================================================================

void PrintStatusWidget::on_print_state_changed(PrintJobState state) {
    if (!widget_obj_ || !print_card_thumb_ || !print_card_label_) {
        return;
    }
    if (!lv_obj_is_valid(widget_obj_)) {
        return;
    }

    bool is_active = (state == PrintJobState::PRINTING || state == PrintJobState::PAUSED);

    if (is_active) {
        spdlog::debug("[PrintStatusWidget] Print active - updating card progress display");
        update_print_card_from_state();
    } else {
        spdlog::debug("[PrintStatusWidget] Print not active - reverting card to idle state");
        reset_print_card_to_idle();
    }
}

void PrintStatusWidget::on_print_progress_or_time_changed() {
    update_print_card_from_state();
}

void PrintStatusWidget::on_print_thumbnail_path_changed(const char* /*path*/) {
    if (!widget_obj_ || !print_card_active_thumb_) {
        return;
    }

    // Already deferred via observe_string's queue_update — safe to update directly.
    const char* current_path =
        lv_subject_get_string(printer_state_.get_print_thumbnail_path_subject());

    if (current_path && current_path[0] != '\0') {
        lv_image_set_src(print_card_active_thumb_, current_path);
        spdlog::debug("[PrintStatusWidget] Active print thumbnail updated: {}", current_path);
    } else {
        lv_image_set_src(print_card_active_thumb_, "A:assets/images/benchy_thumbnail_white.png");
        spdlog::debug("[PrintStatusWidget] Active print thumbnail cleared");
    }
}

void PrintStatusWidget::update_print_card_from_state() {
    auto state = static_cast<PrintJobState>(
        lv_subject_get_int(printer_state_.get_print_state_enum_subject()));

    // Only update if actively printing
    if (state != PrintJobState::PRINTING && state != PrintJobState::PAUSED) {
        return;
    }

    int progress = lv_subject_get_int(printer_state_.get_print_progress_subject());
    int time_left = lv_subject_get_int(printer_state_.get_print_time_left_subject());

    update_print_card_label(progress, time_left);
}

void PrintStatusWidget::update_print_card_label(int progress, int time_left_secs) {
    if (!print_card_label_ || !lv_obj_is_valid(print_card_label_)) {
        return;
    }

    char buf[64];
    int hours = time_left_secs / 3600;
    int minutes = (time_left_secs % 3600) / 60;

    if (hours > 0) {
        std::snprintf(buf, sizeof(buf), "%d%% \u2022 %dh %02dm left", progress, hours, minutes);
    } else if (minutes > 0) {
        std::snprintf(buf, sizeof(buf), "%d%% \u2022 %dm left", progress, minutes);
    } else {
        std::snprintf(buf, sizeof(buf), "%d%% \u2022 < 1m left", progress);
    }

    lv_label_set_text(print_card_label_, buf);
}

void PrintStatusWidget::reset_print_card_to_idle() {
    if (print_card_thumb_ && lv_obj_is_valid(print_card_thumb_)) {
        lv_image_set_src(print_card_thumb_, "A:assets/images/benchy_thumbnail_white.png");
    }
    if (print_card_label_ && lv_obj_is_valid(print_card_label_)) {
        lv_label_set_text(print_card_label_, "Print Files");
    }
}

// ============================================================================
// Filament Runout Modal
// ============================================================================

void PrintStatusWidget::check_and_show_idle_runout_modal() {
    // Grace period - don't show modal during startup
    auto& fsm = helix::FilamentSensorManager::instance();
    if (fsm.is_in_startup_grace_period()) {
        spdlog::debug("[PrintStatusWidget] In startup grace period - skipping runout modal");
        return;
    }

    // Verify actual sensor state
    if (!fsm.has_any_runout()) {
        spdlog::debug("[PrintStatusWidget] No actual runout detected - skipping modal");
        return;
    }

    // Check suppression logic (AMS without bypass, wizard active, etc.)
    if (!get_runtime_config()->should_show_runout_modal()) {
        spdlog::debug("[PrintStatusWidget] Runout modal suppressed by runtime config");
        return;
    }

    // Only show modal if not already shown
    if (runout_modal_shown_) {
        spdlog::debug("[PrintStatusWidget] Runout modal already shown - skipping");
        return;
    }

    // Only show if printer is idle (not printing/paused)
    int print_state = lv_subject_get_int(printer_state_.get_print_state_enum_subject());
    if (print_state != static_cast<int>(PrintJobState::STANDBY) &&
        print_state != static_cast<int>(PrintJobState::COMPLETE) &&
        print_state != static_cast<int>(PrintJobState::CANCELLED)) {
        spdlog::debug("[PrintStatusWidget] Print active (state={}) - skipping idle runout modal",
                      print_state);
        return;
    }

    spdlog::info("[PrintStatusWidget] Showing idle runout modal");
    show_idle_runout_modal();
    runout_modal_shown_ = true;
}

void PrintStatusWidget::trigger_idle_runout_check() {
    spdlog::debug("[PrintStatusWidget] Triggering deferred runout check");
    runout_modal_shown_ = false;
    check_and_show_idle_runout_modal();
}

void PrintStatusWidget::show_idle_runout_modal() {
    if (runout_modal_.is_visible()) {
        return;
    }

    runout_modal_.set_on_load_filament([this]() {
        spdlog::info("[PrintStatusWidget] User chose to load filament (idle)");
        NavigationManager::instance().set_active(PanelId::Filament);
    });

    runout_modal_.set_on_resume([]() {
        // Resume not applicable when idle
    });

    runout_modal_.set_on_cancel_print([]() {
        // Cancel not applicable when idle
    });

    runout_modal_.show(parent_screen_);
}

// ============================================================================
// Static Trampolines
// ============================================================================

void PrintStatusWidget::print_card_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintStatusWidget] print_card_clicked_cb");

    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto* self = static_cast<PrintStatusWidget*>(lv_obj_get_user_data(target));
    if (self) {
        self->handle_print_card_clicked();
    } else {
        spdlog::warn(
            "[PrintStatusWidget] print_card_clicked_cb: could not recover widget instance");
    }

    LVGL_SAFE_EVENT_CB_END();
}
