// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ams_backend_ace.cpp
 * @brief ACE (AnyCubic ACE Pro) backend implementation
 *
 * Implements AMS backend for ACE Pro (ValgACE/BunnyACE/DuckACE) using REST API polling.
 * See ams_backend_ace.h for API documentation.
 */

#include "ams_backend_ace.h"

#include "moonraker_api.h"
#include "moonraker_client.h"
#include "spdlog/spdlog.h"

#include <chrono>

using json = nlohmann::json;
using namespace helix;

// ============================================================================
// Construction / Destruction
// ============================================================================

AmsBackendAce::AmsBackendAce(MoonrakerAPI* api, MoonrakerClient* client)
    : api_(api), client_(client) {
    // Initialize system info with ACE defaults
    system_info_.type = AmsType::ACE;
    system_info_.type_name = "ACE";
    system_info_.version = "unknown";
    system_info_.supports_bypass = false; // ACE Pro has no bypass mode

    // Initialize dryer info with ACE Pro capabilities
    dryer_info_.supported = true;
    dryer_info_.active = false;
    dryer_info_.allows_during_print = false; // Default: block during print
    dryer_info_.min_temp_c = 35.0f;
    dryer_info_.max_temp_c = 70.0f;
    dryer_info_.max_duration_min = 720;       // 12 hours
    dryer_info_.supports_fan_control = false; // ACE Pro doesn't expose fan control
}

AmsBackendAce::~AmsBackendAce() {
    // lifetime_ destructor calls invalidate() automatically
    stop();
}

// ============================================================================
// Lifecycle
// ============================================================================

AmsError AmsBackendAce::start() {
    if (running_.load()) {
        return AmsErrorHelper::success();
    }

    if (!api_ || !client_) {
        return AmsError(AmsResult::NOT_INITIALIZED,
                        "ACE backend requires valid MoonrakerAPI and MoonrakerClient",
                        "Internal error", "Contact support");
    }

    spdlog::info("[ACE] Starting backend");

    // Reset state
    stop_requested_.store(false);
    info_fetched_ = false;

    // Start polling thread
    running_.store(true);
    polling_thread_ = std::thread(&AmsBackendAce::polling_thread_func, this);

    return AmsErrorHelper::success();
}

void AmsBackendAce::stop() {
    if (!running_.load()) {
        return;
    }

    spdlog::info("[ACE] Stopping backend");

    // Signal thread to stop
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        stop_requested_.store(true);
    }
    stop_cv_.notify_all();

    // Wait for thread to exit
    if (polling_thread_.joinable()) {
        polling_thread_.join();
    }

    running_.store(false);
    spdlog::info("[ACE] Backend stopped");
}

bool AmsBackendAce::is_running() const {
    return running_.load();
}

// ============================================================================
// Events
// ============================================================================

void AmsBackendAce::set_event_callback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    event_callback_ = std::move(callback);
}

void AmsBackendAce::emit_event(const std::string& event, const std::string& data) {
    EventCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = event_callback_;
    }
    if (cb) {
        cb(event, data);
    }
}

// ============================================================================
// State Queries
// ============================================================================

AmsSystemInfo AmsBackendAce::get_system_info() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return system_info_;
}

AmsType AmsBackendAce::get_type() const {
    return AmsType::ACE;
}

SlotInfo AmsBackendAce::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // ACE is a single-unit system
    if (system_info_.units.empty()) {
        SlotInfo empty;
        empty.slot_index = -1;
        empty.global_index = -1;
        return empty;
    }

    const auto& unit = system_info_.units[0];
    if (slot_index < 0 || slot_index >= static_cast<int>(unit.slots.size())) {
        SlotInfo empty;
        empty.slot_index = -1;
        empty.global_index = -1;
        return empty;
    }
    return unit.slots[static_cast<size_t>(slot_index)];
}

AmsAction AmsBackendAce::get_current_action() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return system_info_.action;
}

int AmsBackendAce::get_current_tool() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return system_info_.current_tool;
}

