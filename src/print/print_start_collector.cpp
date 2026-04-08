// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_start_collector.h"

#include "ui_update_queue.h"

#include "config.h"
#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "memory_monitor.h"
#include "printer_detector.h"
#include "thermal_rate_model.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <ctime>

using namespace helix;

using json = nlohmann::json;

// Config path for pre-print prediction history
static constexpr const char* PREPRINT_HISTORY_PATH = "/print_start_history/entries";

// ============================================================================
// STATIC PATTERN DEFINITIONS
// ============================================================================

// Pattern to detect PRINT_START macro invocation
const std::regex
    PrintStartCollector::print_start_pattern_(R"(PRINT_START|START_PRINT|_PRINT_START)",
                                              std::regex::icase);

// Pattern to detect print start completion (first layer indicator)
// Includes HELIX:READY for our custom macro integration
const std::regex PrintStartCollector::completion_pattern_(
    R"(SET_PRINT_STATS_INFO\s+CURRENT_LAYER=|LAYER:?\s*1\b|;LAYER:1|First layer|HELIX:READY)",
    std::regex::icase);

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PrintStartCollector::PrintStartCollector(MoonrakerClient& client, PrinterState& state)
    : client_(client), state_(state) {
    spdlog::debug("[PrintStartCollector] Constructed");
}

PrintStartCollector::~PrintStartCollector() {
    // Don't call stop() here - it uses client_ and state_ references which may
    // already be destroyed during static destruction order. Callers should
    // explicitly call stop() before letting the shared_ptr go out of scope.
    // Just mark as inactive to prevent any pending callbacks from running.
    active_.store(false);
    registered_.store(false);

    // Safe to delete timer here (doesn't use client_ or state_)
    if (eta_timer_) {
        lv_timer_delete(eta_timer_);
        eta_timer_ = nullptr;
    }
}

// ============================================================================
// PUBLIC METHODS
// ============================================================================

void PrintStartCollector::start() {
    if (active_.load()) {
        spdlog::debug("[PrintStartCollector] Already active, ignoring start()");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // Record start time for timeout fallback
        printing_state_start_ = std::chrono::steady_clock::now();
        detected_phases_.clear();
        current_phase_ = PrintStartPhase::INITIALIZING;
        print_start_detected_ = false;
        max_sequential_progress_ = 0;
        phase_enter_times_.clear();
        // Snapshot stale subject values so fallbacks only trigger on real changes
        baseline_layer_ = lv_subject_get_int(state_.get_print_layer_current_subject());
        baseline_progress_ = lv_subject_get_int(state_.get_print_progress_subject());
    }
    fallbacks_enabled_.store(false); // Will be enabled after initial window
    spdlog::info("[PrintStartCollector] Baselines: layer={}, progress={}", baseline_layer_,
                 baseline_progress_);
    helix::MemoryMonitor::log_now("print_start_collector_start", spdlog::level::debug);

    // Capture start temperatures for heating fraction calculation
    start_ext_temp_ = lv_subject_get_int(state_.get_active_extruder_temp_subject()) / 10;
    start_bed_temp_ = lv_subject_get_int(state_.get_bed_temp_subject()) / 10;
    last_remaining_ = 0;

    // Reset thermal rate models with current temperatures
    {
        auto& mgr = ThermalRateManager::instance();
        mgr.get_model("extruder").reset(static_cast<float>(start_ext_temp_));
        mgr.get_model("heater_bed").reset(static_cast<float>(start_bed_temp_));
    }

    // Load prediction history from config
    load_prediction_history();

    // Compute duration-proportional weights from predictions + thermal model
    compute_predicted_weights();

    // Ensure we have a profile for pattern matching
    if (!profile_) {
        // Try printer-specific profile first (printer type may not have been known at init time)
        std::string printer_type = state_.get_printer_type();
        if (!printer_type.empty()) {
            std::string profile_name = PrinterDetector::get_print_start_profile(printer_type);
            if (!profile_name.empty()) {
                profile_ = PrintStartProfile::load(profile_name);
                spdlog::info("[PrintStartCollector] Loaded profile '{}' for '{}'", profile_name,
                             printer_type);
            }
        }
        if (!profile_) {
            profile_ = PrintStartProfile::load_default();
        }
    }

    auto self = shared_from_this();

    // Register gcode response handler if not already registered (stays registered
    // permanently — on_gcode_response() returns early when !active_)
    if (handler_name_.empty()) {
        static std::atomic<uint64_t> s_collector_id{0};
        handler_name_ = "print_start_collector_" + std::to_string(++s_collector_id);
        client_.register_method_callback("notify_gcode_response", handler_name_,
                                         [self](const json& msg) { self->on_gcode_response(msg); });
    }

    // Register for printer status updates (fallback for printers with KAMP/custom macros)
    // This watches for _START_PRINT.print_started, START_PRINT.preparation_done, etc.
    macro_subscription_id_ = client_.register_notify_update([self](const json& notification) {
        if (!self->active_.load())
            return;

        if (!notification.contains("params") || !notification["params"].is_array() ||
            notification["params"].empty()) {
            return;
        }

        const auto& status = notification["params"][0];

        // Check _START_PRINT.print_started (AD5M KAMP macro)
        if (status.contains("gcode_macro _START_PRINT")) {
            const auto& macro = status["gcode_macro _START_PRINT"];
            if (macro.contains("print_started") && macro["print_started"].is_boolean() &&
                macro["print_started"].get<bool>()) {
                spdlog::info("[PrintStartCollector] Macro signal: print_started=true");
                self->update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
                return;
            }
        }

        // Check START_PRINT.preparation_done
        if (status.contains("gcode_macro START_PRINT")) {
            const auto& macro = status["gcode_macro START_PRINT"];
            if (macro.contains("preparation_done") && macro["preparation_done"].is_boolean() &&
                macro["preparation_done"].get<bool>()) {
                spdlog::info("[PrintStartCollector] Macro signal: preparation_done=true");
                self->update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
                return;
            }
        }

        // Check _HELIX_STATE.print_started (our custom macro)
        if (status.contains("gcode_macro _HELIX_STATE")) {
            const auto& macro = status["gcode_macro _HELIX_STATE"];
            if (macro.contains("print_started") && macro["print_started"].is_boolean() &&
                macro["print_started"].get<bool>()) {
                spdlog::info("[PrintStartCollector] Helix macro signal: print_started=true");
                self->update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
                return;
            }
        }

        // Pass through display_status.message (M117) during preparation.
        // Many firmware/macros send M117 messages like "Homing...", "Probing...",
        // "Heating bed..." during startup that provide useful status info.
        if (status.contains("display_status")) {
            const auto& ds = status["display_status"];
            if (ds.contains("message") && ds["message"].is_string()) {
                const auto& msg = ds["message"].get_ref<const std::string&>();
                if (!msg.empty()) {
                    spdlog::debug("[PrintStartCollector] display_status.message: {}", msg);
                    // Update message without changing the current phase
                    self->update_message_only(msg);
                }
            }
        }
    });

    registered_.store(true);
    active_.store(true);

    // Set initial state
    state_.set_print_start_state(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."), 0);

    // Create LVGL timer for periodic elapsed + ETA updates (runs on main thread)
    {
        eta_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* collector = static_cast<PrintStartCollector*>(lv_timer_get_user_data(timer));
                if (collector) {
                    collector->update_eta_display();
                    collector->check_fallback_completion();
                }
            },
            ETA_UPDATE_INTERVAL_MS, this);
        spdlog::debug("[PrintStartCollector] ETA timer created ({}ms interval)",
                      ETA_UPDATE_INTERVAL_MS);
    }
    // Run first update immediately
    update_eta_display();

    spdlog::debug("[PrintStartCollector] Started monitoring (handler: {})", handler_name_);
}

