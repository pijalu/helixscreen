// SPDX-License-Identifier: GPL-3.0-or-later
#if HELIX_HAS_CFS

#include "ams_backend_cfs.h"

#include "moonraker_error.h"
#include "ui_update_queue.h"

#include "hv/json.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

namespace helix::printer {

using json = nlohmann::json;

static constexpr const char* SLOT_OVERRIDES_JSON = "cfs_slot_overrides.json";
static constexpr const char* MOONRAKER_DB_KEY_SLOTS = "cfs_slot_overrides";
static constexpr const char* MOONRAKER_DB_NAMESPACE = "helix-screen";

const CfsMaterialDb& CfsMaterialDb::instance()
{
    static CfsMaterialDb db;
    return db;
}

CfsMaterialDb::CfsMaterialDb()
{
    load_database();
}

void CfsMaterialDb::load_database()
{
    for (const auto& path : {"assets/cfs_materials.json",
                              "../assets/cfs_materials.json",
                              "/opt/helixscreen/assets/cfs_materials.json"}) {
        std::ifstream f(path);
        if (!f.is_open())
            continue;

        try {
            auto j = nlohmann::json::parse(f);
            for (auto& [id, entry] : j.items()) {
                CfsMaterialInfo info;
                info.id            = id;
                info.brand         = entry.value("brand", "");
                info.name          = entry.value("name", "");
                info.material_type = entry.value("type", "");
                info.min_temp      = entry.value("min_temp", 0);
                info.max_temp      = entry.value("max_temp", 0);
                materials_[id]     = std::move(info);
            }
            spdlog::info("[AMS CFS] Loaded {} materials from {}", materials_.size(), path);
            return;
        } catch (const std::exception& e) {
            spdlog::warn("[AMS CFS] Failed to parse {}: {}", path, e.what());
        }
    }
    spdlog::warn("[AMS CFS] Material database not found");
}

const CfsMaterialInfo* CfsMaterialDb::lookup(const std::string& id) const
{
    auto it = materials_.find(id);
    return it != materials_.end() ? &it->second : nullptr;
}

std::string CfsMaterialDb::strip_code(const std::string& code)
{
    if (code == "-1" || code == "None" || code.empty())
        return "";
    if (code.size() == 6 && code[0] == '1')
        return code.substr(1);
    return code;
}

uint32_t CfsMaterialDb::parse_color(const std::string& color_str)
{
    if (color_str == "-1" || color_str == "None" || color_str.empty())
        return DEFAULT_COLOR;
    std::string hex = color_str;
    if (hex.size() == 7 && hex[0] == '0')
        hex = hex.substr(1);
    try {
        return static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {
        return DEFAULT_COLOR;
    }
}

std::string CfsMaterialDb::slot_to_tnn(int global_index)
{
    if (global_index < 0 || global_index > 15)
        return "";
    int unit   = global_index / 4 + 1;
    int slot   = global_index % 4;
    char letter = 'A' + static_cast<char>(slot);
    return "T" + std::to_string(unit) + letter;
}

int CfsMaterialDb::tnn_to_slot(const std::string& tnn)
{
    if (tnn.size() != 3 || tnn[0] != 'T')
        return -1;
    int unit = tnn[1] - '0';
    int slot = tnn[2] - 'A';
    if (unit < 1 || unit > 4 || slot < 0 || slot > 3)
        return -1;
    return (unit - 1) * 4 + slot;
}

// --- CFS Error Decoder ---

struct CfsErrorEntry {
    const char* message;
    const char* hint;
    AmsAlertLevel level;
};

static const std::unordered_map<std::string, CfsErrorEntry> CFS_ERROR_TABLE = {
    {"key831", {"Lost connection to CFS unit", "Check the RS-485 cable between printer and CFS", AmsAlertLevel::SYSTEM}},
    {"key834", {"Invalid parameters sent to CFS", "This may indicate a firmware bug — try restarting", AmsAlertLevel::SYSTEM}},
    {"key835", {"Filament jammed at CFS connector", "Open the CFS lid, check the PTFE tube connection for the stuck slot", AmsAlertLevel::SLOT}},
    {"key836", {"Filament jammed between CFS and sensor", "Check the Bowden tube for kinks or debris", AmsAlertLevel::SLOT}},
    {"key837", {"Filament jammed before extruder gear", "Check for tangles on the spool and clear the filament path to the printhead", AmsAlertLevel::SLOT}},
    {"key838", {"Filament reached extruder but won't feed", "Check for a clog in the hotend or a worn drive gear", AmsAlertLevel::SLOT}},
    {"key840", {"CFS unit state error", "A unit reported an unexpected state — check its current operation", AmsAlertLevel::UNIT}},
    {"key841", {"Filament cutter stuck", "The cutter blade didn't return — check for filament wrapped around the cutting mechanism", AmsAlertLevel::SYSTEM}},
    {"key843", {"Can't read filament RFID tag", "Re-seat the spool in the slot, ensure the RFID label faces the reader", AmsAlertLevel::SLOT}},
    {"key844", {"PTFE tube connection loose", "Re-seat the Bowden tube connector on the CFS unit", AmsAlertLevel::UNIT}},
    {"key845", {"Nozzle clog detected", "Run a cold pull or replace the nozzle", AmsAlertLevel::SYSTEM}},
    {"key847", {"Empty spool — filament wound around hub", "Remove the empty spool and clear wound filament from the CFS hub", AmsAlertLevel::SLOT}},
    {"key848", {"Filament snapped inside CFS", "Open the CFS unit and remove the broken filament from the slot", AmsAlertLevel::SLOT}},
    {"key849", {"Retract failed — filament stuck in connector", "Manually pull the filament back through the connector", AmsAlertLevel::SLOT}},
    {"key850", {"Retract error — multiple connectors triggered", "Check that only one filament path is active", AmsAlertLevel::UNIT}},
    {"key851", {"Retract didn't reach buffer empty position", "The filament may not have fully retracted — try again or manually pull", AmsAlertLevel::SLOT}},
    {"key852", {"Sensor mismatch — check extruder and CFS sensors", "Extruder and CFS disagree on filament state — inspect both sensors", AmsAlertLevel::SYSTEM}},
    {"key853", {"Humidity sensor malfunction", "CFS unit's humidity sensor is not responding — may need service", AmsAlertLevel::UNIT}},
    {"key855", {"Filament cutter position error", "The cutter is out of alignment — recalibrate with CALIBRATE_CUT_POS", AmsAlertLevel::SYSTEM}},
    {"key856", {"Filament cutter not detected", "Check that the cutter mechanism is properly installed", AmsAlertLevel::SYSTEM}},
    {"key857", {"CFS motor overloaded", "A spool may be tangled or the drive gear is jammed", AmsAlertLevel::UNIT}},
    {"key858", {"EEPROM error on CFS unit", "CFS unit storage is corrupted — may need firmware reflash", AmsAlertLevel::UNIT}},
    {"key859", {"Measuring wheel error", "The filament length sensor is malfunctioning", AmsAlertLevel::UNIT}},
    {"key860", {"Buffer tube problem", "Check the buffer unit on the back of the printer", AmsAlertLevel::SYSTEM}},
    {"key861", {"RFID reader malfunction (left)", "The left RFID reader in this CFS unit may need service", AmsAlertLevel::UNIT}},
    {"key862", {"RFID reader malfunction (right)", "The right RFID reader in this CFS unit may need service", AmsAlertLevel::UNIT}},
    {"key863", {"Retract error — filament still detected", "Filament didn't fully retract, may need manual removal", AmsAlertLevel::SLOT}},
    {"key864", {"Extrude error — buffer not full", "Filament didn't fill buffer tube during load", AmsAlertLevel::SLOT}},
    {"key865", {"Retract error — failed to exit connector", "Filament stuck in connector during unload", AmsAlertLevel::SLOT}},
};

std::optional<AmsAlert> CfsErrorDecoder::decode(const std::string& key_code,
                                                 int unit_index, int slot_index)
{
    auto it = CFS_ERROR_TABLE.find(key_code);
    if (it == CFS_ERROR_TABLE.end())
        return std::nullopt;

    const auto& entry = it->second;
    AmsAlert alert;
    alert.message = entry.message;
    alert.hint = entry.hint;
    alert.level = entry.level;
    alert.severity = SlotError::Severity::ERROR;

    // Extract numeric code: "key845" -> "CFS-845"
    if (key_code.size() > 3) {
        alert.error_code = "CFS-" + key_code.substr(3);
    }

    if (entry.level == AmsAlertLevel::UNIT || entry.level == AmsAlertLevel::SLOT) {
        alert.unit_index = unit_index;
    }
    if (entry.level == AmsAlertLevel::SLOT) {
        alert.slot_index = slot_index;
    }

    return alert;
}

// =============================================================================
// AmsBackendCfs — Main CFS backend class
// =============================================================================

AmsBackendCfs::AmsBackendCfs(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client) {
    system_info_.type = AmsType::CFS;
    system_info_.type_name = "CFS";
    system_info_.supports_bypass = false;
    system_info_.tip_method = TipMethod::CUT;
    system_info_.supports_purge = true;

    spdlog::debug("[AMS CFS] Backend created");
}

void AmsBackendCfs::on_started() {
    spdlog::info("[AMS CFS] Backend started — querying initial box state");

    load_slot_overrides();

    // Query initial state explicitly since the subscription response may have
    // arrived before this backend registered its callback
    nlohmann::json objects_to_query = nlohmann::json::object();
    objects_to_query["box"] = nullptr;
    objects_to_query["motor_control"] = nullptr;
    objects_to_query["filament_switch_sensor filament_sensor"] = nullptr;

    nlohmann::json params = {{"objects", objects_to_query}};

    client_->send_jsonrpc(
        "printer.objects.query", params,
        [this](const nlohmann::json& response) {
            if (response.contains("result") && response["result"].contains("status") &&
                response["result"]["status"].is_object()) {
                // Wrap in notify_status_update format for handle_status_update
                nlohmann::json notification = {
                    {"params", nlohmann::json::array({response["result"]["status"]})}};
                handle_status_update(notification);
                spdlog::info("[AMS CFS] Initial state loaded");
            } else {
                spdlog::warn("[AMS CFS] Initial state query returned unexpected format");
            }
        },
        [](const MoonrakerError& err) {
            spdlog::warn("[AMS CFS] Failed to query initial state: {}", err.message);
        });
}

// --- Static parser ---

AmsSystemInfo AmsBackendCfs::parse_box_status(const nlohmann::json& box_json) {
    AmsSystemInfo info;
    info.type = AmsType::CFS;
    info.type_name = "CFS";
    info.tip_method = TipMethod::CUT;
    info.supports_purge = true;
    info.supports_bypass = false;

    // Parse auto_refill → endless spool support
    info.supports_endless_spool = box_json.value("auto_refill", 0) != 0;
    info.supports_tool_mapping = true;

    // Parse filament loaded state
    info.filament_loaded = box_json.value("filament", 0) != 0;

    // Parse tool mapping from "map" object
    if (box_json.contains("map") && box_json["map"].is_object()) {
        // Find maximum tool index to size the mapping vector
        int max_tool = -1;
        for (auto& [tnn_key, tnn_val] : box_json["map"].items()) {
            int slot = CfsMaterialDb::tnn_to_slot(tnn_key);
            if (slot >= 0 && slot > max_tool) {
                max_tool = slot;
            }
        }
        if (max_tool >= 0) {
            info.tool_to_slot_map.resize(max_tool + 1, -1);
            for (auto& [tnn_key, tnn_val] : box_json["map"].items()) {
                int src = CfsMaterialDb::tnn_to_slot(tnn_key);
                int dst = CfsMaterialDb::tnn_to_slot(tnn_val.get<std::string>());
                if (src >= 0 && dst >= 0 && src < static_cast<int>(info.tool_to_slot_map.size())) {
                    info.tool_to_slot_map[src] = dst;
                }
            }
        }
    }

    // Build same_material lookup: material_type code -> material name string
    // "same_material" contains groups like ["101001", "0000000", ["T1A"], "PLA"]
    // where the last element is a human-readable material name
    std::unordered_map<std::string, std::string> same_material_names;
    if (box_json.contains("same_material") && box_json["same_material"].is_array()) {
        for (const auto& group : box_json["same_material"]) {
            if (group.is_array() && group.size() >= 4 && group[0].is_string() &&
                group[3].is_string()) {
                same_material_names[group[0].get<std::string>()] = group[3].get<std::string>();
            }
        }
    }

    const auto& db = CfsMaterialDb::instance();

    // Loop over T1-T4 units
    for (int n = 1; n <= 4; ++n) {
        std::string key = "T" + std::to_string(n);
        if (!box_json.contains(key) || !box_json[key].is_object()) {
            spdlog::debug("[AMS CFS] {} not present or not an object", key);
            continue;
        }

        const auto& unit_json = box_json[key];
        std::string state = unit_json.value("state", "None");
        if (state == "None" || state == "-1") {
            spdlog::debug("[AMS CFS] {} disconnected (state={})", key, state);
            continue; // Disconnected unit
        }

        spdlog::debug("[AMS CFS] {} connected (state={})", key, state);

        AmsUnit unit;
        unit.unit_index = n - 1;
        unit.name = key;
        unit.display_name = "CFS Unit " + std::to_string(n);
        unit.slot_count = 4;
        unit.first_slot_global_index = (n - 1) * 4;
        unit.connected = true;
        unit.topology = PathTopology::HUB;

        // Firmware version and serial
        std::string ver = unit_json.value("version", "-1");
        if (ver != "-1" && ver != "None") {
            unit.firmware_version = ver;
        }
        std::string sn = unit_json.value("sn", "-1");
        if (sn != "-1" && sn != "None") {
            unit.serial_number = sn;
        }

        // Environment: temperature and humidity
        std::string temp_str = unit_json.value("temperature", "None");
        std::string humid_str = unit_json.value("dry_and_humidity", "None");
        if (temp_str != "None" && temp_str != "-1" && humid_str != "None" &&
            humid_str != "-1") {
            EnvironmentData env;
            try {
                env.temperature_c = std::stof(temp_str);
            } catch (...) {
                env.temperature_c = 0.0f;
            }
            try {
                env.humidity_pct = std::stof(humid_str);
            } catch (...) {
                env.humidity_pct = 0.0f;
            }
            env.has_humidity = true;
            unit.environment = env;
        }

        // Parse the 4 slots within this unit
        auto color_arr = unit_json.value("color_value", nlohmann::json::array());
        auto material_arr = unit_json.value("material_type", nlohmann::json::array());
        auto remain_arr = unit_json.value("remain_len", nlohmann::json::array());

        for (int i = 0; i < 4; ++i) {
            SlotInfo slot;
            slot.slot_index = i;
            slot.global_index = (n - 1) * 4 + i;

            // Color
            std::string color_str = "-1";
            if (i < static_cast<int>(color_arr.size()) && color_arr[i].is_string()) {
                color_str = color_arr[i].get<std::string>();
            }
            slot.color_rgb = CfsMaterialDb::parse_color(color_str);

            // Material type
            std::string mat_code_raw = "-1";
            if (i < static_cast<int>(material_arr.size()) && material_arr[i].is_string()) {
                mat_code_raw = material_arr[i].get<std::string>();
            }
            std::string mat_id = CfsMaterialDb::strip_code(mat_code_raw);
            if (!mat_id.empty()) {
                auto* mat_info = db.lookup(mat_id);
                if (mat_info) {
                    slot.material = mat_info->material_type;
                    slot.brand = mat_info->brand;
                    slot.nozzle_temp_min = mat_info->min_temp;
                    slot.nozzle_temp_max = mat_info->max_temp;
                } else {
                    // Fallback: check same_material for a human-readable name
                    auto it = same_material_names.find(mat_code_raw);
                    if (it != same_material_names.end()) {
                        slot.material = it->second;
                    }
                }
            }

            // Remaining length
            std::string remain_str = "-1";
            if (i < static_cast<int>(remain_arr.size()) && remain_arr[i].is_string()) {
                remain_str = remain_arr[i].get<std::string>();
            }
            if (remain_str != "-1" && remain_str != "None") {
                try {
                    slot.remaining_length_m = std::stof(remain_str);
                } catch (...) {
                    slot.remaining_length_m = 0.0f;
                }
            }

            // Derive status
            if (color_str == "-1" || color_str == "None") {
                slot.status = SlotStatus::EMPTY;
            } else if (slot.remaining_length_m <= 0.0f && remain_str != "-1") {
                slot.status = SlotStatus::EMPTY;
            } else {
                slot.status = SlotStatus::AVAILABLE;
            }

            unit.slots.push_back(std::move(slot));
        }

        info.units.push_back(std::move(unit));
        info.total_slots += 4;
    }

    return info;
}

// --- handle_status_update ---

void AmsBackendCfs::handle_status_update(const nlohmann::json& notification) {
    // notify_status_update format: {"method": "notify_status_update", "params": [{...}, timestamp]}
    if (!notification.contains("params") || !notification["params"].is_array() ||
        notification["params"].empty()) {
        return;
    }

    const auto& params = notification["params"][0];
    if (!params.is_object()) {
        return;
    }

    bool changed = false;

    if (params.contains("box") && params["box"].is_object()) {
        spdlog::debug("[AMS CFS] Received box data with {} keys", params["box"].size());
        auto new_info = parse_box_status(params["box"]);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            system_info_.units = std::move(new_info.units);
            system_info_.total_slots = new_info.total_slots;
            system_info_.supports_endless_spool = new_info.supports_endless_spool;
            system_info_.filament_loaded = new_info.filament_loaded;
            system_info_.tool_to_slot_map = std::move(new_info.tool_to_slot_map);

            // Apply user overrides over parsed RFID data (uses global slot indices)
            for (auto& unit : system_info_.units) {
                for (size_t j = 0; j < unit.slots.size(); ++j) {
                    auto& slot = unit.slots[j];
                    int global_idx = unit.first_slot_global_index + static_cast<int>(j);

                    // Clear override if slot status changed (spool physically swapped)
                    auto prev_it = prev_slot_status_.find(global_idx);
                    if (prev_it != prev_slot_status_.end() &&
                        prev_it->second != slot.status) {
                        if (slot_overrides_.erase(global_idx) > 0) {
                            spdlog::info("[AMS CFS] Slot {} status changed, clearing user override",
                                         global_idx);
                        }
                    }
                    prev_slot_status_[global_idx] = slot.status;

                    // Apply user overrides (from set_slot_info) over hardware data
                    auto ovr_it = slot_overrides_.find(global_idx);
                    if (ovr_it != slot_overrides_.end()) {
                        const auto& ovr = ovr_it->second;
                        slot.color_rgb = ovr.color_rgb;
                        slot.color_name = ovr.color_name;
                        slot.material = ovr.material;
                        slot.brand = ovr.brand;
                        slot.spool_name = ovr.spool_name;
                        slot.spoolman_id = ovr.spoolman_id;
                        slot.remaining_weight_g = ovr.remaining_weight_g;
                        slot.total_weight_g = ovr.total_weight_g;
                    }
                }
            }
        }
        changed = true;
    }

    if (params.contains("filament_switch_sensor filament_sensor")) {
        const auto& sensor = params["filament_switch_sensor filament_sensor"];
        if (sensor.contains("filament_detected")) {
            std::lock_guard<std::mutex> lock(mutex_);
            system_info_.filament_loaded = sensor["filament_detected"].get<bool>();
        }
        changed = true;
    }

    if (params.contains("motor_control")) {
        const auto& motor = params["motor_control"];
        if (motor.contains("motor_ready")) {
            std::lock_guard<std::mutex> lock(mutex_);
            motor_ready_ = motor["motor_ready"].get<bool>();
        }
        changed = true;
    }

    if (changed) {
        emit_event(EVENT_STATE_CHANGED);
    }
}

// --- State queries ---

AmsSystemInfo AmsBackendCfs::get_system_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_;
}

