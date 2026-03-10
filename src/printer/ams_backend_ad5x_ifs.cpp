// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_ad5x_ifs.h"

#include "moonraker_api.h"
#include "moonraker_client.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>

using json = nlohmann::json;

AmsBackendAd5xIfs::AmsBackendAd5xIfs(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client),
      alive_(std::make_shared<std::atomic<bool>>(true)) {
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

AmsBackendAd5xIfs::~AmsBackendAd5xIfs() {
    alive_->store(false);
}

// --- Lifecycle ---

void AmsBackendAd5xIfs::on_started() {
    // Query initial state from printer
    if (!client_) return;

    auto alive = alive_;
    client_->send_jsonrpc(
        "printer.objects.query",
        json{{"objects", json{
            {"save_variables", nullptr},
            {"filament_switch_sensor _ifs_port_sensor_1", nullptr},
            {"filament_switch_sensor _ifs_port_sensor_2", nullptr},
            {"filament_switch_sensor _ifs_port_sensor_3", nullptr},
            {"filament_switch_sensor _ifs_port_sensor_4", nullptr},
            {"filament_switch_sensor head_switch_sensor", nullptr}
        }}},
        [this, alive](const json& response) {
            if (!alive->load()) return;
            if (response.contains("result") && response["result"].contains("status")) {
                handle_status_update(response["result"]["status"]);
            }
        });
}

// --- Status parsing ---

void AmsBackendAd5xIfs::handle_status_update(const json& notification) {
    std::unique_lock<std::mutex> lock(mutex_);

    bool state_changed = false;
    bool was_loading = (system_info_.action == AmsAction::LOADING);
    bool was_unloading = (system_info_.action == AmsAction::UNLOADING);

    // Parse save_variables if present
    if (notification.contains("save_variables")) {
        const auto& sv = notification["save_variables"];
        if (sv.contains("variables") && sv["variables"].is_object()) {
            parse_save_variables(sv["variables"]);
            state_changed = true;
        }
    }

    // Parse per-port filament sensors
    // Leading space in sensor name is intentional — Klipper object naming convention
    for (int port = 1; port <= NUM_PORTS; ++port) {
        std::string key = "filament_switch_sensor _ifs_port_sensor_" + std::to_string(port);
        if (notification.contains(key)) {
            const auto& sensor = notification[key];
            if (sensor.contains("filament_detected") && sensor["filament_detected"].is_boolean()) {
                parse_port_sensor(port, sensor["filament_detected"].get<bool>());
                state_changed = true;
            }
        }
    }

    // Parse head sensor
    if (notification.contains("filament_switch_sensor head_switch_sensor")) {
        const auto& head = notification["filament_switch_sensor head_switch_sensor"];
        if (head.contains("filament_detected") && head["filament_detected"].is_boolean()) {
            bool detected = head["filament_detected"].get<bool>();
            parse_head_sensor(detected);
            state_changed = true;

            // Detect operation completion based on head sensor state transitions
            if (was_loading && detected) {
                system_info_.action = AmsAction::IDLE;
                spdlog::info("{} Load complete (head sensor triggered)", backend_log_tag());
            } else if (was_unloading && !detected) {
                system_info_.action = AmsAction::IDLE;
                spdlog::info("{} Unload complete (head sensor cleared)", backend_log_tag());
            }
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

    lock.unlock();

    if (state_changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

void AmsBackendAd5xIfs::parse_save_variables(const json& vars) {
    // Colors: array of hex strings ["FF0000", "00FF00", ...]
    if (vars.contains("less_waste_colors") && vars["less_waste_colors"].is_array()) {
        const auto& colors = vars["less_waste_colors"];
        for (size_t i = 0; i < std::min(colors.size(), static_cast<size_t>(NUM_PORTS)); ++i) {
            if (colors[i].is_string()) {
                colors_[i] = colors[i].get<std::string>();
            }
        }
    }

    // Materials: array of type strings ["PLA", "PETG", ...]
    if (vars.contains("less_waste_types") && vars["less_waste_types"].is_array()) {
        const auto& types = vars["less_waste_types"];
        for (size_t i = 0; i < std::min(types.size(), static_cast<size_t>(NUM_PORTS)); ++i) {
            if (types[i].is_string()) {
                materials_[i] = types[i].get<std::string>();
            }
        }
    }

    // Tool mapping: 16-element array, index=tool, value=port (1-4, 5=unmapped)
    if (vars.contains("less_waste_tools") && vars["less_waste_tools"].is_array()) {
        const auto& tools = vars["less_waste_tools"];
        for (size_t i = 0; i < std::min(tools.size(), static_cast<size_t>(TOOL_MAP_SIZE)); ++i) {
            if (tools[i].is_number_integer()) {
                tool_map_[i] = tools[i].get<int>();
            }
        }
    }

    // Current tool (-1 = none, 0-15 = tool number)
    if (vars.contains("less_waste_current_tool") &&
        vars["less_waste_current_tool"].is_number_integer()) {
        active_tool_ = vars["less_waste_current_tool"].get<int>();
    }

    // External/bypass mode (0 or 1)
    if (vars.contains("less_waste_external") && vars["less_waste_external"].is_number_integer()) {
        external_mode_ = (vars["less_waste_external"].get<int>() != 0);
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
    bool has_filament = port_presence_[idx];
    bool is_active_slot = (system_info_.current_slot == slot_index);

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
    }
    spdlog::info("{} Loading filament from port {}", backend_log_tag(), port);
    return execute_gcode("_INSERT_PRUTOK_IFS PRUTOK=" + std::to_string(port));
}

AmsError AmsBackendAd5xIfs::unload_filament(int slot_index) {
    auto err = check_preconditions();
    if (!err.success()) return err;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        system_info_.action = AmsAction::UNLOADING;
    }

    if (slot_index >= 0 && slot_index < NUM_PORTS) {
        int port = slot_index + 1;
        spdlog::info("{} Unloading filament from port {}", backend_log_tag(), port);
        return execute_gcode("_REMOVE_PRUTOK_IFS PRUTOK=" + std::to_string(port));
    }

    // Default: unload current
    spdlog::info("{} Unloading current filament", backend_log_tag());
    return execute_gcode("_IFS_REMOVE_PRUTOK");
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
    }
    spdlog::info("{} Changing to tool T{} (port {})", backend_log_tag(), tool_number, port);
    return execute_gcode("_A_CHANGE_FILAMENT CHANNEL=" + std::to_string(port));
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
        // Persist colors and types to save_variables
        std::string color_val, type_val;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            color_val = build_color_list_value();
            type_val = build_type_list_value();
        }

        auto err = write_save_variable("less_waste_colors", color_val);
        if (!err.success()) return err;

        err = write_save_variable("less_waste_types", type_val);
        if (!err.success()) return err;
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

    // Persist tool mapping
    std::string tools_val;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tools_val = build_tool_map_value();
    }

    return write_save_variable("less_waste_tools", tools_val);
}

// --- Bypass ---

AmsError AmsBackendAd5xIfs::enable_bypass() {
    auto err = check_preconditions();
    if (!err.success()) return err;

    spdlog::info("{} Enabling bypass (external) mode", backend_log_tag());
    return write_save_variable("less_waste_external", "1");
}

AmsError AmsBackendAd5xIfs::disable_bypass() {
    auto err = check_preconditions();
    if (!err.success()) return err;

    spdlog::info("{} Disabling bypass (external) mode", backend_log_tag());
    return write_save_variable("less_waste_external", "0");
}

// --- Private helpers ---

std::string AmsBackendAd5xIfs::build_color_list_value() const {
    // Build Python list literal for SAVE_VARIABLE.
    // G-code quoting: outer double quotes are G-code parameter delimiters (Klipper strips them).
    // Inner content is evaluated by Python's ast.literal_eval().
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
    // Same quoting convention as build_color_list_value()
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

AmsError AmsBackendAd5xIfs::write_save_variable(const std::string& name,
                                                  const std::string& value) {
    if (!api_) {
        return AmsErrorHelper::invalid_parameter("No API connection");
    }

    // SAVE_VARIABLE G-code command writes to Klipper's save_variables file.
    // VALUE= parameter: Klipper strips outer double quotes, then ast.literal_eval()
    // parses the inner content. For strings/lists, the inner content must be valid Python.
    std::string gcode = "SAVE_VARIABLE VARIABLE=" + name + " VALUE=" + value;
    spdlog::debug("{} Writing save_variable: {} = {}", backend_log_tag(), name, value);
    return execute_gcode(gcode);
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