void PrintStartCollector::stop() {
    // Mark inactive first to stop callbacks from processing
    bool was_active = active_.exchange(false);
    bool was_registered = registered_.exchange(false);

    // Keep gcode response handler registered permanently — it checks active_ and
    // returns early when not collecting. This avoids the race where early responses
    // from START_PRINT are lost because the handler isn't registered yet.

    // Unregister macro subscription using atomic exchange to prevent double-unsubscribe
    SubscriptionId sub_id = macro_subscription_id_.exchange(0);
    if (sub_id != 0) {
        client_.unsubscribe_notify_update(sub_id);
        spdlog::debug("[PrintStartCollector] Unsubscribed from status updates");
    }

    fallbacks_enabled_.store(false);

    // Delete ETA timer (must be on main thread, but stop() is always called from main thread)
    if (eta_timer_) {
        lv_timer_delete(eta_timer_);
        eta_timer_ = nullptr;
    }

    if (was_active) {
        state_.clear_print_start_time_left();
        state_.reset_print_start_state();
        spdlog::debug("[PrintStartCollector] Stopped monitoring");
    }
}

void PrintStartCollector::reset() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        detected_phases_.clear();
        current_phase_ = PrintStartPhase::INITIALIZING;
        print_start_detected_ = false;
        max_sequential_progress_ = 0;
        printing_state_start_ = std::chrono::steady_clock::now();
        phase_enter_times_.clear();
    }
    fallbacks_enabled_.store(false);

    if (active_.load()) {
        state_.set_print_start_state(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."), 0);
    }

    spdlog::debug("[PrintStartCollector] Reset state");
}

void PrintStartCollector::enable_fallbacks() {
    if (!active_.load())
        return;

    fallbacks_enabled_.store(true);
    spdlog::debug("[PrintStartCollector] Fallback detection enabled");

    // Don't immediately check fallback conditions here.
    // set_print_start_state() defers reset_for_new_print() via async invoke,
    // so stale subject data (layer count, progress) from the previous print
    // hasn't been cleared yet. Let fallback checks be triggered naturally by
    // incoming data updates (observer callbacks on layer/progress subjects).
}