SlotInfo AmsBackendCfs::get_slot_info(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (slot) {
        return *slot;
    }
    return SlotInfo{};
}

// --- Path segments ---

PathSegment AmsBackendCfs::get_filament_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.filament_loaded ? PathSegment::NOZZLE : PathSegment::NONE;
}

PathSegment AmsBackendCfs::get_slot_filament_segment(int slot_index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* slot = system_info_.get_slot_global(slot_index);
    if (!slot) {
        return PathSegment::NONE;
    }
    switch (slot->status) {
    case SlotStatus::AVAILABLE:
    case SlotStatus::FROM_BUFFER:
        return PathSegment::SPOOL;
    case SlotStatus::LOADED:
        return PathSegment::NOZZLE;
    default:
        return PathSegment::NONE;
    }
}

PathSegment AmsBackendCfs::infer_error_segment() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& alert : system_info_.alerts) {
        if (alert.level == AmsAlertLevel::SLOT) {
            return PathSegment::LANE;
        }
        if (alert.level == AmsAlertLevel::SYSTEM || alert.level == AmsAlertLevel::UNIT) {
            return PathSegment::HUB;
        }
    }
    return PathSegment::NONE;
}

// --- Operations ---

AmsError AmsBackendCfs::load_filament(int slot_index) {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;
    auto gcode = load_gcode(slot_index);
    if (gcode.empty()) {
        return AmsErrorHelper::invalid_slot(slot_index, 15);
    }
    return ensure_homed_then(std::move(gcode));
}