int AmsBackendAce::get_current_slot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return system_info_.current_slot;
}

bool AmsBackendAce::is_filament_loaded() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return system_info_.filament_loaded;
}

// ============================================================================
// Path Visualization
// ============================================================================

PathTopology AmsBackendAce::get_topology() const {
    // ACE Pro uses a hub topology (4 slots merge to single output)
    return PathTopology::HUB;
}

PathSegment AmsBackendAce::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!system_info_.filament_loaded) {
        return PathSegment::NONE;
    }

    // If filament is loaded, it's at the nozzle (fully loaded)
    // (ACE Pro doesn't report intermediate positions)
    return PathSegment::NOZZLE;
}

PathSegment AmsBackendAce::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // ACE is a single-unit system
    if (system_info_.units.empty()) {
        return PathSegment::NONE;
    }

    const auto& unit = system_info_.units[0];
    if (slot_index < 0 || slot_index >= static_cast<int>(unit.slots.size())) {
        return PathSegment::NONE;
    }

    const auto& slot = unit.slots[static_cast<size_t>(slot_index)];

    // If this is the loaded slot, show full path
    if (system_info_.filament_loaded && system_info_.current_slot == slot_index) {
        return PathSegment::NOZZLE;
    }

    // Otherwise, filament is at the spool/slot (if present)
    if (slot.status == SlotStatus::AVAILABLE || slot.status == SlotStatus::LOADED) {
        return PathSegment::SPOOL;
    }

    return PathSegment::NONE;
}

PathSegment AmsBackendAce::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // If we're in an error state, try to infer location
    if (system_info_.action == AmsAction::ERROR) {
        // Most ACE errors occur at the hub (feeding mechanism)
        return PathSegment::HUB;
    }

    return PathSegment::NONE;
}

// ============================================================================
// Filament Operations
// ============================================================================

AmsError AmsBackendAce::load_filament(int slot_index) {
    auto err = check_preconditions();
    if (!err.success()) {
        return err;
    }

    err = validate_slot_index(slot_index);
    if (!err.success()) {
        return err;
    }

    spdlog::info("[ACE] Loading filament from slot {}", slot_index);
    return execute_gcode("ACE_CHANGE_TOOL TOOL=" + std::to_string(slot_index));
}

AmsError AmsBackendAce::unload_filament(int /*slot_index*/) {
    auto err = check_preconditions();
    if (!err.success()) {
        return err;
    }

    spdlog::info("[ACE] Unloading filament");
    return execute_gcode("ACE_CHANGE_TOOL TOOL=-1");
}

AmsError AmsBackendAce::select_slot(int slot_index) {
    // ACE Pro doesn't have a "select without load" concept
    // Just do a load operation
    return load_filament(slot_index);
}

AmsError AmsBackendAce::change_tool(int tool_number) {
    // Tool number maps directly to slot index on ACE Pro
    return load_filament(tool_number);
}

// ============================================================================
// Recovery Operations
// ============================================================================

AmsError AmsBackendAce::recover() {
    spdlog::info("[ACE] Attempting recovery");
    // ACE Pro may have a recovery command - TBD based on ACE driver docs
    return execute_gcode("ACE_RECOVER");
}

AmsError AmsBackendAce::reset() {
    spdlog::info("[ACE] Resetting");
    return execute_gcode("ACE_RESET");
}

AmsError AmsBackendAce::cancel() {
    spdlog::info("[ACE] Cancelling operation");
    // Unload any active filament operation
    return execute_gcode("ACE_CHANGE_TOOL TOOL=-1");
}

// ============================================================================
// Configuration
// ============================================================================

AmsError AmsBackendAce::set_slot_info(int slot_index, const SlotInfo& info, bool /*persist*/) {
    // ACE may support slot configuration via Spoolman integration
    // For now, this is not supported
    (void)slot_index;
    (void)info;
    return AmsErrorHelper::not_supported("Slot configuration");
}

