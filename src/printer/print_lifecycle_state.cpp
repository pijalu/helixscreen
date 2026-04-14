// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_lifecycle_state.h"

#include "printer_state.h"

#include <spdlog/spdlog.h>

static const char* print_state_name(PrintState s) {
    switch (s) {
    case PrintState::Idle:
        return "Idle";
    case PrintState::Preparing:
        return "Preparing";
    case PrintState::Printing:
        return "Printing";
    case PrintState::Paused:
        return "Paused";
    case PrintState::Complete:
        return "Complete";
    case PrintState::Cancelled:
        return "Cancelled";
    case PrintState::Error:
        return "Error";
    }
    return "Unknown";
}

StateChangeResult PrintLifecycleState::on_job_state_changed(helix::PrintJobState job_state,
                                                            helix::PrintOutcome /* outcome */) {
    // Map PrintJobState to PrintState
    PrintState new_state;
    switch (job_state) {
    case helix::PrintJobState::STANDBY:
        new_state = PrintState::Idle;
        break;
    case helix::PrintJobState::PRINTING:
        new_state = PrintState::Printing;
        break;
    case helix::PrintJobState::PAUSED:
        new_state = PrintState::Paused;
        break;
    case helix::PrintJobState::COMPLETE:
        new_state = PrintState::Complete;
        break;
    case helix::PrintJobState::CANCELLED:
        new_state = PrintState::Cancelled;
        break;
    case helix::PrintJobState::ERROR:
        new_state = PrintState::Error;
        break;
    default:
        new_state = PrintState::Idle;
        break;
    }

    if (new_state == current_state_) {
        spdlog::trace("[PrintLifecycleState] state unchanged: {}", print_state_name(new_state));
        StateChangeResult unchanged{};
        unchanged.old_state = current_state_;
        unchanged.new_state = current_state_;
        return unchanged;
    }

    spdlog::debug("[PrintLifecycleState] state transition: {} -> {}",
                  print_state_name(current_state_), print_state_name(new_state));

    // Compute derived booleans
    bool going_idle = (new_state == PrintState::Idle);
    // print_ended fires only on transition to Idle — that's when Moonraker sends
    // Standby after a terminal state. Resources (thumbnail, gcode, viewer) persist
    // through Complete/Cancelled/Error so the user can see final state.
    bool print_ended = going_idle;

    bool should_reset_progress_bar =
        (new_state == PrintState::Printing && current_state_ != PrintState::Paused);
    bool should_clear_excluded_objects =
        (new_state == PrintState::Printing && current_state_ != PrintState::Paused);
    bool should_freeze_complete = (new_state == PrintState::Complete);
    bool should_animate_cancelled = (new_state == PrintState::Cancelled);
    bool should_animate_error = (new_state == PrintState::Error);

    // Handle gcode_loaded:
    // Keep on terminal states (Complete/Cancelled/Error) so viewer stays visible.
    // Clear on transition to Idle (print_ended) so next print starts clean.
    bool clear_gcode_loaded = print_ended;

    // Compute viewer visibility — show during active print AND terminal states
    // so the user can see where the print stopped (cancelled/errored/completed).
    // Hide only in Idle.
    bool want = (new_state != PrintState::Idle);
    bool should_show_viewer = want && gcode_loaded_;

    // Apply freeze for complete state
    if (should_freeze_complete) {
        current_progress_ = 100;
        if (total_layers_ > 0) {
            current_layer_ = total_layers_;
        }
        remaining_seconds_ = 0;
        // elapsed_seconds_ is frozen at its current value
        spdlog::debug("[PrintLifecycleState] frozen complete: progress=100, layer={}/{}, "
                      "remaining=0, elapsed={}",
                      current_layer_, total_layers_, elapsed_seconds_);
    }

    // Clear gcode loaded if needed
    if (clear_gcode_loaded) {
        gcode_loaded_ = false;
        spdlog::trace("[PrintLifecycleState] cleared gcode_loaded");
    }

    // Store old state and update
    PrintState old_state = current_state_;
    current_state_ = new_state;

    StateChangeResult result{};
    result.state_changed = true;
    result.print_ended = print_ended;
    result.should_reset_progress_bar = should_reset_progress_bar;
    result.should_clear_excluded_objects = should_clear_excluded_objects;
    result.should_freeze_complete = should_freeze_complete;
    result.should_animate_cancelled = should_animate_cancelled;
    result.should_animate_error = should_animate_error;
    result.clear_gcode_loaded = clear_gcode_loaded;
    result.old_state = old_state;
    result.new_state = new_state;
    result.should_show_viewer = should_show_viewer;
    return result;
}