AmsError AmsBackendCfs::unload_filament(int) {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;
    return ensure_homed_then(unload_gcode());
}

AmsError AmsBackendCfs::select_slot(int) {
    return AmsErrorHelper::not_supported("CFS loads directly");
}

AmsError AmsBackendCfs::change_tool(int tool) {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;
    return ensure_homed_then(load_gcode(tool));
}

AmsError AmsBackendCfs::reset() {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;
    return execute_gcode(reset_gcode());
}

AmsError AmsBackendCfs::recover() {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;
    return execute_gcode(recover_gcode());
}

AmsError AmsBackendCfs::cancel() {
    auto err = check_preconditions();
    if (err.result != AmsResult::SUCCESS) return err;
    return execute_gcode("CANCEL_PRINT");
}

// --- Unsupported stubs ---

AmsError AmsBackendCfs::set_slot_info(int slot_index, const SlotInfo& info, bool /*persist*/) {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Find the unit and local slot for this global index
        SlotInfo* target = nullptr;
        for (auto& unit : system_info_.units) {
            int first = unit.first_slot_global_index;
            int last = first + static_cast<int>(unit.slots.size()) - 1;
            if (slot_index >= first && slot_index <= last) {
                int local = slot_index - first;
                target = &unit.slots[local];
                break;
            }
        }

        if (!target) {
            return AmsErrorHelper::invalid_slot(slot_index, system_info_.total_slots - 1);
        }

        // Update in-memory slot state
        target->color_rgb = info.color_rgb;
        target->color_name = info.color_name;
        target->material = info.material;
        target->brand = info.brand;
        target->spool_name = info.spool_name;
        target->spoolman_id = info.spoolman_id;
        target->remaining_weight_g = info.remaining_weight_g;
        target->total_weight_g = info.total_weight_g;

        // Store override so parse doesn't clobber user edits
        SlotOverride& ovr = slot_overrides_[slot_index];
        ovr.color_rgb = info.color_rgb;
        ovr.color_name = info.color_name;
        ovr.material = info.material;
        ovr.brand = info.brand;
        ovr.spool_name = info.spool_name;
        ovr.spoolman_id = info.spoolman_id;
        ovr.remaining_weight_g = info.remaining_weight_g;
        ovr.total_weight_g = info.total_weight_g;
    }

    spdlog::info("[AMS CFS] Updated slot {} info (local override): {} {}",
                 slot_index, info.material, info.color_name);

    // Persist overrides to Moonraker DB + local JSON
    save_slot_overrides();

    emit_event(EVENT_SLOT_CHANGED, std::to_string(slot_index));
    return AmsErrorHelper::success();
}