AmsError AmsBackendAce::set_tool_mapping(int tool_number, int slot_index) {
    // ACE Pro uses 1:1 tool-to-slot mapping
    (void)tool_number;
    (void)slot_index;
    return AmsErrorHelper::not_supported("Tool mapping");
}

helix::printer::ToolMappingCapabilities AmsBackendAce::get_tool_mapping_capabilities() const {
    // ACE has fixed 1:1 mapping - not configurable
    return {false, false, ""};
}

std::vector<int> AmsBackendAce::get_tool_mapping() const {
    // ACE has fixed 1:1 mapping - return empty (not supported)
    return {};
}

// ============================================================================
// Bypass Mode (not supported)
// ============================================================================

AmsError AmsBackendAce::enable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

AmsError AmsBackendAce::disable_bypass() {
    return AmsErrorHelper::not_supported("Bypass mode");
}

bool AmsBackendAce::is_bypass_active() const {
    return false; // ACE Pro has no bypass
}

// ============================================================================
// Dryer Control
// ============================================================================

DryerInfo AmsBackendAce::get_dryer_info() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return dryer_info_;
}

AmsError AmsBackendAce::start_drying(float temp_c, int duration_min, int fan_pct, int unit) {
    (void)unit;
    auto err = check_preconditions();
    if (!err.success()) {
        return err;
    }

    // Read dryer limits under lock for thread safety
    float min_temp, max_temp;
    int max_duration;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        min_temp = dryer_info_.min_temp_c;
        max_temp = dryer_info_.max_temp_c;
        max_duration = dryer_info_.max_duration_min;
    }

    // Validate temperature
    if (temp_c < min_temp || temp_c > max_temp) {
        return AmsError(AmsResult::COMMAND_FAILED,
                        "Temperature out of range: " + std::to_string(temp_c),
                        "Invalid temperature",
                        "Set temperature between " + std::to_string(static_cast<int>(min_temp)) +
                            "°C and " + std::to_string(static_cast<int>(max_temp)) + "°C");
    }

    // Validate duration
    if (duration_min <= 0 || duration_min > max_duration) {
        return AmsError(AmsResult::COMMAND_FAILED,
                        "Duration out of range: " + std::to_string(duration_min),
                        "Invalid duration",
                        "Set duration between 1 and " + std::to_string(max_duration) + " minutes");
    }

    spdlog::info("[ACE] Starting drying: {}°C for {} minutes", temp_c, duration_min);

    // Fan percentage ignored - ACE Pro doesn't support it
    (void)fan_pct;

    std::string gcode = "ACE_START_DRYING TEMP=" + std::to_string(static_cast<int>(temp_c)) +
                        " DURATION=" + std::to_string(duration_min);
    return execute_gcode(gcode);
}

AmsError AmsBackendAce::stop_drying(int unit) {
    (void)unit;
    spdlog::info("[ACE] Stopping drying");
    return execute_gcode("ACE_STOP_DRYING");
}

AmsError AmsBackendAce::update_drying(float temp_c, int duration_min, int fan_pct) {
    // ACE Pro may not support updating while running - stop and restart
    auto err = stop_drying();
    if (!err.success()) {
        return err;
    }

    // Use current values if -1 passed
    float target_temp = temp_c;
    int target_duration = duration_min;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (temp_c < 0) {
            target_temp = dryer_info_.target_temp_c;
        }
        if (duration_min < 0) {
            target_duration = dryer_info_.duration_min;
        }
    }

    return start_drying(target_temp, target_duration, fan_pct);
}

std::vector<DryingPreset> AmsBackendAce::get_drying_presets() const {
    // Return ACE Pro optimized presets
    return get_default_drying_presets();
}

// ============================================================================
// Polling Thread
// ============================================================================

