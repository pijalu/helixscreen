// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_IFS

#include "ams_backend_ad5x_ifs.h"

#include "moonraker_api.h"
#include "moonraker_client.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <regex>
#include <sstream>

using json = nlohmann::json;

AmsBackendAd5xIfs::AmsBackendAd5xIfs(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Fill tool map with UNMAPPED_PORT
    tool_map_.fill(UNMAPPED_PORT);
    port_presence_.fill(false);

    // Initialize SlotRegistry with 4 ports in a single unit
    std::vector<std::string> slot_names;
    for (int i = 1; i <= NUM_PORTS; ++i) {
        slot_names.push_back(std::to_string(i));
    }
    slots_.initialize("IFS", slot_names);

    // Set system info capabilities
    system_info_.type = AmsType::AD5X_IFS;
    system_info_.type_name = "AD5X IFS";
    system_info_.total_slots = NUM_PORTS;
    system_info_.supports_bypass = true;
    system_info_.supports_tool_mapping = true;
    system_info_.supports_endless_spool = false;
    system_info_.supports_purge = false;
}

AmsBackendAd5xIfs::~AmsBackendAd5xIfs() = default;

// --- Lifecycle ---

void AmsBackendAd5xIfs::on_started() {
    // Query initial state from printer
    if (!client_) return;

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query",
        json{{"objects", json{
            {"save_variables", nullptr},
            // lessWaste plugin: per-port filament switch sensors
            {"filament_switch_sensor _ifs_port_sensor_1", nullptr},
            {"filament_switch_sensor _ifs_port_sensor_2", nullptr},
            {"filament_switch_sensor _ifs_port_sensor_3", nullptr},
            {"filament_switch_sensor _ifs_port_sensor_4", nullptr},
            // Shared: head switch sensor (both lessWaste and native ZMOD)
            {"filament_switch_sensor head_switch_sensor", nullptr},
            // Native ZMOD IFS: single motion sensor (replaces per-port sensors)
            {"filament_motion_sensor ifs_motion_sensor", nullptr}
        }}},
        [this, token](const json& response) {
            if (token.expired()) return;
            if (response.contains("result") && response["result"].contains("status")) {
                handle_status_update(response["result"]["status"]);
            }

            // If save_variables didn't populate any colors, we're likely on native
            // ZMOD which stores color/type in FlashForge config, not save_variables.
            // Fall back to querying GET_ZCOLOR for initial slot data.
            bool need_zcolor = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                need_zcolor = !has_ifs_vars_;
            }
            if (need_zcolor) {
                spdlog::info("{} No _IFS_VARS data found, querying GET_ZCOLOR (native ZMOD)",
                             backend_log_tag());
                query_zcolor_fallback();
            }
        });
}

// --- Status parsing ---