AmsError AmsBackendCfs::set_tool_mapping(int, int) {
    return AmsErrorHelper::not_supported("CFS tool mapping via BOX_MODIFY_TN");
}

AmsError AmsBackendCfs::enable_bypass() {
    return AmsErrorHelper::not_supported("CFS has no bypass");
}

AmsError AmsBackendCfs::disable_bypass() {
    return AmsErrorHelper::not_supported("CFS has no bypass");
}

// --- GCode helpers ---

std::string AmsBackendCfs::load_gcode(int idx) {
    std::string tnn = CfsMaterialDb::slot_to_tnn(idx);
    if (tnn.empty()) {
        spdlog::error("[AMS CFS] Invalid slot index for load: {}", idx);
        return "";
    }
    // M8200 P (CR_BOX_PRE_OPT) is required before extrude — sets CFS to material-change mode.
    // Creality's BOX_LOAD_MATERIAL macro omits this and fails with key60 "Internal error".
    // M8200: P=prepare, L I=N=load slot N (CR_BOX_EXTRUDE), F=flush, O=end (CR_BOX_END_OPT)
    return "M8200 P\nM8200 L I=" + std::to_string(idx) + "\nM8200 F\nM8200 O";
}

std::string AmsBackendCfs::unload_gcode() {
    // M8200: P=prepare, C=cut (CR_BOX_CUT), R=retract (CR_BOX_RETRUDE), O=end
    return "M8200 P\nM8200 C\nM8200 R\nM8200 O";
}