void AmsBackendAce::polling_thread_func() {
    spdlog::debug("[ACE] Polling thread started");

    // Fetch system info — retry until success or failure limit reached.
    // info_fetch_failures_ is incremented on each failed attempt; after 3
    // failures we stop retrying to avoid hammering a missing bridge endpoint.
    static constexpr int MAX_INFO_FETCH_FAILURES = 3;
    poll_info();

    while (!stop_requested_.load()) {
        // Retry info fetch if it hasn't succeeded yet and we haven't hit the limit
        if (!info_fetched_.load() && info_fetch_failures_ < MAX_INFO_FETCH_FAILURES) {
            poll_info();
        } else if (!info_fetched_.load() && info_fetch_failures_ >= MAX_INFO_FETCH_FAILURES) {
            // Bridge is not available — log once at this threshold and keep backend alive
            // so it can recover if the bridge is later installed and the backend restarted.
            if (info_fetch_failures_ == MAX_INFO_FETCH_FAILURES) {
                spdlog::warn("[ACE] Giving up on /server/ace/info after {} attempts. "
                             "Backend will remain idle until restarted.",
                             MAX_INFO_FETCH_FAILURES);
                ++info_fetch_failures_; // Advance past threshold so this logs only once
            }
        }

        // Poll status and slots
        poll_status();
        poll_slots();

        // Sleep with interrupt support
        if (!interruptible_sleep(POLL_INTERVAL_MS)) {
            break; // Stop requested during sleep
        }
    }

    spdlog::debug("[ACE] Polling thread exiting");
}

void AmsBackendAce::poll_info() {
    if (!api_) {
        return;
    }

    spdlog::debug("[ACE] Polling /server/ace/info");

    // Use heap-allocated sync state to avoid dangling reference if timeout expires
    struct SyncState {
        std::mutex mtx;
        std::condition_variable cv;
        bool done{false};
    };
    auto state = std::make_shared<SyncState>();

    auto token = lifetime_.token();

    api_->rest().call_rest_get("/server/ace/info", [this, state, token](const RestResponse& resp) {
        // Check if object is still alive before accessing members
        if (token.expired()) {
            std::lock_guard<std::mutex> lock(state->mtx);
            state->done = true;
            state->cv.notify_one();
            return;
        }

        if (resp.success && resp.data.contains("result")) {
            parse_info_response(resp.data["result"]);
            info_fetched_.store(true);
            info_fetch_failures_ = 0;
        } else {
            ++info_fetch_failures_;
            if (info_fetch_failures_ <= 3) {
                spdlog::warn("[ACE] Moonraker bridge not available at /server/ace/info: {}. "
                             "BunnyACE/DuckACE users need to install ValgACE's ace_status.py "
                             "Moonraker component for HelixScreen integration.",
                             resp.error);
                emit_event(EVENT_ERROR,
                           "ACE detected but Moonraker bridge not found. "
                           "Install the ace_status.py component from ValgACE for full ACE "
                           "support.");
            }
        }

        {
            std::lock_guard<std::mutex> lock(state->mtx);
            state->done = true;
        }
        state->cv.notify_one();
    });

    // Wait for response (with timeout)
    std::unique_lock<std::mutex> lock(state->mtx);
    state->cv.wait_for(lock, std::chrono::seconds(5), [state] { return state->done; });
}

void AmsBackendAce::poll_status() {
    if (!api_) {
        return;
    }

    spdlog::trace("[ACE] Polling /server/ace/status");

    auto token = lifetime_.token();

    api_->rest().call_rest_get("/server/ace/status", [this, token](const RestResponse& resp) {
        if (token.expired()) return;

        if (resp.success && resp.data.contains("result")) {
            if (parse_status_response(resp.data["result"])) {
                emit_event(EVENT_STATE_CHANGED);
            }
        } else {
            spdlog::debug("[ACE] Status poll failed: {}", resp.error);
        }
    });
}

void AmsBackendAce::poll_slots() {
    if (!api_) {
        return;
    }

    spdlog::trace("[ACE] Polling /server/ace/slots");

    auto token = lifetime_.token();

    api_->rest().call_rest_get("/server/ace/slots", [this, token](const RestResponse& resp) {
        if (token.expired()) return;

        if (resp.success && resp.data.contains("result")) {
            if (parse_slots_response(resp.data["result"])) {
                emit_event(EVENT_SLOT_CHANGED);
            }
        } else {
            spdlog::debug("[ACE] Slots poll failed: {}", resp.error);
        }
    });
}