void AmsBackendAd5xIfs::handle_status_update(const json& notification) {
    // notify_status_update has format: { "method": "notify_status_update", "params": [{ ... },
    // timestamp] }
    // Initial query response sends unwrapped status directly — handle both formats.
    const json* status = &notification;
    if (notification.contains("params") && notification["params"].is_array() &&
        !notification["params"].empty()) {
        status = &notification["params"][0];
        if (!status->is_object()) {
            return;
        }
    }

    std::unique_lock<std::mutex> lock(mutex_);

    bool state_changed = false;

    // Parse save_variables if present
    if (status->contains("save_variables")) {
        const auto& sv = (*status)["save_variables"];
        if (sv.contains("variables") && sv["variables"].is_object()) {
            parse_save_variables(sv["variables"]);
            state_changed = true;
        }
    }

    // Parse per-port filament sensors
    // Leading space in sensor name is intentional — Klipper object naming convention
    for (int port = 1; port <= NUM_PORTS; ++port) {
        std::string key = "filament_switch_sensor _ifs_port_sensor_" + std::to_string(port);
        if (status->contains(key)) {
            const auto& sensor = (*status)[key];
            if (sensor.contains("filament_detected") && sensor["filament_detected"].is_boolean()) {
                parse_port_sensor(port, sensor["filament_detected"].get<bool>());
                state_changed = true;
            }
        }
    }

    // Native ZMOD IFS: single motion sensor replaces per-port presence sensors.
    // Maps to head_filament_ since it detects filament at the toolhead.
    if (status->contains("filament_motion_sensor ifs_motion_sensor")) {
        const auto& motion = (*status)["filament_motion_sensor ifs_motion_sensor"];
        if (motion.contains("filament_detected") && motion["filament_detected"].is_boolean()) {
            bool detected = motion["filament_detected"].get<bool>();
            parse_head_sensor(detected);
            detect_load_unload_completion(detected);
            state_changed = true;
        }
    }

    // Parse head sensor
    if (status->contains("filament_switch_sensor head_switch_sensor")) {
        const auto& head = (*status)["filament_switch_sensor head_switch_sensor"];
        if (head.contains("filament_detected") && head["filament_detected"].is_boolean()) {
            bool detected = head["filament_detected"].get<bool>();
            parse_head_sensor(detected);
            detect_load_unload_completion(detected);
            state_changed = true;
        }
    }

    // Update system info from cached state
    if (state_changed) {
        system_info_.current_tool = active_tool_;
        system_info_.filament_loaded = head_filament_;

        // Map current tool to current slot
        if (active_tool_ >= 0 && active_tool_ < TOOL_MAP_SIZE) {
            int port = tool_map_[static_cast<size_t>(active_tool_)];
            if (port >= 1 && port <= NUM_PORTS) {
                system_info_.current_slot = port - 1;
            } else {
                system_info_.current_slot = -1;
            }
        } else {
            system_info_.current_slot = -1;
        }

        // Update all slot states
        for (int i = 0; i < NUM_PORTS; ++i) {
            update_slot_from_state(i);
        }
    }

    // Check for stuck operations on every status update
    check_action_timeout();

    lock.unlock();

    if (state_changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

void AmsBackendAd5xIfs::parse_save_variables(const json& vars) {
    // Auto-detect variable prefix: lessWaste/zmod uses "less_waste_*", bambufy uses "bambufy_*".
    // Check once per status update — the prefix can't change at runtime, but we may not
    // see both sets of variables in the initial query.
    if (vars.contains("bambufy_colors") || vars.contains("bambufy_tools")) {
        if (var_prefix_ != "bambufy") {
            var_prefix_ = "bambufy";
            spdlog::info("{} Detected bambufy variable prefix", backend_log_tag());
        }
        has_ifs_vars_ = true;
    } else if (vars.contains("less_waste_colors") || vars.contains("less_waste_tools")) {
        if (var_prefix_ != "less_waste") {
            var_prefix_ = "less_waste";
            spdlog::info("{} Detected lessWaste variable prefix", backend_log_tag());
        }
        has_ifs_vars_ = true;
    }

    const std::string p = var_prefix_;

    // Colors: array of hex strings ["FF0000", "00FF00", ...]
    if (vars.contains(p + "_colors") && vars[p + "_colors"].is_array()) {
        const auto& colors = vars[p + "_colors"];
        for (size_t i = 0; i < std::min(colors.size(), static_cast<size_t>(NUM_PORTS)); ++i) {
            if (colors[i].is_string()) {
                colors_[i] = colors[i].get<std::string>();
            }
        }
    }

    // Materials: array of type strings ["PLA", "PETG", ...]
    if (vars.contains(p + "_types") && vars[p + "_types"].is_array()) {
        const auto& types = vars[p + "_types"];
        for (size_t i = 0; i < std::min(types.size(), static_cast<size_t>(NUM_PORTS)); ++i) {
            if (types[i].is_string()) {
                materials_[i] = types[i].get<std::string>();
            }
        }
    }

    // Tool mapping: 16-element array, index=tool, value=port (1-4, 5=unmapped)
    if (vars.contains(p + "_tools") && vars[p + "_tools"].is_array()) {
        const auto& tools = vars[p + "_tools"];
        for (size_t i = 0; i < std::min(tools.size(), static_cast<size_t>(TOOL_MAP_SIZE)); ++i) {
            if (tools[i].is_number_integer()) {
                tool_map_[i] = tools[i].get<int>();
            }
        }
    }

    // Current tool (-1 = none, 0-15 = tool number)
    if (vars.contains(p + "_current_tool") &&
        vars[p + "_current_tool"].is_number_integer()) {
        active_tool_ = vars[p + "_current_tool"].get<int>();
    }

    // External/bypass mode (0 or 1)
    if (vars.contains(p + "_external") && vars[p + "_external"].is_number_integer()) {
        external_mode_ = (vars[p + "_external"].get<int>() != 0);
    }

    // Rebuild SlotRegistry tool mapping from IFS tool_map_
    for (int i = 0; i < NUM_PORTS; ++i) {
        int tool = find_first_tool_for_port(i + 1); // port is 1-based
        slots_.set_tool_mapping(i, tool);
    }

    // Sync all slots from cached state
    for (int i = 0; i < NUM_PORTS; ++i) {
        update_slot_from_state(i);
    }
}

void AmsBackendAd5xIfs::parse_port_sensor(int port_1based, bool detected) {
    int slot = port_1based - 1;
    if (slot >= 0 && slot < NUM_PORTS) {
        has_per_port_sensors_ = true;
        port_presence_[static_cast<size_t>(slot)] = detected;
    }
}

void AmsBackendAd5xIfs::parse_head_sensor(bool detected) {
    head_filament_ = detected;
}

void AmsBackendAd5xIfs::update_slot_from_state(int slot_index) {
    if (slot_index < 0 || slot_index >= NUM_PORTS) return;

    auto* entry = slots_.get_mut(slot_index);
    if (!entry) return;

    auto idx = static_cast<size_t>(slot_index);

    // Color: parse hex string to uint32_t
    if (!colors_[idx].empty()) {
        try {
            entry->info.color_rgb =
                static_cast<uint32_t>(std::stoul(colors_[idx], nullptr, 16));
        } catch (...) {
            // Invalid hex — leave color unchanged
        }
    }

    // Material
    entry->info.material = materials_[idx];

    // Status based on sensor and active state
    bool is_active_slot = (system_info_.current_slot == slot_index);
    bool has_filament = port_presence_[idx];

    // Native ZMOD IFS has no per-port sensors. For the active slot, infer
    // presence from the head sensor so the UI doesn't show all slots as EMPTY.
    if (!has_per_port_sensors_ && is_active_slot && head_filament_) {
        has_filament = true;
    }

    if (has_filament && is_active_slot && head_filament_) {
        entry->info.status = SlotStatus::LOADED;
    } else if (has_filament) {
        entry->info.status = SlotStatus::AVAILABLE;
    } else {
        entry->info.status = SlotStatus::EMPTY;
    }

    // Reverse tool mapping: find first tool that maps to this port
    entry->info.mapped_tool = find_first_tool_for_port(slot_index + 1);
}

// --- State queries ---

AmsSystemInfo AmsBackendAd5xIfs::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check for stuck operations on every UI poll, not just status updates.
    // This catches cases where the printer goes silent (network drop, Klipper crash).
    const_cast<AmsBackendAd5xIfs*>(this)->check_action_timeout();

    auto info = slots_.build_system_info();

    // Overlay our cached system info
    info.type = system_info_.type;
    info.type_name = system_info_.type_name;
    info.total_slots = system_info_.total_slots;
    info.current_tool = system_info_.current_tool;
    info.current_slot = system_info_.current_slot;
    info.filament_loaded = system_info_.filament_loaded;
    info.action = system_info_.action;
    info.supports_bypass = system_info_.supports_bypass;
    info.supports_tool_mapping = system_info_.supports_tool_mapping;
    info.supports_endless_spool = system_info_.supports_endless_spool;
    info.supports_purge = system_info_.supports_purge;

    // Replace registry's tool map with IFS-specific 16-entry mapping
    info.tool_to_slot_map.clear();
    for (int t = 0; t < TOOL_MAP_SIZE; ++t) {
        int port = tool_map_[static_cast<size_t>(t)];
        if (port >= 1 && port <= NUM_PORTS) {
            info.tool_to_slot_map.push_back(port - 1);
        } else {
            info.tool_to_slot_map.push_back(-1);
        }
    }

    return info;
}

SlotInfo AmsBackendAd5xIfs::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* entry = slots_.get(slot_index);
    if (!entry) {
        return SlotInfo{};
    }
    return entry->info;
}