std::string AmsBackendCfs::reset_gcode() {
    return "BOX_ERROR_CLEAR";
}

std::string AmsBackendCfs::recover_gcode() {
    return "BOX_ERROR_RESUME_PROCESS";
}

// --- Capabilities ---

helix::printer::EndlessSpoolCapabilities AmsBackendCfs::get_endless_spool_capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {.supported = true,
            .editable = false,
            .description = system_info_.supports_endless_spool ? "Auto-refill enabled"
                                                               : "Auto-refill disabled"};
}

helix::printer::ToolMappingCapabilities AmsBackendCfs::get_tool_mapping_capabilities() const {
    return {.supported = true, .editable = false, .description = ""};
}

std::vector<helix::printer::DeviceAction> AmsBackendCfs::get_device_actions() const {
    using DA = helix::printer::DeviceAction;
    using AT = helix::printer::ActionType;
    return {
        DA{"refresh_rfid", "Refresh RFID", "", "", "Re-read spool RFID tags and remaining length",
           AT::BUTTON, {}, {}, 0, 100, "", -1, true, ""},
        DA{"toggle_auto_refill", "Toggle Auto-Refill", "", "",
           "Enable/disable automatic backup spool switching",
           AT::TOGGLE, {}, {}, 0, 100, "", -1, true, ""},
        DA{"nozzle_clean", "Clean Nozzle", "", "",
           "Wipe nozzle on silicone cleaning strip",
           AT::BUTTON, {}, {}, 0, 100, "", -1, true, ""},
        DA{"comm_test", "Communication Test", "", "",
           "Test RS-485 link to CFS units",
           AT::BUTTON, {}, {}, 0, 100, "", -1, true, ""},
    };
}