// ============================================================================
// Response Parsing
// ============================================================================

void AmsBackendAce::parse_info_response(const json& data) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (data.contains("model") && data["model"].is_string()) {
        system_info_.type_name = "ACE";
    }

    if (data.contains("version") && data["version"].is_string()) {
        system_info_.version = data["version"].get<std::string>();
    }

    if (data.contains("slot_count") && data["slot_count"].is_number_integer()) {
        int slot_count = data["slot_count"].get<int>();

        // Sanity check: ACE Pro has 4 slots max, be generous with 16
        if (slot_count < 0 || slot_count > 16) {
            spdlog::warn("[ACE] Ignoring invalid slot_count: {}", slot_count);
            return;
        }

        system_info_.total_slots = slot_count;

        // ACE is a single-unit system - ensure we have one unit
        if (system_info_.units.empty()) {
            system_info_.units.emplace_back();
            system_info_.units[0].name = "ACE Pro";
            system_info_.units[0].unit_index = 0;
            system_info_.units[0].connected = true;
        }

        auto& unit = system_info_.units[0];
        unit.slot_count = slot_count;

        // Initialize slots if not already done
        if (unit.slots.size() != static_cast<size_t>(slot_count)) {
            unit.slots.resize(static_cast<size_t>(slot_count));
            for (int i = 0; i < slot_count; ++i) {
                unit.slots[static_cast<size_t>(i)].slot_index = i;
                unit.slots[static_cast<size_t>(i)].global_index = i;
                unit.slots[static_cast<size_t>(i)].status = SlotStatus::UNKNOWN;
            }
        }
    }

    spdlog::info("[ACE] Detected: {} v{} with {} slots", system_info_.type_name,
                 system_info_.version, system_info_.total_slots);
}

bool AmsBackendAce::parse_status_response(const json& data) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    bool changed = false;

    // Parse loaded slot
    if (data.contains("loaded_slot") && data["loaded_slot"].is_number_integer()) {
        int slot = data["loaded_slot"].get<int>();
        if (slot != system_info_.current_slot) {
            system_info_.current_slot = slot;
            system_info_.current_tool = slot; // 1:1 mapping
            changed = true;
        }

        bool loaded = (slot >= 0);
        if (loaded != system_info_.filament_loaded) {
            system_info_.filament_loaded = loaded;
            changed = true;
        }
    }

    // Parse action state
    if (data.contains("action") && data["action"].is_string()) {
        std::string action_str = data["action"].get<std::string>();
        AmsAction action = AmsAction::IDLE;

        if (action_str == "loading") {
            action = AmsAction::LOADING;
        } else if (action_str == "unloading") {
            action = AmsAction::UNLOADING;
        } else if (action_str == "error") {
            action = AmsAction::ERROR;
        } else if (action_str == "drying") {
            action = AmsAction::IDLE; // Drying doesn't block filament operations
        }

        if (action != system_info_.action) {
            system_info_.action = action;
            changed = true;
        }
    }

    // Parse dryer state
    if (data.contains("dryer") && data["dryer"].is_object()) {
        const auto& dryer = data["dryer"];

        if (dryer.contains("active") && dryer["active"].is_boolean()) {
            dryer_info_.active = dryer["active"].get<bool>();
        }
        if (dryer.contains("current_temp") && dryer["current_temp"].is_number()) {
            dryer_info_.current_temp_c = dryer["current_temp"].get<float>();
        }
        if (dryer.contains("target_temp") && dryer["target_temp"].is_number()) {
            dryer_info_.target_temp_c = dryer["target_temp"].get<float>();
        }
        if (dryer.contains("remaining_minutes") && dryer["remaining_minutes"].is_number_integer()) {
            dryer_info_.remaining_min = dryer["remaining_minutes"].get<int>();
        }
        if (dryer.contains("duration_minutes") && dryer["duration_minutes"].is_number_integer()) {
            dryer_info_.duration_min = dryer["duration_minutes"].get<int>();
        }
    }

    return changed;
}