bool AmsBackendAd5xIfs::is_bypass_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return external_mode_;
}

// --- Path visualization ---

PathSegment AmsBackendAd5xIfs::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (head_filament_) {
        return PathSegment::NOZZLE;
    }
    // Check if active tool's port has filament
    if (active_tool_ >= 0 && active_tool_ < TOOL_MAP_SIZE) {
        int port = tool_map_[static_cast<size_t>(active_tool_)];
        if (port >= 1 && port <= NUM_PORTS &&
            port_presence_[static_cast<size_t>(port - 1)]) {
            return PathSegment::LANE;
        }
    }
    return PathSegment::NONE;
}

PathSegment AmsBackendAd5xIfs::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot_index < 0 || slot_index >= NUM_PORTS) {
        return PathSegment::NONE;
    }

    auto idx = static_cast<size_t>(slot_index);
    if (!port_presence_[idx]) {
        return PathSegment::NONE;
    }

    bool is_active = (system_info_.current_slot == slot_index);
    if (is_active && head_filament_) {
        return PathSegment::NOZZLE;
    }

    // Filament is in the slot but not at head — it's at the spool/lane
    if (is_active) {
        return PathSegment::LANE;
    }
    return PathSegment::SPOOL;
}

PathSegment AmsBackendAd5xIfs::infer_error_segment() const {
    // IFS doesn't report fine-grained error segments
    return PathSegment::NONE;
}