AmsError AmsBackendCfs::execute_device_action(const std::string& action_id,
                                               const std::any& value) {
    if (action_id == "refresh_rfid") {
        // Re-query box state from Moonraker (the box module polls CFS automatically)
        on_started(); // Triggers printer.objects.query for box state
        return AmsErrorHelper::success();
    }

    if (action_id == "toggle_auto_refill") {
        return execute_gcode("BOX_ENABLE_AUTO_REFILL");
    }

    if (action_id == "nozzle_clean") {
        return execute_gcode("BOX_NOZZLE_CLEAN");
    }

    if (action_id == "comm_test") {
        // Query box state as a connectivity test
        on_started();
        return AmsErrorHelper::success();
    }

    return AmsErrorHelper::not_supported("Unknown action: " + action_id);
}

// ============================================================================
// Slot Override Persistence
// ============================================================================

json AmsBackendCfs::slot_overrides_to_json() const {
    json result = json::object();

    for (const auto& [idx, ovr] : slot_overrides_) {
        json entry;
        entry["color_rgb"] = ovr.color_rgb;
        entry["material"] = ovr.material;
        entry["color_name"] = ovr.color_name;
        entry["brand"] = ovr.brand;
        entry["spool_name"] = ovr.spool_name;
        entry["spoolman_id"] = ovr.spoolman_id;
        if (std::isfinite(ovr.remaining_weight_g) && ovr.remaining_weight_g >= 0)
            entry["remaining_weight_g"] = ovr.remaining_weight_g;
        if (std::isfinite(ovr.total_weight_g) && ovr.total_weight_g >= 0)
            entry["total_weight_g"] = ovr.total_weight_g;

        result[std::to_string(idx)] = entry;
    }

    return result;
}