void PrintStartCollector::check_fallback_completion() {
    if (!active_.load() || !fallbacks_enabled_.load()) {
        return;
    }

    // Check current phase under lock
    std::chrono::steady_clock::time_point start_time;
    PrintStartPhase current;
    bool print_start_was_detected;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // Already complete - nothing to do
        if (current_phase_ == PrintStartPhase::COMPLETE) {
            return;
        }
        current = current_phase_;
        print_start_was_detected = print_start_detected_;
        start_time = printing_state_start_;
    }

    // Get temperature data for proactive and completion fallback checks
    int ext_temp = lv_subject_get_int(state_.get_active_extruder_temp_subject());
    int ext_target = lv_subject_get_int(state_.get_active_extruder_target_subject());
    int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject());
    int bed_target = lv_subject_get_int(state_.get_bed_target_subject());

    // Temps are in decidegrees (value * 10), targets may be 0 if not set
    bool bed_heating = bed_target > 0 && bed_temp < bed_target - TEMP_TOLERANCE_DECIDEGREES;
    bool nozzle_heating = ext_target > 0 && ext_temp < ext_target - TEMP_TOLERANCE_DECIDEGREES;
    bool temps_ready = !bed_heating && !nozzle_heating;

    // =========================================================================
    // PROACTIVE DETECTION: Track preparation phases from temperature state
    // Continuously updates as heaters change, not just on first detection.
    // Only fires when no gcode_response patterns have been matched recently.
    // =========================================================================
    // Allow proactive temperature detection from any non-terminal phase.
    // On printers like the K1C where gcode responses are sparse, the proactive
    // detector is the primary way to advance through heating phases.
    bool is_temp_detected_phase =
        (current != PrintStartPhase::COMPLETE && current != PrintStartPhase::PURGING);
    if (is_temp_detected_phase) {
        if (bed_heating && (current != PrintStartPhase::HEATING_BED)) {
            // Bed still heating — show bed phase
            spdlog::info("[PrintStartCollector] Proactive: bed heating ({}/{})", bed_temp / 10,
                         bed_target / 10);
            update_phase(PrintStartPhase::HEATING_BED, lv_tr("Heating Bed..."));
            return;
        }
        if (nozzle_heating && !bed_heating && current != PrintStartPhase::HEATING_NOZZLE) {
            // Bed done (or no bed target), nozzle still heating
            spdlog::info("[PrintStartCollector] Proactive: nozzle heating ({}/{})", ext_temp / 10,
                         ext_target / 10);
            update_phase(PrintStartPhase::HEATING_NOZZLE, lv_tr("Heating Nozzle..."));
            return;
        }
        if (temps_ready && !bed_heating && !nozzle_heating &&
            (current == PrintStartPhase::HEATING_BED ||
             current == PrintStartPhase::HEATING_NOZZLE)) {
            // Both heaters at target — preparation continues (homing, mesh, purge, etc.)
            spdlog::info("[PrintStartCollector] Proactive: temps ready, waiting for print start");
            update_phase(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."));
            return;
        }
        if ((bed_heating || nozzle_heating) && current == PrintStartPhase::IDLE) {
            // First detection — heaters just started ramping
            spdlog::info("[PrintStartCollector] Proactive: initializing (heaters ramping)");
            update_phase(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."));
            return;
        }
    }

    // =========================================================================
    // COMPLETION DETECTION: Detect when PRINT_START is done
    // =========================================================================

    // Fallback 1: Layer count (slicer-dependent but reliable when present)
    // Triggers when current_layer advances past baseline — the baseline snapshot at
    // collector start guards against stale values from a previous print.
    {
        int layer = lv_subject_get_int(state_.get_print_layer_current_subject());
        if (layer >= 1 && layer != baseline_layer_) {
            spdlog::info("[PrintStartCollector] Fallback: layer {} detected (baseline={})", layer,
                         baseline_layer_);
            update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
            return;
        }
    }

    // Fallback 2: Progress advancing with temps at target
    // Require progress >= 3% to avoid false triggers during START_PRINT macro execution.
    // Typical START_PRINT macros occupy < 1% of the file (gcode_start_byte / file_size),
    // but virtual_sdcard.progress advances through them. 3% ensures actual layer printing
    // has begun. On K1C, progress stays at 0 during the entire pre-print macro then
    // jumps once the first layer begins.
    int progress = lv_subject_get_int(state_.get_print_progress_subject());
    if (progress > baseline_progress_ && progress >= 3 && temps_ready) {
        spdlog::info("[PrintStartCollector] Fallback: progress {}% with temps ready (baseline={})",
                     progress, baseline_progress_);
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        return;
    }

    // Fallback 3: Timeout with temps near target (90% of target)
    bool temps_near = (ext_target <= 0 || ext_temp >= static_cast<int>(ext_target * 0.9)) &&
                      (bed_target <= 0 || bed_temp >= static_cast<int>(bed_target * 0.9));

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    if (elapsed > FALLBACK_TIMEOUT && temps_near) {
        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        spdlog::info("[PrintStartCollector] Fallback: timeout ({} sec)", elapsed_sec);
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        return;
    }
}

// ============================================================================
// PRIVATE METHODS
// ============================================================================

void PrintStartCollector::on_gcode_response(const json& msg) {
    if (!active_.load()) {
        return;
    }

    // Parse notify_gcode_response format: {"method": "...", "params": ["line"]}
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }

    const std::string& line = msg["params"][0].get_ref<const std::string&>();

    // Skip empty lines and common noise
    if (line.empty() || line == "ok") {
        return;
    }

    spdlog::trace("[PrintStartCollector] G-code: {}", line);

    // Check for HELIX:PHASE signals (highest priority - definitive signals from plugin/macros)
    if (check_helix_phase_signal(line)) {
        return; // Signal handled
    }

    // Check profile signal formats (priority 2, after HELIX:PHASE)
    if (profile_) {
        PrintStartProfile::MatchResult match;
        if (profile_->try_match_signal(line, match)) {
            if (profile_->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL) {
                update_phase(match.phase, match.message, match.progress);
            } else {
                update_phase(match.phase, match.message.c_str());
            }
            return;
        }
    }

    // Check for PRINT_START marker (once per session)
    bool should_set_initializing = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!print_start_detected_ && is_print_start_marker(line)) {
            print_start_detected_ = true;
            should_set_initializing = true;
        }
    }
    if (should_set_initializing) {
        update_phase(PrintStartPhase::INITIALIZING, lv_tr("Starting Print..."));
        spdlog::info("[PrintStartCollector] PRINT_START detected");
        return;
    }

    // Check for completion (layer 1 indicator)
    if (is_completion_marker(line)) {
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        spdlog::debug("[PrintStartCollector] Print start complete - layer 1 detected");
        // Note: The caller (main.cpp) should stop the collector when print state becomes PRINTING
        return;
    }

    // Check phase patterns
    check_phase_patterns(line);
}