// --- Filament operations ---

AmsError AmsBackendAd5xIfs::load_filament(int slot_index) {
    if (!validate_slot_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
    }
    auto err = check_preconditions();
    if (!err.success()) return err;

    int port = slot_index + 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::LOADING;
        action_start_time_ = std::chrono::steady_clock::now();
    }
    spdlog::info("{} Loading filament from port {}", backend_log_tag(), port);
    return ensure_homed_then("INSERT_PRUTOK_IFS PRUTOK=" + std::to_string(port));
}

AmsError AmsBackendAd5xIfs::unload_filament(int slot_index) {
    auto err = check_preconditions();
    if (!err.success()) return err;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::UNLOADING;
        action_start_time_ = std::chrono::steady_clock::now();
    }

    std::string unload_cmd;
    if (slot_index >= 0 && slot_index < NUM_PORTS) {
        int port = slot_index + 1;
        spdlog::info("{} Unloading filament from port {}", backend_log_tag(), port);
        unload_cmd = "REMOVE_PRUTOK_IFS PRUTOK=" + std::to_string(port);
    } else {
        spdlog::info("{} Unloading current filament", backend_log_tag());
        unload_cmd = "IFS_REMOVE_PRUTOK";
    }

    return ensure_homed_then(std::move(unload_cmd));
}

AmsError AmsBackendAd5xIfs::select_slot(int slot_index) {
    if (!validate_slot_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
    }
    auto err = check_preconditions();
    if (!err.success()) return err;

    int port = slot_index + 1;
    spdlog::info("{} Selecting port {}", backend_log_tag(), port);
    return execute_gcode("SET_EXTRUDER_SLOT SLOT=" + std::to_string(port));
}

AmsError AmsBackendAd5xIfs::change_tool(int tool_number) {
    if (tool_number < 0 || tool_number >= TOOL_MAP_SIZE) {
        return AmsErrorHelper::invalid_slot(tool_number, TOOL_MAP_SIZE - 1);
    }
    auto err = check_preconditions();
    if (!err.success()) return err;

    int port;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        port = tool_map_[static_cast<size_t>(tool_number)];
    }

    if (port < 1 || port > NUM_PORTS) {
        return AmsErrorHelper::invalid_parameter("Tool T" + std::to_string(tool_number) +
                                          " is not mapped to any port");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::LOADING;
        action_start_time_ = std::chrono::steady_clock::now();
    }
    spdlog::info("{} Changing to tool T{} (port {})", backend_log_tag(), tool_number, port);
    return ensure_homed_then("A_CHANGE_FILAMENT CHANNEL=" + std::to_string(port));
}

// --- Recovery ---

