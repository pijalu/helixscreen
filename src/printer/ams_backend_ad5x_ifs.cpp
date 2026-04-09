// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_IFS

#include "ams_backend_ad5x_ifs.h"

#include "moonraker_api.h"
#include "moonraker_client.h"
#include "post_op_cooldown_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <regex>
#include <sstream>
#include <thread>

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
    if (!client_)
        return;

    auto token = lifetime_.token();
    client_->send_jsonrpc(
        "printer.objects.query",
        json{{"objects",
              json{{"save_variables", nullptr},
                   // Verify _IFS_VARS macro actually exists (not just save_variables data)
                   {"gcode_macro _ifs_vars", nullptr},
                   // lessWaste plugin: per-port filament switch sensors
                   {"filament_switch_sensor _ifs_port_sensor_1", nullptr},
                   {"filament_switch_sensor _ifs_port_sensor_2", nullptr},
                   {"filament_switch_sensor _ifs_port_sensor_3", nullptr},
                   {"filament_switch_sensor _ifs_port_sensor_4", nullptr},
                   // Shared: head switch sensor (both lessWaste and native ZMOD)
                   {"filament_switch_sensor head_switch_sensor", nullptr},
                   // Native ZMOD IFS: single motion sensor (replaces per-port sensors)
                   {"filament_motion_sensor ifs_motion_sensor", nullptr}}}},
        [this, token](const json& response) {
            if (token.expired())
                return;

            // Check if the _IFS_VARS gcode macro actually exists on this printer.
            // save_variables may contain lessWaste/bambufy data from a partially installed
            // plugin, but the macro itself might not be loaded in Klipper.
            bool macro_exists = false;
            if (response.contains("result") && response["result"].contains("status")) {
                const auto& status = response["result"]["status"];
                macro_exists = status.contains("gcode_macro _ifs_vars");
                handle_status_update(status);
            }

            // Log initial state after processing query response
            {
                std::lock_guard<std::mutex> lock(mutex_);
                spdlog::debug("{} Initial query: has_ifs_vars={}, macro_exists={}, "
                              "has_per_port_sensors={}, head_filament={}, "
                              "port_presence=[{},{},{},{}], "
                              "colors=[{},{},{},{}]",
                              backend_log_tag(), has_ifs_vars_, macro_exists, has_per_port_sensors_,
                              head_filament_, port_presence_[0], port_presence_[1],
                              port_presence_[2], port_presence_[3], colors_[0], colors_[1],
                              colors_[2], colors_[3]);
            }

            // If parse_save_variables set has_ifs_vars_ but the macro doesn't exist,
            // fall back to native ZMOD. This happens when lessWaste/bambufy plugins
            // are partially installed (save_variables data exists but macros aren't loaded).
            bool need_zcolor = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (has_ifs_vars_ && !macro_exists) {
                    spdlog::warn("{} save_variables contain {}_ data but _IFS_VARS macro "
                                 "not found — falling back to native ZMOD",
                                 backend_log_tag(), var_prefix_);
                    has_ifs_vars_ = false;
                }
                need_zcolor = !has_ifs_vars_;
            }
            if (need_zcolor) {
                spdlog::info("{} No _IFS_VARS data found, reading Adventurer5M.json (native ZMOD)",
                             backend_log_tag());
                read_adventurer_json();
                register_zcolor_listener();
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
    bool sensor_changed = false;

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
                sensor_changed = true;
            }
        }
    }

    // Native ZMOD: when a port sensor changes, the user may have swapped filament.
    // Schedule a re-read of Adventurer5M.json to pick up any color/type changes.
    if (sensor_changed && !has_ifs_vars_) {
        lock.unlock();
        schedule_json_reread();
        lock.lock();
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
            if (!colors[i].is_string())
                continue;
            std::string incoming = colors[i].get<std::string>();
            if (dirty_[i]) {
                // Slot was edited locally. Check if Klipper has persisted our value.
                // Case-insensitive compare: Klipper may normalize case.
                bool match = std::equal(incoming.begin(), incoming.end(), colors_[i].begin(),
                                        colors_[i].end(),
                                        [](char a, char b) { return toupper(a) == toupper(b); });
                if (match) {
                    spdlog::debug("{} Slot {} dirty cleared — Klipper confirmed color {}",
                                  backend_log_tag(), i, incoming);
                    dirty_[i] = false;
                } else {
                    spdlog::debug("{} Slot {} still dirty — incoming color '{}' != local '{}'",
                                  backend_log_tag(), i, incoming, colors_[i]);
                }
                continue;
            }
            colors_[i] = incoming;

            // Latch port_presence for non-empty colors from save_variables,
            // same logic as parse_adventurer_json. Without per-port sensors,
            // a non-empty color in the IFS variables means filament is present.
            if (!incoming.empty() && !has_per_port_sensors_) {
                port_presence_[i] = true;
            }
        }
    }

    // Materials: array of type strings ["PLA", "PETG", ...]
    // Dirty is cleared by the color match path above — _IFS_VARS writes both
    // colors and types atomically, so they arrive in the same status update.
    if (vars.contains(p + "_types") && vars[p + "_types"].is_array()) {
        const auto& types = vars[p + "_types"];
        for (size_t i = 0; i < std::min(types.size(), static_cast<size_t>(NUM_PORTS)); ++i) {
            if (types[i].is_string() && !dirty_[i]) {
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
    if (vars.contains(p + "_current_tool") && vars[p + "_current_tool"].is_number_integer()) {
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
        bool was_first = !has_per_port_sensors_;
        bool changed = port_presence_[static_cast<size_t>(slot)] != detected;
        has_per_port_sensors_ = true;
        port_presence_[static_cast<size_t>(slot)] = detected;
        if (was_first || changed) {
            spdlog::debug("{} Port {} sensor: {} (per_port_sensors=true{})", backend_log_tag(),
                          port_1based, detected ? "present" : "empty",
                          was_first ? ", first detection" : "");
        }
    }
}

void AmsBackendAd5xIfs::parse_head_sensor(bool detected) {
    if (head_filament_ != detected) {
        spdlog::debug("{} Head sensor: {}", backend_log_tag(),
                      detected ? "filament detected" : "no filament");
    }
    head_filament_ = detected;
}

void AmsBackendAd5xIfs::update_slot_from_state(int slot_index) {
    if (slot_index < 0 || slot_index >= NUM_PORTS)
        return;

    auto* entry = slots_.get_mut(slot_index);
    if (!entry)
        return;

    auto idx = static_cast<size_t>(slot_index);

    // Color: parse hex string to uint32_t
    if (!colors_[idx].empty()) {
        try {
            entry->info.color_rgb = static_cast<uint32_t>(std::stoul(colors_[idx], nullptr, 16));
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

    SlotStatus prev_status = entry->info.status;
    if (has_filament && is_active_slot && head_filament_) {
        entry->info.status = SlotStatus::LOADED;
    } else if (has_filament) {
        entry->info.status = SlotStatus::AVAILABLE;
    } else {
        entry->info.status = SlotStatus::EMPTY;
    }

    if (entry->info.status != prev_status) {
        spdlog::debug("{} Slot {} status: {} → {} (port_presence={}, active={}, head={}, "
                      "per_port_sensors={}, color={}, material={})",
                      backend_log_tag(), slot_index, static_cast<int>(prev_status),
                      static_cast<int>(entry->info.status), port_presence_[idx], is_active_slot,
                      head_filament_, has_per_port_sensors_, colors_[idx], materials_[idx]);
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
        if (port >= 1 && port <= NUM_PORTS && port_presence_[static_cast<size_t>(port - 1)]) {
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

    // Active slot in transit — filament is in the lane between gate and head
    if (is_active) {
        return PathSegment::LANE;
    }
    // Non-active slots with filament detected at gate — show at hub
    return PathSegment::HUB;
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
    if (!err.success())
        return err;

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
    if (!err.success())
        return err;

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
    if (!err.success())
        return err;

    int port = slot_index + 1;
    spdlog::info("{} Selecting port {}", backend_log_tag(), port);
    return execute_gcode("SET_EXTRUDER_SLOT SLOT=" + std::to_string(port));
}

AmsError AmsBackendAd5xIfs::change_tool(int tool_number) {
    if (tool_number < 0 || tool_number >= TOOL_MAP_SIZE) {
        return AmsErrorHelper::invalid_slot(tool_number, TOOL_MAP_SIZE - 1);
    }
    auto err = check_preconditions();
    if (!err.success())
        return err;

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
    if (!err.success())
        return err;

    // IFS_UNLOCK resets the IFS driver state machine — safest recovery command
    spdlog::info("{} Recovery: IFS_UNLOCK", backend_log_tag());
    return execute_gcode("IFS_UNLOCK");
}

AmsError AmsBackendAd5xIfs::reset() {
    auto err = check_preconditions();
    if (!err.success())
        return err;

    // IFS_UNLOCK resets the IFS driver — F15 (hard reset) is not exposed as a safe macro
    spdlog::info("{} Reset: IFS_UNLOCK", backend_log_tag());
    return execute_gcode("IFS_UNLOCK");
}

AmsError AmsBackendAd5xIfs::cancel() {
    auto err = check_preconditions();
    if (!err.success())
        return err;

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

        // Mark slot dirty to prevent parse_save_variables from overwriting our edit
        dirty_[idx] = true;

        // Convert color to hex string for our cached array
        char hex[7];
        snprintf(hex, sizeof(hex), "%06X", info.color_rgb & 0xFFFFFF);
        colors_[idx] = hex;

        materials_[idx] = info.material;

        // Without per-port sensors, infer presence from user-provided data.
        // Setting color/material marks the slot occupied; clearing both marks it empty.
        if (!has_per_port_sensors_) {
            bool has_data = !info.material.empty() || info.color_rgb != AMS_DEFAULT_SLOT_COLOR;
            port_presence_[idx] = has_data;
        }

        spdlog::debug("{} set_slot_info: slot {} dirty=true, color={}, material={}, presence={}",
                      backend_log_tag(), slot_index, hex, info.material, port_presence_[idx]);

        // Update entry directly
        entry->info.color_rgb = info.color_rgb;
        entry->info.material = info.material;
        entry->info.spoolman_id = info.spoolman_id;
        entry->info.remaining_weight_g = info.remaining_weight_g;
        entry->info.total_weight_g = info.total_weight_g;

        // Recalculate slot status now that port_presence may have changed
        update_slot_from_state(slot_index);
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

            // execute_gcode is async — these always return success immediately.
            // dirty_ stays true until parse_save_variables sees the updated
            // value come back from Klipper, preventing stale notify_status_update
            // events from reverting our edit (#716).
            write_ifs_var("colors", color_val);
            write_ifs_var("types", type_val);
        } else {
            // Native ZMOD: per-slot update via Adventurer5M.json
            auto err = write_adventurer_json(slot_index);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                dirty_[idx] = false;
            }
            if (!err.success())
                return err;
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
    if (!err.success())
        return err;

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
    if (!err.success())
        return err;

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
        if (i > 0)
            ss << ", ";
        ss << "'" << colors_[static_cast<size_t>(i)] << "'";
    }
    ss << "]\"";
    return ss.str();
}

std::string AmsBackendAd5xIfs::build_type_list_value() const {
    std::ostringstream ss;
    ss << "\"[";
    for (int i = 0; i < NUM_PORTS; ++i) {
        if (i > 0)
            ss << ", ";
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
        if (i > 0)
            ss << ", ";
        ss << tool_map_[static_cast<size_t>(i)];
    }
    ss << "]\"";
    return ss.str();
}

AmsError AmsBackendAd5xIfs::write_ifs_var(const std::string& key, const std::string& value) {
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

AmsError AmsBackendAd5xIfs::write_adventurer_json(int slot_index) {
    if (!api_) {
        return AmsErrorHelper::invalid_parameter("No API connection");
    }

    auto idx = static_cast<size_t>(slot_index);
    int port = slot_index + 1; // JSON uses 1-based slot numbering

    std::string hex, material;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hex = colors_[idx];
        material = materials_[idx];
    }

    if (hex.empty())
        hex = "808080";
    if (material.empty())
        material = "PLA";

    // Add # prefix for JSON file format
    std::string hex_with_hash = "#" + hex;

    spdlog::info("{} Writing slot {} to Adventurer5M.json (native ZMOD)", backend_log_tag(), port);

    // Read-modify-write: download current file, update slot, re-upload
    auto result = std::make_shared<AmsError>(AmsErrorHelper::success());
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto token = lifetime_.token();

    api_->transfers().download_file(
        "config", "Adventurer5M.json",
        [this, token, port, hex_with_hash, material, result, done](const std::string& content) {
            if (token.expired()) {
                *result =
                    AmsErrorHelper::command_failed("write_adventurer_json", "Connection lost");
                done->store(true);
                return;
            }

            json doc;
            try {
                doc = json::parse(content);
            } catch (const json::parse_error& e) {
                spdlog::warn("{} Failed to parse Adventurer5M.json for write: {}",
                             backend_log_tag(), e.what());
                *result =
                    AmsErrorHelper::command_failed("write_adventurer_json", "JSON parse error");
                done->store(true);
                return;
            }

            // Ensure FFMInfo exists
            if (!doc.contains("FFMInfo")) {
                doc["FFMInfo"] = json::object();
            }

            // Update the slot
            doc["FFMInfo"]["ffmColor" + std::to_string(port)] = hex_with_hash;
            doc["FFMInfo"]["ffmType" + std::to_string(port)] = material;

            // Serialize with indentation to match zmod's format
            std::string updated = doc.dump(4);

            api_->transfers().upload_file(
                "config", "Adventurer5M.json", updated,
                [this, done, port]() {
                    spdlog::info("{} Wrote slot {} to Adventurer5M.json", backend_log_tag(), port);
                    done->store(true);
                },
                [this, result, done, port](const MoonrakerError& err) {
                    spdlog::warn("{} Failed to upload Adventurer5M.json for slot {}: {}",
                                 backend_log_tag(), port, err.message);
                    *result = AmsErrorHelper::command_failed("write_adventurer_json", err.message);
                    done->store(true);
                });
        },
        [this, result, done](const MoonrakerError& err) {
            spdlog::warn("{} Failed to download Adventurer5M.json for write: {}", backend_log_tag(),
                         err.message);
            *result = AmsErrorHelper::command_failed("write_adventurer_json", err.message);
            done->store(true);
        });

    // Wait for async operation to complete (this is called from a sync API)
    // The existing write_ifs_var / execute_gcode also blocks, so this is consistent.
    for (int i = 0; i < 100 && !done->load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!done->load()) {
        return AmsErrorHelper::command_failed("write_adventurer_json",
                                              "Timeout writing Adventurer5M.json");
    }

    return *result;
}

void AmsBackendAd5xIfs::read_adventurer_json() {
    if (!api_)
        return;

    auto token = lifetime_.token();
    api_->transfers().download_file(
        "config", "Adventurer5M.json",
        [this, token](const std::string& content) {
            if (token.expired())
                return;
            spdlog::debug("{} Downloaded Adventurer5M.json ({} bytes)", backend_log_tag(),
                          content.size());
            parse_adventurer_json(content);
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired())
                return;
            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND || err.code == 404) {
                spdlog::info("{} Adventurer5M.json not found — not a native ZMOD AD5X",
                             backend_log_tag());
            } else {
                spdlog::warn("{} Failed to download Adventurer5M.json: {}", backend_log_tag(),
                             err.message);
            }
        });
}

void AmsBackendAd5xIfs::register_zcolor_listener() {
    if (!client_)
        return;

    static const std::string handler_name = "ifs_zcolor_watcher";
    auto token = lifetime_.token();

    client_->register_method_callback(
        "notify_gcode_response", handler_name, [this, token](const json& msg) {
            if (token.expired())
                return;

            std::string line;
            if (msg.contains("params") && msg["params"].is_array() && !msg["params"].empty() &&
                msg["params"][0].is_string()) {
                line = msg["params"][0].get<std::string>();
            } else if (msg.is_array() && !msg.empty() && msg[0].is_string()) {
                line = msg[0].get<std::string>();
            } else {
                return;
            }

            if (line.find("RUN_ZCOLOR") != std::string::npos ||
                line.find("CHANGE_ZCOLOR") != std::string::npos) {
                spdlog::debug(
                    "{} Detected external color change in gcode stream, scheduling re-read",
                    backend_log_tag());
                schedule_json_reread();
            }
        });
}

void AmsBackendAd5xIfs::schedule_json_reread() {
    if (reread_pending_.exchange(true))
        return;

    auto token = lifetime_.token();

    std::thread([this, token]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (token.expired())
            return;
        reread_pending_.store(false);
        spdlog::debug("{} Re-reading Adventurer5M.json after external change", backend_log_tag());
        read_adventurer_json();
    }).detach();
}

void AmsBackendAd5xIfs::parse_adventurer_json(const std::string& content) {
    json doc;
    try {
        doc = json::parse(content);
    } catch (const json::parse_error& e) {
        spdlog::warn("{} Failed to parse Adventurer5M.json: {}", backend_log_tag(), e.what());
        return;
    }

    auto ffm_it = doc.find("FFMInfo");
    if (ffm_it == doc.end() || !ffm_it->is_object()) {
        spdlog::debug("{} No FFMInfo section in Adventurer5M.json", backend_log_tag());
        return;
    }
    const auto& ffm = *ffm_it;

    int parsed_count = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Ports 1-4 in JSON map to slots 0-3
        for (int port = 1; port <= NUM_PORTS; ++port) {
            std::string color_key = "ffmColor" + std::to_string(port);
            std::string type_key = "ffmType" + std::to_string(port);

            auto color_it = ffm.find(color_key);
            auto type_it = ffm.find(type_key);
            if (color_it == ffm.end() && type_it == ffm.end())
                continue;

            // Extract color — strip '#' prefix, default to gray if empty
            std::string hex;
            if (color_it != ffm.end() && color_it->is_string()) {
                hex = color_it->get<std::string>();
            }
            if (!hex.empty() && hex[0] == '#') {
                hex = hex.substr(1);
            }
            // Non-empty color means filament was loaded into this port
            bool has_filament_data = !hex.empty();
            if (hex.empty()) {
                hex = "808080";
            }
            // Uppercase
            for (auto& c : hex)
                c = static_cast<char>(toupper(c));

            // Extract material type
            std::string type;
            if (type_it != ffm.end() && type_it->is_string()) {
                type = type_it->get<std::string>();
            }

            int idx = port - 1;
            if (dirty_[static_cast<size_t>(idx)])
                continue;
            colors_[static_cast<size_t>(idx)] = hex;
            materials_[static_cast<size_t>(idx)] = type;

            // Native ZMOD has no per-port switch sensors — infer presence from
            // Adventurer5M.json data. A non-empty color means filament is present.
            // This is a one-way latch (never set back to false) — acceptable because
            // native ZMOD doesn't provide removal events, and a backend restart
            // re-reads fresh data.
            if (has_filament_data && !has_per_port_sensors_) {
                port_presence_[static_cast<size_t>(idx)] = true;
            }

            update_slot_from_state(idx);
            ++parsed_count;
        }
    } // release lock before emit_event (which also takes mutex_)

    if (parsed_count > 0) {
        spdlog::info("{} Loaded {} slots from Adventurer5M.json (native ZMOD)", backend_log_tag(),
                     parsed_count);
        emit_event(EVENT_STATE_CHANGED);
    } else {
        spdlog::debug("{} No slot data found in Adventurer5M.json", backend_log_tag());
    }
}

void AmsBackendAd5xIfs::detect_load_unload_completion(bool head_detected) {
    if (system_info_.action == AmsAction::LOADING && head_detected) {
        system_info_.action = AmsAction::IDLE;
        spdlog::info("{} Load complete (head sensor triggered)", backend_log_tag());
        PostOpCooldownManager::instance().schedule();
    } else if (system_info_.action == AmsAction::UNLOADING && !head_detected) {
        system_info_.action = AmsAction::IDLE;
        spdlog::info("{} Unload complete (head sensor cleared)", backend_log_tag());
        PostOpCooldownManager::instance().schedule();
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
    if (system_info_.action != AmsAction::LOADING && system_info_.action != AmsAction::UNLOADING) {
        return;
    }

    auto elapsed = std::chrono::steady_clock::now() - action_start_time_;
    if (elapsed >= std::chrono::seconds(ACTION_TIMEOUT_SECONDS)) {
        spdlog::warn("{} {} timed out after {}s, resetting to IDLE", backend_log_tag(),
                     ams_action_to_string(system_info_.action),
                     std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        system_info_.action = AmsAction::IDLE;
    }
}

#endif // HELIX_HAS_IFS