void PrintStartCollector::check_phase_patterns(const std::string& line) {
    if (!profile_) {
        return;
    }

    PrintStartProfile::MatchResult match;
    if (profile_->try_match_pattern(line, match)) {
        // Only update if this is a new phase
        bool is_new_phase = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (detected_phases_.find(match.phase) == detected_phases_.end()) {
                detected_phases_.insert(match.phase);
                is_new_phase = true;
            }
        }
        if (is_new_phase) {
            if (profile_->progress_mode() == PrintStartProfile::ProgressMode::SEQUENTIAL) {
                update_phase(match.phase, match.message, match.progress);
            } else {
                update_phase(match.phase, match.message.c_str());
            }
            spdlog::debug("[PrintStartCollector] Detected phase: {} (progress: {}%)",
                          static_cast<int>(match.phase), calculate_progress());
        }
    }
}

bool PrintStartCollector::check_helix_phase_signal(const std::string& line) {
    // Check for HELIX:PHASE:* signals (definitive markers from plugin/macros)
    static const char* HELIX_PHASE_PREFIX = "HELIX:PHASE:";
    constexpr size_t PREFIX_LEN = 12; // strlen("HELIX:PHASE:")

    // Find the prefix in the line (may have quotes or other wrappers)
    size_t pos = line.find(HELIX_PHASE_PREFIX);
    if (pos == std::string::npos) {
        return false;
    }

    // Extract the phase name
    std::string phase_name = line.substr(pos + PREFIX_LEN);
    // Trim trailing whitespace, quotes, etc.
    size_t end = phase_name.find_first_of(" \t\n\r\"'");
    if (end != std::string::npos) {
        phase_name = phase_name.substr(0, end);
    }

    spdlog::info("[PrintStartCollector] HELIX:PHASE signal: {}", phase_name);

    // Map phase name to PrintStartPhase
    if (phase_name == "STARTING" || phase_name == "START") {
        // Mark print start detected and transition to INITIALIZING
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            print_start_detected_ = true;
        }
        update_phase(PrintStartPhase::INITIALIZING, lv_tr("Preparing Print..."));
        return true;
    }

    if (phase_name == "COMPLETE" || phase_name == "DONE") {
        update_phase(PrintStartPhase::COMPLETE, lv_tr("Starting Print..."));
        spdlog::info("[PrintStartCollector] Print start complete via HELIX:PHASE signal");
        return true;
    }

    // Individual phases
    if (phase_name == "HOMING") {
        update_phase(PrintStartPhase::HOMING, lv_tr("Homing..."));
        return true;
    }
    if (phase_name == "HEATING_BED" || phase_name == "BED_HEATING") {
        update_phase(PrintStartPhase::HEATING_BED, lv_tr("Heating Bed..."));
        return true;
    }
    if (phase_name == "HEATING_NOZZLE" || phase_name == "NOZZLE_HEATING" ||
        phase_name == "HEATING_HOTEND") {
        update_phase(PrintStartPhase::HEATING_NOZZLE, lv_tr("Heating Nozzle..."));
        return true;
    }
    if (phase_name == "QGL" || phase_name == "QUAD_GANTRY_LEVEL") {
        update_phase(PrintStartPhase::QGL, lv_tr("Leveling Gantry..."));
        return true;
    }
    if (phase_name == "Z_TILT" || phase_name == "Z_TILT_ADJUST") {
        update_phase(PrintStartPhase::Z_TILT, lv_tr("Z Tilt Adjust..."));
        return true;
    }
    if (phase_name == "BED_MESH" || phase_name == "BED_LEVELING") {
        update_phase(PrintStartPhase::BED_MESH, lv_tr("Loading Bed Mesh..."));
        return true;
    }
    if (phase_name == "CLEANING" || phase_name == "NOZZLE_CLEAN") {
        update_phase(PrintStartPhase::CLEANING, lv_tr("Cleaning Nozzle..."));
        return true;
    }
    if (phase_name == "PURGING" || phase_name == "PURGE" || phase_name == "PRIMING") {
        update_phase(PrintStartPhase::PURGING, lv_tr("Purging..."));
        return true;
    }

    // Unknown phase - log but don't block
    spdlog::warn("[PrintStartCollector] Unknown HELIX:PHASE: {}", phase_name);
    return false;
}