AmsError AmsBackendAd5xIfs::recover() {
    auto err = check_preconditions();
    if (!err.success()) return err;

    // IFS_UNLOCK resets the IFS driver state machine — safest recovery command
    spdlog::info("{} Recovery: IFS_UNLOCK", backend_log_tag());
    return execute_gcode("IFS_UNLOCK");
}

AmsError AmsBackendAd5xIfs::reset() {
    auto err = check_preconditions();
    if (!err.success()) return err;

    // IFS_UNLOCK resets the IFS driver — F15 (hard reset) is not exposed as a safe macro
    spdlog::info("{} Reset: IFS_UNLOCK", backend_log_tag());
    return execute_gcode("IFS_UNLOCK");
}

AmsError AmsBackendAd5xIfs::cancel() {
    auto err = check_preconditions();
    if (!err.success()) return err;

    // IFS_UNLOCK to abort current operation
    spdlog::info("{} Cancel: IFS_UNLOCK", backend_log_tag());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::IDLE;
    }
    return execute_gcode("IFS_UNLOCK");
}

// --- Configuration ---

AmsError AmsBackendAd5xIfs::set_slot_info(int slot_index, const SlotInfo& info, bool persist) {
    if (!validate_slot_index(slot_index)) {
        return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
    }

    auto idx = static_cast<size_t>(slot_index);

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Update local state
        auto* entry = slots_.get_mut(slot_index);
        if (!entry) {
            return AmsErrorHelper::invalid_slot(slot_index, NUM_PORTS - 1);
        }

        // Convert color to hex string for our cached array
        char hex[7];
        snprintf(hex, sizeof(hex), "%06X", info.color_rgb & 0xFFFFFF);
        colors_[idx] = hex;

        materials_[idx] = info.material;

        // Update entry directly
        entry->info.color_rgb = info.color_rgb;
        entry->info.material = info.material;
        entry->info.spoolman_id = info.spoolman_id;
        entry->info.remaining_weight_g = info.remaining_weight_g;
        entry->info.total_weight_g = info.total_weight_g;
    }

    if (persist) {
        bool use_ifs_vars;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            use_ifs_vars = has_ifs_vars_;
        }

        if (use_ifs_vars) {
            // lessWaste/bambufy: bulk update all slots via _IFS_VARS macro
            std::string color_val, type_val;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                color_val = build_color_list_value();
                type_val = build_type_list_value();
            }

            auto err = write_ifs_var("colors", color_val);
            if (!err.success()) return err;

            err = write_ifs_var("types", type_val);
            if (!err.success()) return err;
        } else {
            // Native ZMOD: per-slot update via CHANGE_ZCOLOR
            auto err = write_zcolor(slot_index);
            if (!err.success()) return err;
        }
    }

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendAd5xIfs::set_tool_mapping(int tool_number, int slot_index) {
    if (tool_number < 0 || tool_number >= TOOL_MAP_SIZE) {
        return AmsErrorHelper::invalid_parameter("Invalid tool number");
    }

    int port = (slot_index >= 0 && slot_index < NUM_PORTS) ? (slot_index + 1) : UNMAPPED_PORT;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tool_map_[static_cast<size_t>(tool_number)] = port;

        // Update SlotRegistry reverse mapping
        for (int i = 0; i < NUM_PORTS; ++i) {
            int tool = find_first_tool_for_port(i + 1);
            slots_.set_tool_mapping(i, tool);
        }
    }

    // Persist tool mapping (only for lessWaste/bambufy — native ZMOD manages
    // tool mapping internally via the COLOR/SET_ZCOLOR dialog)
    if (!has_ifs_vars_) {
        return AmsErrorHelper::success();
    }

    std::string tools_val;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tools_val = build_tool_map_value();
    }

    return write_ifs_var("tools", tools_val);
}

// --- Bypass ---

AmsError AmsBackendAd5xIfs::enable_bypass() {
    auto err = check_preconditions();
    if (!err.success()) return err;

    spdlog::info("{} Enabling bypass (external) mode", backend_log_tag());
    if (!has_ifs_vars_) {
        // Native ZMOD has no external/bypass mode variable — update local state only
        std::lock_guard<std::mutex> lock(mutex_);
        external_mode_ = true;
        return AmsErrorHelper::success();
    }
    return write_ifs_var("external", "1");
}