void AmsBackendCfs::apply_slot_overrides_json(const json& data) {
    if (!data.is_object()) {
        spdlog::warn("[AMS CFS] apply_slot_overrides_json: expected JSON object");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    slot_overrides_.clear();

    for (auto& [key, entry] : data.items()) {
        if (!entry.is_object()) continue;

        int idx = 0;
        try {
            idx = std::stoi(key);
        } catch (const std::exception&) {
            spdlog::warn("[AMS CFS] Skipping invalid slot override key: {}", key);
            continue;
        }

        SlotOverride ovr;
        ovr.color_rgb = entry.value("color_rgb", static_cast<uint32_t>(0));
        ovr.material = entry.value("material", std::string{});
        ovr.color_name = entry.value("color_name", std::string{});
        ovr.brand = entry.value("brand", std::string{});
        ovr.spool_name = entry.value("spool_name", std::string{});
        ovr.spoolman_id = entry.value("spoolman_id", 0);
        ovr.remaining_weight_g = entry.value("remaining_weight_g", -1.0f);
        ovr.total_weight_g = entry.value("total_weight_g", -1.0f);

        slot_overrides_[idx] = std::move(ovr);

        spdlog::debug("[AMS CFS] Loaded slot override for slot {}: {} {}",
                      idx, slot_overrides_[idx].material, slot_overrides_[idx].color_name);
    }

    spdlog::info("[AMS CFS] Applied {} slot override(s) from persistent storage",
                 slot_overrides_.size());
}

void AmsBackendCfs::save_slot_overrides_json() const {
    namespace fs = std::filesystem;

    json json_data;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        json_data = slot_overrides_to_json();
    }

    auto path = fs::path(config_dir_) / SLOT_OVERRIDES_JSON;

    try {
        fs::create_directories(config_dir_);

        std::ofstream ofs(path);
        if (!ofs.is_open()) {
            spdlog::warn("[AMS CFS] Failed to open {} for writing", path.string());
            return;
        }
        ofs << json_data.dump(2);
        spdlog::debug("[AMS CFS] Saved slot overrides to {}", path.string());
    } catch (const std::exception& e) {
        spdlog::warn("[AMS CFS] Error saving slot overrides JSON: {}", e.what());
    }
}