void PrintStartCollector::update_phase(PrintStartPhase phase, const char* message) {
    int progress;
    bool should_save = false;
    bool has_predictions = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (phase == PrintStartPhase::COMPLETE && current_phase_ == PrintStartPhase::COMPLETE)
            return;
        current_phase_ = phase;

        // Record phase enter timestamp (skip IDLE and INITIALIZING)
        int phase_int = static_cast<int>(phase);
        if (phase != PrintStartPhase::IDLE && phase != PrintStartPhase::INITIALIZING &&
            phase != PrintStartPhase::COMPLETE) {
            if (phase_enter_times_.find(phase_int) == phase_enter_times_.end()) {
                phase_enter_times_[phase_int] = std::chrono::steady_clock::now();
            }
        }

        if (phase == PrintStartPhase::COMPLETE) {
            should_save = true;
        }

        progress = calculate_progress_locked();
        has_predictions = predictor_.has_predictions();
    }

    // When predictor has data, time-based progress in update_eta_display() is the
    // sole progress source. Don't override it with phase-weight progress here.
    if (has_predictions && phase != PrintStartPhase::COMPLETE) {
        auto* subj = state_.get_print_start_progress_subject();
        if (subj) {
            progress = lv_subject_get_int(subj);
        }
    }

    // Call PrinterState outside the lock to avoid potential deadlocks
    state_.set_print_start_state(phase, message, progress);

    if (should_save) {
        save_prediction_entry();
    }
}

void PrintStartCollector::update_phase(PrintStartPhase phase, const std::string& message,
                                       int progress) {
    int effective_progress;
    bool should_save = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (phase == PrintStartPhase::COMPLETE && current_phase_ == PrintStartPhase::COMPLETE)
            return;
        current_phase_ = phase;
        detected_phases_.insert(phase);

        // Record phase enter timestamp (skip IDLE and INITIALIZING)
        int phase_int = static_cast<int>(phase);
        if (phase != PrintStartPhase::IDLE && phase != PrintStartPhase::INITIALIZING &&
            phase != PrintStartPhase::COMPLETE) {
            if (phase_enter_times_.find(phase_int) == phase_enter_times_.end()) {
                phase_enter_times_[phase_int] = std::chrono::steady_clock::now();
            }
        }

        // Monotonic progress guard: never allow progress to decrease in sequential mode
        // (except COMPLETE which always goes to 100%)
        if (phase == PrintStartPhase::COMPLETE) {
            effective_progress = 100;
            should_save = true;
        } else {
            effective_progress = std::max(progress, max_sequential_progress_);
            effective_progress = std::min(effective_progress, 95);
        }
        max_sequential_progress_ = effective_progress;
    }
    state_.set_print_start_state(phase, message.c_str(), effective_progress);

    if (should_save) {
        save_prediction_entry();
    }
}

void PrintStartCollector::update_message_only(const std::string& message) {
    PrintStartPhase phase;
    int progress;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        phase = current_phase_;
        if (phase == PrintStartPhase::IDLE || phase == PrintStartPhase::COMPLETE) {
            return; // Don't update message when not preparing
        }
        progress = calculate_progress_locked();
    }
    state_.set_print_start_state(phase, message.c_str(), progress);
}

void PrintStartCollector::set_profile(std::shared_ptr<PrintStartProfile> profile) {
    if (active_.load()) {
        spdlog::warn("[PrintStartCollector] set_profile() called while active, ignoring");
        return;
    }
    profile_ = std::move(profile);
    if (profile_) {
        spdlog::debug("[PrintStartCollector] Using profile: {}", profile_->name());
    } else {
        spdlog::info("[PrintStartCollector] No profile set, signal/pattern matching disabled");
    }
}

int PrintStartCollector::calculate_progress() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return calculate_progress_locked();
}

int PrintStartCollector::calculate_progress_locked() const {
    if (predicted_phase_weights_.empty()) {
        // Fallback to profile weights if no predictions
        if (!profile_)
            return 0;
        int total_weight = 0;
        for (const auto& phase : detected_phases_) {
            total_weight += profile_->get_phase_weight(phase);
        }
        return std::min(total_weight, 95);
    }

    float progress = 0.0f;

    // Completed phases (not the current one)
    for (const auto& phase : detected_phases_) {
        if (phase == current_phase_)
            continue;
        auto it = predicted_phase_weights_.find(static_cast<int>(phase));
        if (it != predicted_phase_weights_.end()) {
            progress += it->second;
        }
    }

    // Partial progress for current phase
    auto cur_it = predicted_phase_weights_.find(static_cast<int>(current_phase_));
    if (cur_it != predicted_phase_weights_.end()) {
        float phase_frac = 0.0f;

        if (current_phase_ == PrintStartPhase::HEATING_BED ||
            current_phase_ == PrintStartPhase::HEATING_NOZZLE) {
            phase_frac = compute_heating_fraction();
        } else {
            // Time-based for non-heating phases
            auto enter_it = phase_enter_times_.find(static_cast<int>(current_phase_));
            if (enter_it != phase_enter_times_.end()) {
                float elapsed =
                    static_cast<float>(std::chrono::duration_cast<std::chrono::seconds>(
                                           std::chrono::steady_clock::now() - enter_it->second)
                                           .count());
                auto pred = predictor_.predicted_phases();
                auto pred_it = pred.find(static_cast<int>(current_phase_));
                if (pred_it != pred.end() && pred_it->second > 0) {
                    phase_frac = std::min(elapsed / static_cast<float>(pred_it->second), 0.95f);
                }
            }
        }
        progress += cur_it->second * phase_frac;
    }

    return std::min(static_cast<int>(progress * 95.0f), 95);
}

