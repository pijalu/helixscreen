// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_snapmaker.h"

#include "ams_error.h"
#include "moonraker_api.h"

#include <spdlog/spdlog.h>

#include <spdlog/fmt/fmt.h>

// ============================================================================
// Construction
// ============================================================================

AmsBackendSnapmaker::AmsBackendSnapmaker(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    // Initialize system info
    system_info_.type = AmsType::SNAPMAKER;
    system_info_.type_name = "Snapmaker SnapSwap";
    system_info_.supports_endless_spool = false;
    system_info_.supports_tool_mapping = false;
    system_info_.supports_bypass = false;
    system_info_.has_hardware_bypass_sensor = false;

    // Initialize 1 unit with 4 slots
    AmsUnit unit;
    unit.unit_index = 0;
    unit.name = "SnapSwap";
    unit.display_name = "SnapSwap";
    unit.slot_count = NUM_TOOLS;
    unit.first_slot_global_index = 0;
    unit.connected = true;
    unit.topology = PathTopology::PARALLEL;

    for (int i = 0; i < NUM_TOOLS; i++) {
        SlotInfo slot;
        slot.slot_index = i;
        slot.global_index = i;
        slot.status = SlotStatus::UNKNOWN;
        slot.mapped_tool = i;
        // Klipper uses "extruder" for T0, "extruder1" for T1, etc.
        slot.extruder_name = (i == 0) ? "extruder" : fmt::format("extruder{}", i);
        unit.slots.push_back(slot);
    }

    system_info_.units.push_back(std::move(unit));
    system_info_.total_slots = NUM_TOOLS;

    spdlog::debug("[AMS Snapmaker] Backend created with {} tools", NUM_TOOLS);
}

// ============================================================================
// State Queries
// ============================================================================

AmsSystemInfo AmsBackendSnapmaker::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

SlotInfo AmsBackendSnapmaker::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (slot) {
        return *slot;
    }
    SlotInfo empty;
    empty.slot_index = -1;
    return empty;
}

// ============================================================================
// Path Visualization
// ============================================================================

PathSegment AmsBackendSnapmaker::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (system_info_.current_tool >= 0 && system_info_.filament_loaded) {
        return PathSegment::NOZZLE;
    }
    return PathSegment::SPOOL;
}

PathSegment AmsBackendSnapmaker::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (!slot) return PathSegment::NONE;

    if (slot->status == SlotStatus::LOADED) {
        return PathSegment::NOZZLE;
    }
    if (slot->status == SlotStatus::AVAILABLE) {
        return PathSegment::SPOOL;
    }
    return PathSegment::NONE;
}

PathSegment AmsBackendSnapmaker::infer_error_segment() const {
    return PathSegment::NONE;
}

// ============================================================================
// Filament Operations
// ============================================================================

AmsError AmsBackendSnapmaker::load_filament(int slot_index) {
    auto err = validate_slot_index(slot_index);
    if (err.result != AmsResult::SUCCESS) return err;

    return execute_gcode("AUTO_FEEDING");
}

AmsError AmsBackendSnapmaker::unload_filament(int /*slot_index*/) {
    return execute_gcode("INNER_FILAMENT_UNLOAD");
}

AmsError AmsBackendSnapmaker::select_slot(int slot_index) {
    return change_tool(slot_index);
}

AmsError AmsBackendSnapmaker::change_tool(int tool_number) {
    auto err = validate_slot_index(tool_number);
    if (err.result != AmsResult::SUCCESS) return err;

    return execute_gcode(fmt::format("T{}", tool_number));
}

// ============================================================================
// Recovery (not supported)
// ============================================================================