AmsError AmsBackendAd5xIfs::disable_bypass() {
    auto err = check_preconditions();
    if (!err.success()) return err;

    spdlog::info("{} Disabling bypass (external) mode", backend_log_tag());
    if (!has_ifs_vars_) {
        std::lock_guard<std::mutex> lock(mutex_);
        external_mode_ = false;
        return AmsErrorHelper::success();
    }
    return write_ifs_var("external", "0");
}

// --- Private helpers ---

std::string AmsBackendAd5xIfs::build_color_list_value() const {
    // Build Python list literal for _IFS_VARS macro.
    // Outer double quotes delimit the G-code parameter value (Klipper strips them).
    // _IFS_VARS passes the inner content to SAVE_VARIABLE, adding its own quoting.
    // Single quotes for string elements inside the list.
    // Example: "['FF0000', '00FF00', '0000FF', 'FFFFFF']"
    std::ostringstream ss;
    ss << "\"[";
    for (int i = 0; i < NUM_PORTS; ++i) {
        if (i > 0) ss << ", ";
        ss << "'" << colors_[static_cast<size_t>(i)] << "'";
    }
    ss << "]\"";
    return ss.str();
}

std::string AmsBackendAd5xIfs::build_type_list_value() const {
    std::ostringstream ss;
    ss << "\"[";
    for (int i = 0; i < NUM_PORTS; ++i) {
        if (i > 0) ss << ", ";
        ss << "'" << materials_[static_cast<size_t>(i)] << "'";
    }
    ss << "]\"";
    return ss.str();
}

std::string AmsBackendAd5xIfs::build_tool_map_value() const {
    // Integer list — no quotes around elements.
    // Example: "[1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]"
    std::ostringstream ss;
    ss << "\"[";
    for (int i = 0; i < TOOL_MAP_SIZE; ++i) {
        if (i > 0) ss << ", ";
        ss << tool_map_[static_cast<size_t>(i)];
    }
    ss << "]\"";
    return ss.str();
}

AmsError AmsBackendAd5xIfs::write_ifs_var(const std::string& key,
                                            const std::string& value) {
    if (!api_) {
        return AmsErrorHelper::invalid_parameter("No API connection");
    }

    // Use _IFS_VARS macro to persist state — works for both lessWaste and bambufy.
    // The macro updates in-memory gcode variables AND writes SAVE_VARIABLE with the
    // correct prefix automatically.
    std::string gcode = "_IFS_VARS " + key + "=" + value;
    spdlog::debug("{} Writing IFS var: {} = {}", backend_log_tag(), key, value);
    return execute_gcode(gcode);
}

AmsError AmsBackendAd5xIfs::write_zcolor(int slot_index) {
    if (!api_) {
        return AmsErrorHelper::invalid_parameter("No API connection");
    }

    auto idx = static_cast<size_t>(slot_index);
    int slot_1based = slot_index + 1;

    std::string hex, material;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hex = colors_[idx];
        material = materials_[idx];
    }

    if (hex.empty()) hex = "808080";
    if (material.empty()) material = "PLA";

    // Native ZMOD: CHANGE_ZCOLOR SLOT=N HEX=RRGGBB TYPE=MATERIAL
    std::string gcode = "CHANGE_ZCOLOR SLOT=" + std::to_string(slot_1based) +
                        " HEX=" + hex + " TYPE=" + material;
    spdlog::info("{} Writing slot {} via CHANGE_ZCOLOR (native ZMOD)", backend_log_tag(),
                 slot_1based);
    return execute_gcode(gcode);
}