bool PrintLifecycleState::on_progress_changed(int progress) {
    if (!is_active()) {
        return false;
    }
    current_progress_ = std::clamp(progress, 0, 100);
    return true;
}

bool PrintLifecycleState::on_layer_changed(int layer, int total, bool /* has_real_data */) {
    if (!is_active()) {
        return false;
    }
    current_layer_ = layer;
    total_layers_ = total;
    return true;
}

bool PrintLifecycleState::on_duration_changed(int seconds, helix::PrintOutcome outcome) {
    if (!is_active()) {
        return false;
    }
    if (outcome != helix::PrintOutcome::NONE) {
        return false;
    }
    elapsed_seconds_ = seconds;
    if (current_state_ == PrintState::Preparing) {
        return false; // preprint observer owns display
    }
    return true;
}

bool PrintLifecycleState::on_time_left_changed(int seconds, helix::PrintOutcome outcome) {
    if (!is_active()) {
        return false;
    }
    if (outcome != helix::PrintOutcome::NONE) {
        return false;
    }
    remaining_seconds_ = seconds;
    if (current_state_ == PrintState::Preparing) {
        return false; // preprint observer owns display
    }
    return true;
}

bool PrintLifecycleState::on_start_phase_changed(int phase,
                                                 helix::PrintJobState current_job_state) {
    bool preparing = (phase != 0);

    if (preparing) {
        spdlog::debug("[PrintLifecycleState] entering Preparing (phase={})", phase);
        current_state_ = PrintState::Preparing;
        preprint_elapsed_seconds_ = 0;
        preprint_remaining_seconds_ = 0;
        return true;
    }

    if (current_state_ == PrintState::Preparing) {
        // Restore state from current job state
        switch (current_job_state) {
        case helix::PrintJobState::PRINTING:
            current_state_ = PrintState::Printing;
            break;
        case helix::PrintJobState::PAUSED:
            current_state_ = PrintState::Paused;
            break;
        default:
            current_state_ = PrintState::Idle;
            break;
        }
        spdlog::debug("[PrintLifecycleState] exiting Preparing -> {}",
                      print_state_name(current_state_));
        return true;
    }

    return false;
}

void PrintLifecycleState::on_preprint_elapsed_changed(int seconds) {
    if (current_state_ != PrintState::Preparing) {
        return;
    }
    preprint_elapsed_seconds_ = seconds;
}

void PrintLifecycleState::on_preprint_remaining_changed(int seconds, int /* slicer_remaining */) {
    if (current_state_ != PrintState::Preparing) {
        return;
    }
    preprint_remaining_seconds_ = seconds;
}

void PrintLifecycleState::on_temperature_changed(int nz_cur, int nz_tgt, int bed_cur, int bed_tgt) {
    nozzle_current_ = nz_cur;
    nozzle_target_ = nz_tgt;
    bed_current_ = bed_cur;
    bed_target_ = bed_tgt;
}

void PrintLifecycleState::on_speed_changed(int speed) {
    speed_percent_ = speed;
}

void PrintLifecycleState::on_flow_changed(int flow) {
    flow_percent_ = flow;
}

void PrintLifecycleState::set_gcode_loaded(bool loaded) {
    gcode_loaded_ = loaded;
    spdlog::trace("[PrintLifecycleState] gcode_loaded = {}", loaded);
}