bool AmsBackendAce::parse_slots_response(const json& data) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    bool changed = false;

    if (!data.contains("slots") || !data["slots"].is_array()) {
        return false;
    }

    const auto& slots_data = data["slots"];

    // Sanity check: ACE Pro has 4 slots max, be generous with 16
    if (slots_data.size() > 16) {
        spdlog::warn("[ACE] Ignoring excessive slot count: {}", slots_data.size());
        return false;
    }

    // Ensure we have a unit to store slots
    if (system_info_.units.empty()) {
        system_info_.units.emplace_back();
        system_info_.units[0].name = "ACE Pro";
        system_info_.units[0].unit_index = 0;
        system_info_.units[0].connected = true;
    }

    auto& unit = system_info_.units[0];

    // Resize if needed
    if (unit.slots.size() != slots_data.size()) {
        unit.slots.resize(slots_data.size());
        unit.slot_count = static_cast<int>(slots_data.size());
        system_info_.total_slots = static_cast<int>(slots_data.size());
        changed = true;
    }

    for (size_t i = 0; i < slots_data.size(); ++i) {
        const auto& slot_json = slots_data[i];

        // Skip non-object entries
        if (!slot_json.is_object()) {
            continue;
        }

        auto& slot = unit.slots[i];

        slot.slot_index = static_cast<int>(i);
        slot.global_index = static_cast<int>(i);

        // Parse status
        if (slot_json.contains("status") && slot_json["status"].is_string()) {
            std::string status_str = slot_json["status"].get<std::string>();
            SlotStatus status = SlotStatus::UNKNOWN;

            if (status_str == "empty") {
                status = SlotStatus::EMPTY;
            } else if (status_str == "available" || status_str == "loaded") {
                status = SlotStatus::AVAILABLE;
            }

            if (status != slot.status) {
                slot.status = status;
                changed = true;
            }
        }

        // Parse color
        if (slot_json.contains("color") && slot_json["color"].is_string()) {
            std::string color_str = slot_json["color"].get<std::string>();
            // Color is typically hex like "#FF0000" or "0xFF0000"
            uint32_t color = 0;
            if (!color_str.empty()) {
                try {
                    // Remove leading # or 0x
                    if (color_str[0] == '#') {
                        color_str = color_str.substr(1);
                    } else if (color_str.size() > 2 && color_str[0] == '0' &&
                               (color_str[1] == 'x' || color_str[1] == 'X')) {
                        color_str = color_str.substr(2);
                    }
                    color = static_cast<uint32_t>(std::stoul(color_str, nullptr, 16));
                } catch (const std::exception& e) {
                    spdlog::debug("[ACE] Failed to parse color '{}': {}", color_str, e.what());
                }
            }
            if (color != slot.color_rgb) {
                slot.color_rgb = color;
                changed = true;
            }
        }

        // Parse material
        if (slot_json.contains("material") && slot_json["material"].is_string()) {
            std::string material = slot_json["material"].get<std::string>();
            if (material != slot.material) {
                slot.material = material;
                changed = true;
            }
        }

        // Parse temperature range
        if (slot_json.contains("temp_min") && slot_json["temp_min"].is_number_integer()) {
            slot.nozzle_temp_min = slot_json["temp_min"].get<int>();
        }
        if (slot_json.contains("temp_max") && slot_json["temp_max"].is_number_integer()) {
            slot.nozzle_temp_max = slot_json["temp_max"].get<int>();
        }
    }

    return changed;
}

// ============================================================================
// Helpers
// ============================================================================