void AmsBackendAd5xIfs::query_zcolor_fallback() {
    if (!client_ || !api_) return;

    // Accumulate gcode response lines, then parse when prompt_show arrives
    auto lines = std::make_shared<std::vector<std::string>>();
    auto token = lifetime_.token();

    static const std::string handler_name = "ifs_zcolor_query";

    client_->register_method_callback(
        "notify_gcode_response", handler_name,
        [this, lines, token](const json& msg) {
            if (token.expired()) return;

            // notify_gcode_response: {"method": "notify_gcode_response", "params": ["line"]}
            std::string line;
            if (msg.contains("params") && msg["params"].is_array() &&
                !msg["params"].empty() && msg["params"][0].is_string()) {
                line = msg["params"][0].get<std::string>();
            } else if (msg.is_array() && !msg.empty() && msg[0].is_string()) {
                line = msg[0].get<std::string>();
            } else {
                return;
            }

            lines->push_back(line);

            // prompt_show marks the end of the GET_ZCOLOR response
            if (line.find("action:prompt_show") != std::string::npos) {
                client_->unregister_method_callback("notify_gcode_response", handler_name);

                std::string combined;
                for (const auto& l : *lines) {
                    combined += l + "\n";
                }
                parse_zcolor_response(combined);
            }
        });

    // Execute GET_ZCOLOR — non-silent mode produces prompt buttons with slot data
    execute_gcode("GET_ZCOLOR");
}

void AmsBackendAd5xIfs::parse_zcolor_response(const std::string& response) {
    // Parse lines like:
    //   // action:prompt_button 1: PLA|RUN_ZCOLOR SLOT=1 HEX=FFFFFF TYPE=PLA|primary|FFFFFF
    // Extract SLOT, HEX, TYPE from the RUN_ZCOLOR command embedded in each button.
    static const std::regex button_re(
        R"(RUN_ZCOLOR\s+SLOT=(\d+)\s+HEX=([0-9A-Fa-f]{6})\s+TYPE=([^|\s]+))");

    std::istringstream stream(response);
    std::string line;
    int parsed_count = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        while (std::getline(stream, line)) {
            std::smatch match;
            if (std::regex_search(line, match, button_re)) {
                int slot_1based = std::stoi(match[1].str());
                std::string hex = match[2].str();
                std::string type = match[3].str();

                // Convert to uppercase hex
                for (auto& c : hex) c = static_cast<char>(toupper(c));

                int idx = slot_1based - 1; // ZMOD slots are 1-based
                if (idx >= 0 && idx < NUM_PORTS) {
                    colors_[static_cast<size_t>(idx)] = hex;
                    materials_[static_cast<size_t>(idx)] = type;
                    update_slot_from_state(idx);
                    ++parsed_count;
                }
            }
        }
    } // release lock before emit_event (which also takes mutex_)

    if (parsed_count > 0) {
        spdlog::info("{} Loaded {} slots from GET_ZCOLOR (native ZMOD)", backend_log_tag(),
                     parsed_count);
        emit_event(EVENT_STATE_CHANGED);
    } else {
        spdlog::warn("{} GET_ZCOLOR returned no parseable slot data", backend_log_tag());
    }
}

void AmsBackendAd5xIfs::detect_load_unload_completion(bool head_detected) {
    if (system_info_.action == AmsAction::LOADING && head_detected) {
        system_info_.action = AmsAction::IDLE;
        spdlog::info("{} Load complete (head sensor triggered)", backend_log_tag());
    } else if (system_info_.action == AmsAction::UNLOADING && !head_detected) {
        system_info_.action = AmsAction::IDLE;
        spdlog::info("{} Unload complete (head sensor cleared)", backend_log_tag());
    }
}

int AmsBackendAd5xIfs::find_first_tool_for_port(int port_1based) const {
    for (int t = 0; t < TOOL_MAP_SIZE; ++t) {
        if (tool_map_[static_cast<size_t>(t)] == port_1based) {
            return t;
        }
    }
    return -1; // No tool mapped to this port
}

bool AmsBackendAd5xIfs::validate_slot_index(int slot_index) const {
    return slot_index >= 0 && slot_index < NUM_PORTS;
}

// ensure_homed_then() provided by AmsSubscriptionBackend

void AmsBackendAd5xIfs::check_action_timeout() {
    if (system_info_.action != AmsAction::LOADING &&
        system_info_.action != AmsAction::UNLOADING) {
        return;
    }

    auto elapsed = std::chrono::steady_clock::now() - action_start_time_;
    if (elapsed >= std::chrono::seconds(ACTION_TIMEOUT_SECONDS)) {
        spdlog::warn("{} {} timed out after {}s, resetting to IDLE",
                     backend_log_tag(),
                     ams_action_to_string(system_info_.action),
                     std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        system_info_.action = AmsAction::IDLE;
    }
}

#endif // HELIX_HAS_IFS