bool PrintStartCollector::is_print_start_marker(const std::string& line) const {
    return std::regex_search(line, print_start_pattern_);
}

bool PrintStartCollector::is_completion_marker(const std::string& line) const {
    return std::regex_search(line, completion_pattern_);
}

// ============================================================================
// PUBLIC ACCESSORS FOR PREDICTION
// ============================================================================

std::set<int> PrintStartCollector::get_completed_phase_ints() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return get_completed_phase_ints_locked();
}

std::set<int> PrintStartCollector::get_completed_phase_ints_locked() const {
    std::set<int> result;
    for (const auto& phase : detected_phases_) {
        int p = static_cast<int>(phase);
        // Exclude current phase from "completed" set
        if (phase != current_phase_ && phase != PrintStartPhase::IDLE &&
            phase != PrintStartPhase::INITIALIZING) {
            result.insert(p);
        }
    }
    return result;
}

int PrintStartCollector::get_current_phase_int() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return static_cast<int>(current_phase_);
}

int PrintStartCollector::get_current_phase_elapsed_seconds() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int phase_int = static_cast<int>(current_phase_);
    auto it = phase_enter_times_.find(phase_int);
    if (it == phase_enter_times_.end()) {
        return 0;
    }
    auto elapsed = std::chrono::steady_clock::now() - it->second;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
}

// ============================================================================
// ETA DISPLAY UPDATE
// ============================================================================

void PrintStartCollector::update_eta_display() {
    if (!active_.load()) {
        return;
    }

    // Always update elapsed time since preparation started
    int total_elapsed;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto now = std::chrono::steady_clock::now();
        total_elapsed = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(now - printing_state_start_).count());
    }
    state_.set_preprint_elapsed_seconds(total_elapsed);
    feed_thermal_sample();

    // Snapshot predictor data under lock — save_prediction_entry() can call
    // predictor_.add_entry() from the background thread (via update_phase).
    int remaining;
    int predicted_total;
    int current_snap = 0;
    int phase_elapsed_snap = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!predictor_.has_predictions()) {
            return;
        }

        auto completed = get_completed_phase_ints_locked();
        int current = static_cast<int>(current_phase_);
        int phase_elapsed = 0;
        auto it = phase_enter_times_.find(current);
        if (it != phase_enter_times_.end()) {
            phase_elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                                                 std::chrono::steady_clock::now() - it->second)
                                                 .count());
        }

        remaining = predictor_.remaining_seconds(completed, current, phase_elapsed);
        predicted_total = predictor_.predicted_total();
        current_snap = current;
        phase_elapsed_snap = phase_elapsed;
    }

    // --- Diagnostic: monotonic bias data collection ---
    {
        auto pred_phases = predictor_.predicted_phases();
        int cur_phase_predicted = 0;
        auto pp_it = pred_phases.find(current_snap);
        if (pp_it != pred_phases.end())
            cur_phase_predicted = pp_it->second;

        spdlog::debug("[PrintStartCollector] ETA update: phase={} elapsed={}s, remaining={}s, "
                      "predicted_total={:.0f}s, phase_predicted={}s",
                      current_snap, phase_elapsed_snap, remaining, predicted_total_seconds_,
                      cur_phase_predicted);
    }

    // Always update the int subject for print time integration
    // Monotonic bias: once under 2 minutes, only decrease unless overrun > 20%
    if (remaining < 120 && last_remaining_ > 0 && last_remaining_ < 120) {
        if (remaining > last_remaining_) {
            float overrun_pct = static_cast<float>(remaining - last_remaining_) /
                                std::max(1.0f, predicted_total_seconds_) * 100.0f;
            if (overrun_pct < 20.0f) {
                auto pred_phases = predictor_.predicted_phases();
                int cur_phase_predicted = 0;
                auto pp_it = pred_phases.find(current_snap);
                if (pp_it != pred_phases.end())
                    cur_phase_predicted = pp_it->second;
                float per_phase_overrun_pct =
                    static_cast<float>(remaining - last_remaining_) /
                    std::max(1.0f, static_cast<float>(cur_phase_predicted)) * 100.0f;
                int remaining_before_bias = remaining;
                remaining = last_remaining_;
                spdlog::debug("[PrintStartCollector] Monotonic bias: suppressed {}s→{}s, "
                              "overrun={:.1f}% (total denom={:.0f}s), "
                              "per-phase would be {:.1f}% (phase denom={}s)",
                              last_remaining_, remaining_before_bias, overrun_pct,
                              predicted_total_seconds_, per_phase_overrun_pct, cur_phase_predicted);
            }
        }
    }
    last_remaining_ = remaining;

    state_.set_preprint_remaining_seconds(remaining);
    if (predicted_total > 0) {
        int effective_progress = calculate_progress();
        auto* subj = state_.get_print_start_progress_subject();
        if (subj && lv_subject_get_int(subj) != effective_progress) {
            lv_subject_set_int(subj, effective_progress);
        }
    }

    if (remaining <= 0) {
        state_.set_print_start_time_left("Almost ready");
        return;
    }

    // Round for stable display, then format as "~X min left" or "~X:XX left"
    int display_remaining = helix::format::round_eta_seconds(remaining);
    std::string text = "~" + helix::format::duration_remaining(display_remaining);
    state_.set_print_start_time_left(text.c_str());

    spdlog::trace("[PrintStartCollector] ETA: {}s remaining, predicted_total={}s", remaining,
                  predicted_total);
}