AmsError AmsBackendAce::execute_gcode(const std::string& gcode) {
    if (!api_) {
        return AmsErrorHelper::not_connected("No API connection");
    }

    // Execute via MoonrakerAPI
    api_->execute_gcode(
        gcode, []() { spdlog::debug("[ACE] G-code executed successfully"); },
        [gcode](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("[ACE] G-code response timed out (may still be running): {}",
                             gcode);
            } else {
                spdlog::error("[ACE] G-code '{}' failed: {}", gcode, err.message);
            }
        },
        MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);

    return AmsErrorHelper::success();
}

AmsError AmsBackendAce::check_preconditions() const {
    if (!running_.load()) {
        return AmsError(AmsResult::NOT_INITIALIZED, "ACE backend not running",
                        "Backend not ready", "Start the backend first");
    }

    std::lock_guard<std::mutex> lock(state_mutex_);

    if (system_info_.action == AmsAction::LOADING || system_info_.action == AmsAction::UNLOADING) {
        return AmsErrorHelper::busy("filament operation");
    }

    return AmsErrorHelper::success();
}

AmsError AmsBackendAce::validate_slot_index(int slot_index) const {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (slot_index < 0 || slot_index >= system_info_.total_slots) {
        return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
    }

    return AmsErrorHelper::success();
}

bool AmsBackendAce::interruptible_sleep(int ms) {
    std::unique_lock<std::mutex> lock(stop_mutex_);
    return !stop_cv_.wait_for(lock, std::chrono::milliseconds(ms),
                              [this] { return stop_requested_.load(); });
}

// ============================================================================
// Device Actions
// ============================================================================

std::vector<helix::printer::DeviceSection> AmsBackendAce::get_device_sections() const {
    using DS = helix::printer::DeviceSection;
    return {
        DS{"filament_control", "Filament Control", 0, "Manual feed and retract operations"},
        DS{"maintenance", "Maintenance", 1, "Feed assist and debug tools"},
    };
}

std::vector<helix::printer::DeviceAction> AmsBackendAce::get_device_actions() const {
    using DA = helix::printer::DeviceAction;
    using AT = helix::printer::ActionType;
    return {
        DA{"ace_manual_feed", "Manual Feed", "", "filament_control",
           "Feed filament from current slot",
           AT::BUTTON, {}, {}, 0, 100, "", -1, true, ""},
        DA{"ace_manual_retract", "Manual Retract", "", "filament_control",
           "Retract filament to current slot",
           AT::BUTTON, {}, {}, 0, 100, "", -1, true, ""},
        DA{"ace_feed_assist_toggle", "Feed Assist", "", "maintenance",
           "Enable feed assist for active slot during printing",
           AT::TOGGLE, {}, {}, 0, 100, "", -1, true, ""},
    };
}

AmsError AmsBackendAce::execute_device_action(const std::string& action_id,
                                                  const std::any& value) {
    if (action_id == "ace_manual_feed") {
        int slot = get_current_slot();
        if (slot < 0) slot = 0;
        return execute_gcode("ACE_FEED INDEX=" + std::to_string(slot) + " LENGTH=50 SPEED=50");
    }

    if (action_id == "ace_manual_retract") {
        int slot = get_current_slot();
        if (slot < 0) slot = 0;
        return execute_gcode("ACE_RETRACT INDEX=" + std::to_string(slot) + " LENGTH=50 SPEED=50");
    }

    if (action_id == "ace_feed_assist_toggle") {
        int slot = get_current_slot();
        if (slot < 0) slot = 0;

        bool enable = true;
        if (value.has_value()) {
            try {
                enable = std::any_cast<bool>(value);
            } catch (const std::bad_any_cast&) {
                // Default to enable if cast fails
            }
        }

        if (enable) {
            return execute_gcode("ACE_ENABLE_FEED_ASSIST INDEX=" + std::to_string(slot));
        } else {
            return execute_gcode("ACE_DISABLE_FEED_ASSIST INDEX=" + std::to_string(slot));
        }
    }

    return AmsErrorHelper::not_supported("Unknown ACE action: " + action_id);
}