bool AmsBackendCfs::load_slot_overrides_json() {
    namespace fs = std::filesystem;

    auto path = fs::path(config_dir_) / SLOT_OVERRIDES_JSON;

    if (!fs::exists(path)) {
        spdlog::debug("[AMS CFS] No slot overrides JSON file at {}", path.string());
        return false;
    }

    try {
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            spdlog::warn("[AMS CFS] Failed to open {}", path.string());
            return false;
        }

        auto data = json::parse(ifs);
        apply_slot_overrides_json(data);
        spdlog::info("[AMS CFS] Loaded slot overrides from {}", path.string());
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("[AMS CFS] Error loading slot overrides JSON: {}", e.what());
        return false;
    }
}

void AmsBackendCfs::save_slot_overrides() {
    // Always save to local JSON (fast, reliable)
    save_slot_overrides_json();

    // Fire-and-forget to Moonraker DB (async, best-effort)
    if (api_) {
        json json_data;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            json_data = slot_overrides_to_json();
        }
        api_->database_post_item(
            MOONRAKER_DB_NAMESPACE, MOONRAKER_DB_KEY_SLOTS, json_data,
            []() { spdlog::debug("[AMS CFS] Slot overrides saved to Moonraker DB"); },
            [](const MoonrakerError& err) {
                spdlog::warn("[AMS CFS] Failed to save slot overrides to Moonraker DB: {}",
                             err.user_message());
            });
    }
}

void AmsBackendCfs::load_slot_overrides() {
    if (!api_) {
        // No API — try local JSON only
        load_slot_overrides_json();
        return;
    }

    // Try Moonraker DB first. Callbacks fire from WebSocket thread,
    // so we marshal back to UI thread via queue_update().
    auto token = lifetime_.token();

    api_->database_get_item(
        MOONRAKER_DB_NAMESPACE, MOONRAKER_DB_KEY_SLOTS,
        [this, token](const json& data) {
            if (token.expired()) return;
            auto data_copy = std::make_unique<json>(data);
            helix::ui::queue_update<json>(
                std::move(data_copy), [this](json* d) {
                    apply_slot_overrides_json(*d);
                    save_slot_overrides_json();
                    emit_event(EVENT_SLOT_CHANGED);
                    spdlog::info("[AMS CFS] Loaded slot overrides from Moonraker DB");
                });
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired()) return;
            spdlog::debug("[AMS CFS] Moonraker DB load failed ({}), trying local JSON",
                          err.user_message());
            helix::ui::queue_update<int>(std::make_unique<int>(0), [this](int*) {
                load_slot_overrides_json();
                emit_event(EVENT_SLOT_CHANGED);
            });
        });
}

} // namespace helix::printer

#endif // HELIX_HAS_CFS