// ============================================================================
// PREDICTION HISTORY PERSISTENCE
// ============================================================================

void PrintStartCollector::load_prediction_history() {
    auto entries = helix::PreprintPredictor::load_entries_from_config();

    // Cold (1) vs Warm (2) based on current bed temp
    int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject()) / 10;
    int temp_bucket = (bed_temp >= 40) ? 2 : 1;

    std::lock_guard<std::mutex> lock(state_mutex_);
    predictor_.load_entries(entries, temp_bucket);
    loaded_temp_bucket_ = temp_bucket;

    if (!entries.empty()) {
        spdlog::debug("[PrintStartCollector] Loaded {} prediction entries for {} bucket "
                      "(predicted total: {}s)",
                      predictor_.get_entries().size(), temp_bucket == 2 ? "warm" : "cold",
                      predictor_.predicted_total());
    }
}

void PrintStartCollector::feed_thermal_sample() {
    auto& mgr = ThermalRateManager::instance();
    auto now_ms =
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - printing_state_start_)
                                  .count());

    PrintStartPhase phase;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        phase = current_phase_;
    }

    if (phase == PrintStartPhase::HEATING_BED || phase == PrintStartPhase::HEATING_NOZZLE) {
        // Temps are in decidegrees (value * 10)
        int ext_temp = lv_subject_get_int(state_.get_active_extruder_temp_subject());
        int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject());
        mgr.get_model("extruder").record_sample(ext_temp / 10.0f, now_ms);
        mgr.get_model("heater_bed").record_sample(bed_temp / 10.0f, now_ms);
    }
}

void PrintStartCollector::compute_predicted_weights() {
    auto& mgr = ThermalRateManager::instance();

    int ext_target = lv_subject_get_int(state_.get_active_extruder_target_subject()) / 10;
    int bed_target = lv_subject_get_int(state_.get_bed_target_subject()) / 10;

    std::map<int, float> durations;

    // Heating phases from thermal model
    float ext_heat = mgr.estimate_heating_seconds("extruder", static_cast<float>(start_ext_temp_),
                                                  static_cast<float>(ext_target));
    float bed_heat = mgr.estimate_heating_seconds("heater_bed", static_cast<float>(start_bed_temp_),
                                                  static_cast<float>(bed_target));
    if (ext_heat > 0)
        durations[static_cast<int>(PrintStartPhase::HEATING_NOZZLE)] = ext_heat;
    if (bed_heat > 0)
        durations[static_cast<int>(PrintStartPhase::HEATING_BED)] = bed_heat;

    // Non-heating phases from predictor
    std::map<int, int> pred_phases;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        pred_phases = predictor_.predicted_phases();
    }
    for (const auto& [phase, secs] : pred_phases) {
        if (phase != static_cast<int>(PrintStartPhase::HEATING_BED) &&
            phase != static_cast<int>(PrintStartPhase::HEATING_NOZZLE)) {
            durations[phase] = static_cast<float>(secs);
        }
    }

    // Ensure homing has at least a default
    int homing = static_cast<int>(PrintStartPhase::HOMING);
    if (durations.find(homing) == durations.end()) {
        durations[homing] = 20.0f;
    }

    predicted_total_seconds_ = 0.0f;
    for (const auto& [_, dur] : durations) {
        predicted_total_seconds_ += dur;
    }

    predicted_phase_weights_.clear();
    if (predicted_total_seconds_ > 0.0f) {
        for (const auto& [phase, dur] : durations) {
            predicted_phase_weights_[phase] = dur / predicted_total_seconds_;
        }
    }

    spdlog::debug("[PrintStartCollector] Predicted weights: total={:.0f}s, {} phases",
                  predicted_total_seconds_, predicted_phase_weights_.size());
}

float PrintStartCollector::compute_heating_fraction() const {
    int ext_temp = lv_subject_get_int(state_.get_active_extruder_temp_subject()) / 10;
    int ext_target = lv_subject_get_int(state_.get_active_extruder_target_subject()) / 10;
    int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject()) / 10;
    int bed_target = lv_subject_get_int(state_.get_bed_target_subject()) / 10;

    if (current_phase_ == PrintStartPhase::HEATING_NOZZLE && ext_target > 0) {
        float start = start_ext_temp_ > 0 ? static_cast<float>(start_ext_temp_) : 25.0f;
        float range = static_cast<float>(ext_target) - start;
        if (range > 0.0f) {
            return std::clamp((static_cast<float>(ext_temp) - start) / range, 0.0f, 1.0f);
        }
    }
    if (current_phase_ == PrintStartPhase::HEATING_BED && bed_target > 0) {
        float start = start_bed_temp_ > 0 ? static_cast<float>(start_bed_temp_) : 25.0f;
        float range = static_cast<float>(bed_target) - start;
        if (range > 0.0f) {
            return std::clamp((static_cast<float>(bed_temp) - start) / range, 0.0f, 1.0f);
        }
    }
    return 0.0f;
}