AmsError AmsBackendSnapmaker::recover() {
    return AmsErrorHelper::not_supported("Recover not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::reset() {
    return AmsErrorHelper::not_supported("Reset not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::cancel() {
    return AmsErrorHelper::not_supported("Cancel not supported on Snapmaker");
}

// ============================================================================
// Configuration
// ============================================================================

AmsError AmsBackendSnapmaker::set_slot_info(int slot_index, const SlotInfo& info, bool /*persist*/) {
    auto err = validate_slot_index(slot_index);
    if (err.result != AmsResult::SUCCESS) return err;

    std::lock_guard<std::mutex> lock(mutex_);
    auto* slot = system_info_.units[0].get_slot(slot_index);
    if (!slot) return AmsErrorHelper::invalid_slot(slot_index, NUM_TOOLS - 1);

    // Update filament fields from info
    slot->color_name = info.color_name;
    slot->color_rgb = info.color_rgb;
    slot->material = info.material;
    slot->brand = info.brand;
    slot->nozzle_temp_min = info.nozzle_temp_min;
    slot->nozzle_temp_max = info.nozzle_temp_max;
    slot->bed_temp = info.bed_temp;
    slot->remaining_weight_g = info.remaining_weight_g;
    slot->total_weight_g = info.total_weight_g;
    slot->spoolman_id = info.spoolman_id;
    slot->spool_name = info.spool_name;

    emit_event(EVENT_SLOT_CHANGED);
    return AmsErrorHelper::success();
}

AmsError AmsBackendSnapmaker::set_tool_mapping(int /*tool_number*/, int /*slot_index*/) {
    return AmsErrorHelper::not_supported("Tool mapping not supported on Snapmaker");
}

// ============================================================================
// Bypass (not applicable)
// ============================================================================

AmsError AmsBackendSnapmaker::enable_bypass() {
    return AmsErrorHelper::not_supported("Bypass not supported on Snapmaker");
}

AmsError AmsBackendSnapmaker::disable_bypass() {
    return AmsErrorHelper::not_supported("Bypass not supported on Snapmaker");
}

// ============================================================================
// Static Parsers
// ============================================================================

ExtruderToolState AmsBackendSnapmaker::parse_extruder_state(const nlohmann::json& json) {
    ExtruderToolState state;

    if (json.contains("state") && json["state"].is_string()) {
        state.state = json["state"].get<std::string>();
    }
    if (json.contains("park_pin") && json["park_pin"].is_boolean()) {
        state.park_pin = json["park_pin"].get<bool>();
    }
    if (json.contains("active_pin") && json["active_pin"].is_boolean()) {
        state.active_pin = json["active_pin"].get<bool>();
    }
    if (json.contains("activating_move") && json["activating_move"].is_boolean()) {
        state.activating_move = json["activating_move"].get<bool>();
    }
    if (json.contains("extruder_offset") && json["extruder_offset"].is_array()) {
        const auto& arr = json["extruder_offset"];
        for (size_t i = 0; i < std::min(arr.size(), size_t{3}); i++) {
            if (arr[i].is_number()) {
                state.extruder_offset[i] = arr[i].get<float>();
            }
        }
    }
    if (json.contains("switch_count") && json["switch_count"].is_number()) {
        state.switch_count = json["switch_count"].get<int>();
    }
    if (json.contains("retry_count") && json["retry_count"].is_number()) {
        state.retry_count = json["retry_count"].get<int>();
    }
    if (json.contains("error_count") && json["error_count"].is_number()) {
        state.error_count = json["error_count"].get<int>();
    }

    return state;
}

SnapmakerRfidInfo AmsBackendSnapmaker::parse_rfid_info(const nlohmann::json& json) {
    SnapmakerRfidInfo info;

    if (json.contains("MAIN_TYPE") && json["MAIN_TYPE"].is_string()) {
        info.main_type = json["MAIN_TYPE"].get<std::string>();
    }
    if (json.contains("SUB_TYPE") && json["SUB_TYPE"].is_string()) {
        info.sub_type = json["SUB_TYPE"].get<std::string>();
    }
    if (json.contains("MANUFACTURER") && json["MANUFACTURER"].is_string()) {
        info.manufacturer = json["MANUFACTURER"].get<std::string>();
    }
    if (json.contains("VENDOR") && json["VENDOR"].is_string()) {
        info.vendor = json["VENDOR"].get<std::string>();
    }
    if (json.contains("ARGB_COLOR") && json["ARGB_COLOR"].is_number()) {
        // ARGB -> RGB: mask off the alpha byte
        uint32_t argb = json["ARGB_COLOR"].get<uint32_t>();
        info.color_rgb = argb & 0x00FFFFFF;
    }
    if (json.contains("HOTEND_MIN_TEMP") && json["HOTEND_MIN_TEMP"].is_number()) {
        info.hotend_min_temp = json["HOTEND_MIN_TEMP"].get<int>();
    }
    if (json.contains("HOTEND_MAX_TEMP") && json["HOTEND_MAX_TEMP"].is_number()) {
        info.hotend_max_temp = json["HOTEND_MAX_TEMP"].get<int>();
    }
    if (json.contains("BED_TEMP") && json["BED_TEMP"].is_number()) {
        info.bed_temp = json["BED_TEMP"].get<int>();
    }
    if (json.contains("WEIGHT") && json["WEIGHT"].is_number()) {
        info.weight_g = json["WEIGHT"].get<int>();
    }

    return info;
}

// ============================================================================
// Status Update Handling
// ============================================================================

void AmsBackendSnapmaker::handle_status_update(const nlohmann::json& notification) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;

    // Parse extruder0..3 state
    // Klipper uses "extruder" for T0, "extruder1" for T1, etc.
    static const std::string extruder_keys[] = {"extruder", "extruder1", "extruder2", "extruder3"};
    for (int i = 0; i < NUM_TOOLS; i++) {
        const auto& key = extruder_keys[i];
        if (notification.contains(key) && notification[key].is_object()) {
            auto new_state = parse_extruder_state(notification[key]);

            // Update slot status based on extruder state
            auto* slot = system_info_.units[0].get_slot(i);
            if (slot) {
                if (new_state.active_pin) {
                    slot->status = SlotStatus::LOADED;
                } else if (new_state.park_pin) {
                    slot->status = SlotStatus::AVAILABLE;
                }
            }

            extruder_states_[i] = std::move(new_state);
            changed = true;
        }
    }

    // Detect active tool: whichever extruder has active_pin set (or state != "PARKED")
    // Also check toolhead.extruder for cross-validation
    int active = -1;
    for (int i = 0; i < NUM_TOOLS; i++) {
        if (extruder_states_[i].active_pin ||
            (!extruder_states_[i].state.empty() && extruder_states_[i].state != "PARKED")) {
            active = i;
            break;
        }
    }
    if (notification.contains("toolhead") && notification["toolhead"].is_object()) {
        const auto& th = notification["toolhead"];
        if (th.contains("extruder") && th["extruder"].is_string()) {
            auto ext_name = th["extruder"].get<std::string>();
            // "extruder" = 0, "extruder1" = 1, etc.
            if (ext_name == "extruder") {
                active = 0;
            } else if (ext_name.size() > 8 && ext_name.rfind("extruder", 0) == 0) {
                try { active = std::stoi(ext_name.substr(8)); } catch (...) {}
            }
        }
    }
    if (active != system_info_.current_tool) {
        system_info_.current_tool = active;
        system_info_.filament_loaded = (active >= 0);
        changed = true;
    }

    // Parse filament_detect info (RFID data per channel)
    if (notification.contains("filament_detect") && notification["filament_detect"].is_object()) {
        const auto& fd = notification["filament_detect"];

        // Parse RFID info per channel — filament_detect.info is a JSON array [ch0, ch1, ch2, ch3]
        if (fd.contains("info") && fd["info"].is_array()) {
            const auto& info_arr = fd["info"];
            for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(info_arr.size()); i++) {
                if (!info_arr[i].is_object()) continue;
                auto rfid = parse_rfid_info(info_arr[i]);

                auto* slot = system_info_.units[0].get_slot(i);
                if (slot) {
                    slot->material = rfid.main_type;
                    // Prefer MANUFACTURER over VENDOR for brand
                    slot->brand = !rfid.manufacturer.empty() ? rfid.manufacturer : rfid.vendor;
                    slot->color_rgb = rfid.color_rgb;
                    slot->color_name = rfid.sub_type;
                    slot->nozzle_temp_min = rfid.hotend_min_temp;
                    slot->nozzle_temp_max = rfid.hotend_max_temp;
                    slot->bed_temp = rfid.bed_temp;
                    slot->total_weight_g = static_cast<float>(rfid.weight_g);
                }
                changed = true;
            }
        }

        // Parse filament state per channel — filament_detect.state is [int, int, int, int]
        // 0 = tag present and read, non-zero = no tag / error
        if (fd.contains("state") && fd["state"].is_array()) {
            const auto& state_arr = fd["state"];
            for (int i = 0; i < NUM_TOOLS && i < static_cast<int>(state_arr.size()); i++) {
                if (!state_arr[i].is_number()) continue;
                int state_val = state_arr[i].get<int>();
                auto* slot = system_info_.units[0].get_slot(i);
                if (slot && slot->status == SlotStatus::UNKNOWN) {
                    slot->status = (state_val == 0) ? SlotStatus::AVAILABLE : SlotStatus::EMPTY;
                }
                changed = true;
            }
        }
    }

    // Parse filament_feed left/right — top-level Klipper objects (not nested in filament_detect)
    // Each contains per-extruder state: filament_detected, channel_state, channel_error
    for (const auto& feed_key : {"filament_feed left", "filament_feed right"}) {
        if (notification.contains(feed_key) && notification[feed_key].is_object()) {
            const auto& feed = notification[feed_key];
            for (int i = 0; i < NUM_TOOLS; i++) {
                std::string ext_key = (i == 0) ? "extruder0" : fmt::format("extruder{}", i);
                if (feed.contains(ext_key) && feed[ext_key].is_object()) {
                    bool detected = feed[ext_key].value("filament_detected", false);
                    auto* slot = system_info_.units[0].get_slot(i);
                    if (slot) {
                        if (detected && slot->status == SlotStatus::EMPTY) {
                            slot->status = SlotStatus::AVAILABLE;
                            changed = true;
                        } else if (!detected && slot->status == SlotStatus::AVAILABLE) {
                            slot->status = SlotStatus::EMPTY;
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

// ============================================================================
// Internal Helpers
// ============================================================================

AmsError AmsBackendSnapmaker::validate_slot_index(int slot_index) const {
    if (slot_index < 0 || slot_index >= NUM_TOOLS) {
        return AmsErrorHelper::invalid_slot(slot_index, NUM_TOOLS - 1);
    }
    return AmsErrorHelper::success();
}