void PrintStartCollector::save_prediction_entry() {
    // Compute per-phase durations from timestamps
    std::map<int, int> phase_durations;
    auto now = std::chrono::steady_clock::now();
    int total_seconds = 0;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);

        // Build sorted list of phase enter times
        std::vector<std::pair<int, std::chrono::steady_clock::time_point>> sorted_phases(
            phase_enter_times_.begin(), phase_enter_times_.end());
        std::sort(sorted_phases.begin(), sorted_phases.end(),
                  [](const auto& a, const auto& b) { return a.second < b.second; });

        for (size_t i = 0; i < sorted_phases.size(); ++i) {
            int phase_int = sorted_phases[i].first;
            // Heating phases tracked by ThermalRateModel, not predictor
            if (phase_int == static_cast<int>(PrintStartPhase::HEATING_BED) ||
                phase_int == static_cast<int>(PrintStartPhase::HEATING_NOZZLE)) {
                continue;
            }
            auto end_time = (i + 1 < sorted_phases.size()) ? sorted_phases[i + 1].second : now;
            int duration = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(end_time - sorted_phases[i].second)
                    .count());
            phase_durations[phase_int] = duration;
            total_seconds += duration;
        }

        // --- Diagnostic: predicted vs actual per-phase ---
        auto predicted = predictor_.predicted_phases();
        spdlog::debug("[PrintStartCollector] Print complete — predicted vs actual:");
        for (const auto& [phase, actual] : phase_durations) {
            auto p_it = predicted.find(phase);
            int pred = (p_it != predicted.end()) ? p_it->second : 0;
            float error_pct =
                pred > 0 ? ((static_cast<float>(actual) - pred) / pred * 100.0f) : 0.0f;
            spdlog::debug(
                "[PrintStartCollector]   phase {}: predicted={}s actual={}s error={:.1f}%", phase,
                pred, actual, error_pct);
        }
    }

    if (phase_durations.empty()) {
        spdlog::debug("[PrintStartCollector] No phase timings to save");
        return;
    }

    // Cold (1) vs Warm (2) based on bed temp at start
    helix::PreprintEntry entry;
    entry.total_seconds = total_seconds;
    entry.timestamp = static_cast<int64_t>(std::time(nullptr));
    entry.phase_durations = std::move(phase_durations);
    entry.temp_bucket = (start_bed_temp_ >= 40) ? 2 : 1;

    std::vector<helix::PreprintEntry> bucket_entries;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        predictor_.add_entry(entry);
        bucket_entries = predictor_.get_entries();
    }

    // Persist to config: merge bucket entries with entries from other buckets
    int temp_bucket = entry.temp_bucket;
    helix::ui::queue_update([bucket_entries, temp_bucket]() {
        // Load all existing entries, keep those from OTHER buckets
        auto all_entries = helix::PreprintPredictor::load_entries_from_config();
        std::vector<helix::PreprintEntry> merged;
        for (const auto& e : all_entries) {
            if (e.temp_bucket != temp_bucket && e.temp_bucket != 0) {
                merged.push_back(e);
            }
        }
        // Add current bucket's entries
        for (const auto& e : bucket_entries) {
            merged.push_back(e);
        }
        // Cap total stored entries to prevent unbounded growth
        constexpr size_t MAX_TOTAL_ENTRIES = 15;
        while (merged.size() > MAX_TOTAL_ENTRIES) {
            merged.erase(merged.begin());
        }
        auto entries = std::move(merged);
        auto* cfg = Config::get_instance();
        if (!cfg) {
            return;
        }

        if (entries.size() > 1000) {
            spdlog::warn("[PrintStartCollector] Suspiciously large entry count ({}), skipping save",
                         entries.size());
            return;
        }

        try {
            json entries_json = json::array();
            for (const auto& e : entries) {
                json entry_json;
                entry_json["total"] = e.total_seconds;
                entry_json["timestamp"] = e.timestamp;

                json phases_json = json::object();
                for (const auto& [phase, duration] : e.phase_durations) {
                    phases_json[std::to_string(phase)] = duration;
                }
                entry_json["phases"] = phases_json;
                if (e.temp_bucket > 0) {
                    entry_json["temp_bucket"] = e.temp_bucket;
                }
                entries_json.push_back(entry_json);
            }

            cfg->set<json>(PREPRINT_HISTORY_PATH, entries_json);
            ThermalRateManager::instance().save_to_config(*cfg);
            cfg->save();

            spdlog::debug("[PrintStartCollector] Saved prediction history ({} entries)",
                          entries.size());
        } catch (const std::exception& ex) {
            spdlog::error("[PrintStartCollector] Failed to save prediction history: {}", ex.what());
        }
    });
}
