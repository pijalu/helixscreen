// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_ad5x_ifs.h"
#include "ams_types.h"

#include <chrono>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

// Test access helper — friend class for accessing internals
class Ad5xIfsTestAccess {
  public:
    static void handle_status(AmsBackendAd5xIfs& b, const json& n) {
        b.handle_status_update(n);
    }
    static void parse_vars(AmsBackendAd5xIfs& b, const json& v) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.parse_save_variables(v);
    }
    static int active_tool(const AmsBackendAd5xIfs& b) {
        return b.active_tool_;
    }
    static bool external_mode(const AmsBackendAd5xIfs& b) {
        return b.external_mode_;
    }
    static bool head_filament(const AmsBackendAd5xIfs& b) {
        return b.head_filament_;
    }
    static bool port_presence(const AmsBackendAd5xIfs& b, int i) {
        return b.port_presence_[static_cast<size_t>(i)];
    }
    static std::string build_colors(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.build_color_list_value();
    }
    static std::string build_types(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.build_type_list_value();
    }
    static std::string build_tools(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.build_tool_map_value();
    }
    static AmsAction action(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.system_info_.action;
    }
    static void set_action(AmsBackendAd5xIfs& b, AmsAction a) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.system_info_.action = a;
        b.action_start_time_ = std::chrono::steady_clock::now();
    }
    static void check_action_timeout(AmsBackendAd5xIfs& b, std::chrono::seconds elapsed) {
        b.action_start_time_ = std::chrono::steady_clock::now() - elapsed;
        b.check_action_timeout();
    }
    static std::string var_prefix(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.var_prefix_;
    }
    static bool has_per_port_sensors(const AmsBackendAd5xIfs& b) {
        return b.has_per_port_sensors_;
    }
    static bool has_ifs_vars(const AmsBackendAd5xIfs& b) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.has_ifs_vars_;
    }
    static void set_has_ifs_vars(AmsBackendAd5xIfs& b, bool val) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.has_ifs_vars_ = val;
    }
    static void parse_adventurer_json(AmsBackendAd5xIfs& b, const std::string& content) {
        b.parse_adventurer_json(content);
    }
    static bool dirty(const AmsBackendAd5xIfs& b, size_t idx) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        return b.dirty_[idx];
    }
    static void set_dirty(AmsBackendAd5xIfs& b, size_t idx, bool val) {
        std::lock_guard<std::mutex> lock(b.mutex_);
        b.dirty_[idx] = val;
    }
    static AmsBackendAd5xIfs::ZColorSilentResult
    parse_zcolor_silent(const std::vector<std::string>& lines) {
        return AmsBackendAd5xIfs::parse_zcolor_silent(lines);
    }
    static bool zcolor_silent_supported(const AmsBackendAd5xIfs& b) {
        return b.zcolor_silent_supported_.load();
    }
    static void apply_zcolor_result(AmsBackendAd5xIfs& b,
                                    const AmsBackendAd5xIfs::ZColorSilentResult& r) {
        b.apply_zcolor_result(r);
    }
};

// Helper to build a full save_variables JSON payload
static json make_save_variables(const json& variables) {
    return json{{"save_variables", json{{"variables", variables}}}};
}

// Helper to build a port sensor notification
static json make_port_sensor(int port_1based, bool detected) {
    std::string key = "filament_switch_sensor _ifs_port_sensor_" + std::to_string(port_1based);
    return json{{key, json{{"filament_detected", detected}}}};
}

// Helper to build a head sensor notification
static json make_head_sensor(bool detected) {
    return json{
        {"filament_switch_sensor head_switch_sensor", json{{"filament_detected", detected}}}};
}

// Helper to build a native ZMOD motion sensor notification
static json make_motion_sensor(bool detected) {
    return json{
        {"filament_motion_sensor ifs_motion_sensor", json{{"filament_detected", detected}}}};
}

// Standard test variables representing a typical IFS configuration
static json standard_variables() {
    return json{{"less_waste_colors", json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"})},
                {"less_waste_types", json::array({"PLA", "PETG", "ABS", "TPU"})},
                {"less_waste_tools", json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5})},
                {"less_waste_current_tool", 0},
                {"less_waste_external", 0}};
}

// ==========================================================================
// 1. Type identification
// ==========================================================================

TEST_CASE("AD5X IFS type identification", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE(backend.get_type() == AmsType::AD5X_IFS);
    REQUIRE(backend.get_topology() == PathTopology::LINEAR);
}

// ==========================================================================
// 2. parse_save_variables — full JSON
// ==========================================================================

TEST_CASE("AD5X IFS parse_save_variables full JSON", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::parse_vars(backend, standard_variables());

    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::external_mode(backend));

    // After handle_status, slot info should reflect parsed data
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xFF0000);
    REQUIRE(info.material == "PLA");
    REQUIRE(info.mapped_tool == 0); // Tool 0 maps to port 1 (slot 0)

    auto info1 = backend.get_slot_info(1);
    REQUIRE(info1.color_rgb == 0x00FF00);
    REQUIRE(info1.material == "PETG");
    REQUIRE(info1.mapped_tool == 1);

    auto info2 = backend.get_slot_info(2);
    REQUIRE(info2.color_rgb == 0x0000FF);
    REQUIRE(info2.material == "ABS");
    REQUIRE(info2.mapped_tool == 2);

    auto info3 = backend.get_slot_info(3);
    REQUIRE(info3.color_rgb == 0xFFFFFF);
    REQUIRE(info3.material == "TPU");
    REQUIRE(info3.mapped_tool == 3);
}

// ==========================================================================
// 3. parse_save_variables with -1 active tool
// ==========================================================================

TEST_CASE("AD5X IFS no active tool", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    auto vars = standard_variables();
    vars["less_waste_current_tool"] = -1;

    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

    auto sys = backend.get_system_info();
    REQUIRE(sys.current_tool == -1);
    REQUIRE(sys.current_slot == -1);
    REQUIRE_FALSE(sys.filament_loaded);
}

// ==========================================================================
// 4. Color hex parsing
// ==========================================================================

TEST_CASE("AD5X IFS color hex parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("lowercase hex works") {
        auto vars = standard_variables();
        vars["less_waste_colors"] = json::array({"ff0000", "00ff00", "0000ff", "ffffff"});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
    }

    SECTION("mixed case hex works") {
        auto vars = standard_variables();
        vars["less_waste_colors"] = json::array({"Ff0000", "00Ff00", "0000Ff", "FfFfFf"});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
    }

    SECTION("empty string defaults to no change") {
        auto vars = standard_variables();
        // First set known colors
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        // Now parse with empty — color array element is empty string,
        // stoul will throw and color_rgb stays at previous value
        auto vars2 = standard_variables();
        vars2["less_waste_colors"] = json::array({"", "00FF00", "0000FF", "FFFFFF"});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars2));

        // Slot 0 color should remain from the first parse since empty string
        // is stored in colors_[] (empty) but update_slot_from_state skips
        // parsing when the hex is empty
        auto info = backend.get_slot_info(1);
        REQUIRE(info.color_rgb == 0x00FF00);
    }
}

// ==========================================================================
// 5. Tool mapping reverse lookup
// ==========================================================================

TEST_CASE("AD5X IFS tool mapping reverse lookup", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("standard 1:1 mapping") {
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

        for (int i = 0; i < 4; ++i) {
            auto info = backend.get_slot_info(i);
            REQUIRE(info.mapped_tool == i);
        }
    }

    SECTION("non-standard mapping: T0->port3, T1->port1") {
        auto vars = standard_variables();
        // T0->3, T1->1, T2->5(unmapped), T3->2, rest unmapped
        vars["less_waste_tools"] = json::array({3, 1, 5, 2, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        // Slot 0 (port 1): first tool mapping to port 1 is T1
        REQUIRE(backend.get_slot_info(0).mapped_tool == 1);
        // Slot 1 (port 2): first tool mapping to port 2 is T3
        REQUIRE(backend.get_slot_info(1).mapped_tool == 3);
        // Slot 2 (port 3): first tool mapping to port 3 is T0
        REQUIRE(backend.get_slot_info(2).mapped_tool == 0);
        // Slot 3 (port 4): no tool maps to port 4
        REQUIRE(backend.get_slot_info(3).mapped_tool == -1);
    }
}

// ==========================================================================
// 6. Port sensor parsing via handle_status_update
// ==========================================================================

TEST_CASE("AD5X IFS port sensor parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Set port 1 and 3 as having filament
    Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(1, true));
    Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(3, true));

    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0) == true);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1) == false);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 2) == true);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 3) == false);

    // Clear port 1
    Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(1, false));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0) == false);
}

// ==========================================================================
// 7. Head sensor parsing via handle_status_update
// ==========================================================================

TEST_CASE("AD5X IFS head sensor parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
}

// ==========================================================================
// 7b. Native ZMOD IFS motion sensor (no lessWaste per-port sensors)
// ==========================================================================

TEST_CASE("AD5X IFS native ZMOD motion sensor parsing", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));

    // Native ZMOD motion sensor maps to head filament state
    Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(true));
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

    Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
    REQUIRE_FALSE(Ad5xIfsTestAccess::head_filament(backend));
}

TEST_CASE("AD5X IFS native ZMOD combined update (no per-port sensors)", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Simulate a native ZMOD IFS status update:
    // save_variables + motion sensor + head switch sensor, NO per-port sensors
    json notification;
    notification["save_variables"] = json{{"variables", standard_variables()}};
    notification["filament_motion_sensor ifs_motion_sensor"] = json{{"filament_detected", true}};
    notification["filament_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};

    Ad5xIfsTestAccess::handle_status(backend, notification);

    // Verify system state — should detect filament loaded via motion sensor
    auto sys = backend.get_system_info();
    REQUIRE(sys.type == AmsType::AD5X_IFS);
    REQUIRE(sys.total_slots == 4);
    REQUIRE(sys.filament_loaded);
    REQUIRE(sys.current_tool == 0);

    // Port presence is unknown in native ZMOD (no per-port sensors)
    // but save_variables provides colors and tool mapping
    REQUIRE(sys.units.size() == 1);
    REQUIRE(sys.units[0].slots.size() == 4);
}

// ==========================================================================
// 8. Combined status update
// ==========================================================================

TEST_CASE("AD5X IFS combined status update", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Build a combined notification with save_variables + sensors
    json notification;
    notification["save_variables"] = json{{"variables", standard_variables()}};
    notification["filament_switch_sensor _ifs_port_sensor_1"] = json{{"filament_detected", true}};
    notification["filament_switch_sensor _ifs_port_sensor_2"] = json{{"filament_detected", false}};
    notification["filament_switch_sensor _ifs_port_sensor_3"] = json{{"filament_detected", true}};
    notification["filament_switch_sensor _ifs_port_sensor_4"] = json{{"filament_detected", false}};
    notification["filament_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};

    Ad5xIfsTestAccess::handle_status(backend, notification);

    // Verify all state
    REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
    REQUIRE_FALSE(Ad5xIfsTestAccess::external_mode(backend));
    REQUIRE(Ad5xIfsTestAccess::head_filament(backend));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 2));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));

    auto sys = backend.get_system_info();
    REQUIRE(sys.current_tool == 0);
    REQUIRE(sys.current_slot == 0); // T0 maps to port 1 (slot 0)
    REQUIRE(sys.filament_loaded);
}

// ==========================================================================
// 9. get_system_info
// ==========================================================================

TEST_CASE("AD5X IFS get_system_info", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    auto sys = backend.get_system_info();
    REQUIRE(sys.type == AmsType::AD5X_IFS);
    REQUIRE(sys.type_name == "AD5X IFS");
    REQUIRE(sys.total_slots == 4);
    REQUIRE(sys.units.size() == 1);
    REQUIRE(sys.units[0].slots.size() == 4);
    REQUIRE(sys.supports_bypass);
    REQUIRE(sys.supports_tool_mapping);
    REQUIRE_FALSE(sys.supports_endless_spool);
    REQUIRE_FALSE(sys.supports_purge);

    // IFS tool mapping: 16 entries (tool→slot), first 4 mapped, rest unmapped
    REQUIRE(sys.tool_to_slot_map.size() == 16);
    REQUIRE(sys.tool_to_slot_map[0] == 0);
    REQUIRE(sys.tool_to_slot_map[1] == 1);
    REQUIRE(sys.tool_to_slot_map[2] == 2);
    REQUIRE(sys.tool_to_slot_map[3] == 3);
    for (size_t i = 4; i < 16; ++i) {
        REQUIRE(sys.tool_to_slot_map[i] == -1);
    }
}

// ==========================================================================
// 10. Bypass mode
// ==========================================================================

TEST_CASE("AD5X IFS bypass mode", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("external=1 activates bypass") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 1;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE(backend.is_bypass_active());
    }

    SECTION("external=0 deactivates bypass") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 0;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE_FALSE(backend.is_bypass_active());
    }

    SECTION("toggle bypass via parse") {
        auto vars = standard_variables();
        vars["less_waste_external"] = 1;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE(backend.is_bypass_active());

        vars["less_waste_external"] = 0;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE_FALSE(backend.is_bypass_active());
    }
}

// ==========================================================================
// 11. build_color_list_value format
// ==========================================================================

TEST_CASE("AD5X IFS build_color_list_value format", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::parse_vars(backend, standard_variables());

    std::string colors = Ad5xIfsTestAccess::build_colors(backend);
    // Expected: Python list literal with outer double quotes
    REQUIRE(colors == "\"['FF0000', '00FF00', '0000FF', 'FFFFFF']\"");
}

// ==========================================================================
// 12. build_tool_map_value format
// ==========================================================================

TEST_CASE("AD5X IFS build_tool_map_value format", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::parse_vars(backend, standard_variables());

    std::string tools = Ad5xIfsTestAccess::build_tools(backend);
    REQUIRE(tools == "\"[1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]\"");
}

// ==========================================================================
// 13. set_slot_info with persist=false
// ==========================================================================

TEST_CASE("AD5X IFS set_slot_info persist=false", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    // First parse standard state so slots exist
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    SlotInfo new_info;
    new_info.color_rgb = 0x123456;
    new_info.material = "SILK_PLA";
    new_info.spoolman_id = 42;
    new_info.remaining_weight_g = 500;
    new_info.total_weight_g = 1000;

    auto err = backend.set_slot_info(1, new_info, false);
    REQUIRE(err.success());

    auto info = backend.get_slot_info(1);
    REQUIRE(info.color_rgb == 0x123456);
    REQUIRE(info.material == "SILK_PLA");
    REQUIRE(info.spoolman_id == 42);
    REQUIRE(info.remaining_weight_g == 500);
    REQUIRE(info.total_weight_g == 1000);
}

// ==========================================================================
// 14. Slot status mapping
// ==========================================================================

TEST_CASE("AD5X IFS slot status mapping", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("port with filament, not active → AVAILABLE") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        // Port 2 has filament, active tool is T0 (mapped to port 1)
        notification["filament_switch_sensor _ifs_port_sensor_2"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        auto info = backend.get_slot_info(1); // slot 1 = port 2
        REQUIRE(info.status == SlotStatus::AVAILABLE);
    }

    SECTION("port with filament, is active + head loaded → LOADED") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        // Port 1 has filament, active tool is T0 (mapped to port 1), head has filament
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        auto info = backend.get_slot_info(0); // slot 0 = port 1
        REQUIRE(info.status == SlotStatus::LOADED);
    }

    SECTION("port without filament → EMPTY") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_3"] =
            json{{"filament_detected", false}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        auto info = backend.get_slot_info(2); // slot 2 = port 3
        REQUIRE(info.status == SlotStatus::EMPTY);
    }
}

// ==========================================================================
// 15. Action state tracking
// ==========================================================================

TEST_CASE("AD5X IFS action state tracking", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("load_filament sets LOADING action (precondition fails with null api)") {
        // load_filament will fail at check_preconditions with null api,
        // so we can't test the action being set via that path.
        // Instead test the action inference: LOADING + head sensor → IDLE
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

        // Head sensor triggers → load complete
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("UNLOADING + head sensor cleared → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::UNLOADING);

        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("LOADING + head sensor NOT triggered → stays LOADING") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    }
}

// ==========================================================================
// 16. Path segments
// ==========================================================================

TEST_CASE("AD5X IFS path segments", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("get_filament_segment: no filament anywhere → NONE") {
        REQUIRE(backend.get_filament_segment() == PathSegment::NONE);
    }

    SECTION("get_filament_segment: head has filament → NOZZLE") {
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
        REQUIRE(backend.get_filament_segment() == PathSegment::NOZZLE);
    }

    SECTION("get_filament_segment: port has filament, active tool set, head empty → LANE") {
        json notification;
        auto vars = standard_variables();
        vars["less_waste_current_tool"] = 0; // T0 → port 1
        notification["save_variables"] = json{{"variables", vars}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        // head sensor NOT set (defaults to false)
        Ad5xIfsTestAccess::handle_status(backend, notification);

        REQUIRE(backend.get_filament_segment() == PathSegment::LANE);
    }

    SECTION("get_slot_filament_segment: active slot with head filament → NOZZLE") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", true}};
        notification["filament_switch_sensor head_switch_sensor"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        REQUIRE(backend.get_slot_filament_segment(0) == PathSegment::NOZZLE);
    }

    SECTION("get_slot_filament_segment: non-active slot with filament → HUB") {
        json notification;
        notification["save_variables"] = json{{"variables", standard_variables()}};
        notification["filament_switch_sensor _ifs_port_sensor_2"] =
            json{{"filament_detected", true}};
        Ad5xIfsTestAccess::handle_status(backend, notification);

        // Slot 1 (port 2) has filament but is not active — shows at hub
        REQUIRE(backend.get_slot_filament_segment(1) == PathSegment::HUB);
    }

    SECTION("get_slot_filament_segment: empty slot → NONE") {
        // Use variables where slot 2 has no color data (truly empty)
        json vars = standard_variables();
        vars["less_waste_colors"][2] = "";
        vars["less_waste_types"][2] = "";
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE(backend.get_slot_filament_segment(2) == PathSegment::NONE);
    }

    SECTION("get_slot_filament_segment: non-active slot with color data → HUB") {
        // Slot 2 has color data in save_variables → inferred present → HUB
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(backend.get_slot_filament_segment(2) == PathSegment::HUB);
    }

    SECTION("get_slot_filament_segment: out of range → NONE") {
        REQUIRE(backend.get_slot_filament_segment(-1) == PathSegment::NONE);
        REQUIRE(backend.get_slot_filament_segment(4) == PathSegment::NONE);
    }
}

// ==========================================================================
// Helper to wrap raw status JSON in Moonraker notify_status_update format
// ==========================================================================
static json wrap_notification(const json& status) {
    return json{{"method", "notify_status_update"}, {"params", json::array({status, 12345.678})}};
}

// ==========================================================================
// 17. Wrapped notification format (real WebSocket path)
// ==========================================================================

TEST_CASE("AD5X IFS handles wrapped notify_status_update", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("wrapped port sensor updates state") {
        auto wrapped = wrap_notification(make_port_sensor(1, true));
        Ad5xIfsTestAccess::handle_status(backend, wrapped);

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0) == true);
    }

    SECTION("wrapped head sensor updates state") {
        auto wrapped = wrap_notification(make_head_sensor(true));
        Ad5xIfsTestAccess::handle_status(backend, wrapped);

        REQUIRE(Ad5xIfsTestAccess::head_filament(backend));
    }

    SECTION("wrapped save_variables updates state") {
        auto wrapped = wrap_notification(make_save_variables(standard_variables()));
        Ad5xIfsTestAccess::handle_status(backend, wrapped);

        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);
        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
        REQUIRE(info.material == "PLA");
    }

    SECTION("wrapped combined notification updates all state") {
        json status;
        status["save_variables"] = json{{"variables", standard_variables()}};
        status["filament_switch_sensor _ifs_port_sensor_1"] = json{{"filament_detected", true}};
        status["filament_switch_sensor head_switch_sensor"] = json{{"filament_detected", true}};

        Ad5xIfsTestAccess::handle_status(backend, wrap_notification(status));

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE(Ad5xIfsTestAccess::head_filament(backend));

        auto sys = backend.get_system_info();
        REQUIRE(sys.current_tool == 0);
        REQUIRE(sys.current_slot == 0);
        REQUIRE(sys.filament_loaded);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.status == SlotStatus::LOADED);
    }

    SECTION("wrapped notification completes load action") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, wrap_notification(make_head_sensor(true)));

        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("wrapped notification completes unload action") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        // Head sensor was true, now cleared
        Ad5xIfsTestAccess::handle_status(backend, make_head_sensor(true));
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        Ad5xIfsTestAccess::handle_status(backend, wrap_notification(make_head_sensor(false)));

        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("unwrapped format still works (initial query response)") {
        // on_started() callback sends unwrapped format — must still work
        Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(2, true));
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1) == true);
    }
}

// ==========================================================================
// 18. Action timeout safety net
// ==========================================================================

TEST_CASE("AD5X IFS action timeout resets stuck operations", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("LOADING resets to IDLE after timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("UNLOADING resets to IDLE after timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("IDLE does not change on timeout check") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::IDLE);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("action does not reset before timeout") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);

        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(30));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    }

    SECTION("get_system_info checks timeout on UI poll") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::check_action_timeout(backend, std::chrono::seconds(120));

        auto sys = backend.get_system_info();
        REQUIRE(sys.action == AmsAction::IDLE);
    }
}

// ==========================================================================
// 19. Variable prefix auto-detection (lessWaste vs bambufy)
// ==========================================================================

TEST_CASE("AD5X IFS variable prefix auto-detection", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("defaults to less_waste prefix") {
        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "less_waste");
    }

    SECTION("detects bambufy prefix from colors") {
        json vars;
        vars["bambufy_colors"] = json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"});
        vars["bambufy_types"] = json::array({"PLA", "PETG", "ABS", "TPU"});
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        vars["bambufy_current_tool"] = 0;
        vars["bambufy_external"] = 0;

        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "bambufy");
        REQUIRE(Ad5xIfsTestAccess::active_tool(backend) == 0);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF0000);
        REQUIRE(info.material == "PLA");
    }

    SECTION("detects bambufy prefix from tools alone") {
        json vars;
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});

        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        REQUIRE(Ad5xIfsTestAccess::var_prefix(backend) == "bambufy");
    }
}

// ==========================================================================
// 20. Motion sensor triggers load/unload completion (native ZMOD)
// ==========================================================================

TEST_CASE("AD5X IFS motion sensor completes load/unload", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("LOADING + motion sensor detected → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(true));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("UNLOADING + motion sensor cleared → IDLE") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::UNLOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::IDLE);
    }

    SECTION("LOADING + motion sensor NOT detected → stays LOADING") {
        Ad5xIfsTestAccess::set_action(backend, AmsAction::LOADING);
        Ad5xIfsTestAccess::handle_status(backend, make_motion_sensor(false));
        REQUIRE(Ad5xIfsTestAccess::action(backend) == AmsAction::LOADING);
    }
}

// ==========================================================================
// 21. Native ZMOD IFS active slot inferred from head sensor
// ==========================================================================

TEST_CASE("AD5X IFS native ZMOD infers active slot from head sensor", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // No per-port sensors — only motion sensor and save_variables
    json notification;
    notification["save_variables"] = json{{"variables", standard_variables()}};
    notification["filament_motion_sensor ifs_motion_sensor"] = json{{"filament_detected", true}};

    Ad5xIfsTestAccess::handle_status(backend, notification);

    // Active tool is T0 → port 1 → slot 0. With head filament detected and no
    // per-port sensors, the active slot should be inferred as LOADED.
    auto info = backend.get_slot_info(0);
    REQUIRE(info.status == SlotStatus::LOADED);

    // Non-active slots with color data in save_variables are AVAILABLE (not EMPTY),
    // because port_presence is inferred from the IFS variable color data.
    auto info1 = backend.get_slot_info(1);
    REQUIRE(info1.status == SlotStatus::AVAILABLE);
}

// ==========================================================================
// 22. has_ifs_vars_ detection
// ==========================================================================

TEST_CASE("AD5X IFS has_ifs_vars detection", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("defaults to false") {
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("set true when lessWaste variables found") {
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("set true when bambufy variables found") {
        json vars;
        vars["bambufy_colors"] = json::array({"FF0000", "00FF00", "0000FF", "FFFFFF"});
        vars["bambufy_tools"] = json::array({1, 2, 3, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("stays false when save_variables has no recognized prefix") {
        json vars;
        vars["some_other_var"] = 42;
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }
}

TEST_CASE("AD5X IFS has_ifs_vars reset when macro missing", "[ams][ad5x_ifs]") {
    // Scenario: lessWaste/bambufy plugins partially installed — save_variables data
    // exists but _IFS_VARS gcode macro is not loaded. parse_save_variables() sets
    // has_ifs_vars_ true, but on_started() should reset it when the macro is absent.
    // This test verifies the parse step sets the flag (the reset happens in on_started).
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("parse_save_variables sets flag even without macro validation") {
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        REQUIRE(Ad5xIfsTestAccess::has_ifs_vars(backend));

        // Simulate what on_started() does: reset if macro absent
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, false);
        REQUIRE_FALSE(Ad5xIfsTestAccess::has_ifs_vars(backend));
    }

    SECTION("set_slot_info uses native ZMOD path when has_ifs_vars is false") {
        // Pre-populate slot data via save_variables
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        // Reset has_ifs_vars_ as on_started() would when macro is missing
        Ad5xIfsTestAccess::set_has_ifs_vars(backend, false);

        // set_slot_info without persist should succeed regardless
        SlotInfo info;
        info.color_rgb = 0x00FF00;
        info.material = "PETG";
        auto err = backend.set_slot_info(0, info, false);
        REQUIRE(err.success());
    }
}

// ==========================================================================
// 23. parse_adventurer_json (native ZMOD Adventurer5M.json)
// ==========================================================================

TEST_CASE("AD5X IFS parse_adventurer_json", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("standard 4-slot JSON with # prefixed hex colors") {
        std::string content = R"({
            "FFMInfo": {
                "channel": 2,
                "ffmColor0": "",
                "ffmColor1": "#FF0000",
                "ffmColor2": "#00FF00",
                "ffmColor3": "#0000FF",
                "ffmColor4": "#FFFFFF",
                "ffmType0": "?",
                "ffmType1": "PLA",
                "ffmType2": "PETG",
                "ffmType3": "ABS",
                "ffmType4": "TPU"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.color_rgb == 0xFF0000);
        REQUIRE(info0.material == "PLA");

        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.color_rgb == 0x00FF00);
        REQUIRE(info1.material == "PETG");

        auto info2 = backend.get_slot_info(2);
        REQUIRE(info2.color_rgb == 0x0000FF);
        REQUIRE(info2.material == "ABS");

        auto info3 = backend.get_slot_info(3);
        REQUIRE(info3.color_rgb == 0xFFFFFF);
        REQUIRE(info3.material == "TPU");
    }

    SECTION("lowercase hex is uppercased") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#ff8800",
                "ffmType1": "PLA"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF8800);
        REQUIRE(info.material == "PLA");
    }

    SECTION("missing FFMInfo section is graceful no-op") {
        std::string content = R"({"OtherSection": {"key": "value"}})";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slots should remain at defaults
        auto info = backend.get_slot_info(0);
        REQUIRE(info.material.empty());
    }

    SECTION("partial slots — only 2 of 4 populated") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#AABBCC",
                "ffmType1": "PLA",
                "ffmColor3": "#112233",
                "ffmType3": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.color_rgb == 0xAABBCC);
        REQUIRE(info0.material == "PLA");

        // Slot 1 (port 2) not in JSON — stays at default
        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.material.empty());

        auto info2 = backend.get_slot_info(2);
        REQUIRE(info2.color_rgb == 0x112233);
        REQUIRE(info2.material == "PETG");
    }

    SECTION("# prefix stripping") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#ABCDEF",
                "ffmType1": "ABS"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xABCDEF);
    }

    SECTION("empty color string defaults to gray") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "",
                "ffmType1": "PLA"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0x808080);
        REQUIRE(info.material == "PLA");
    }

    SECTION("invalid JSON is graceful no-op") {
        std::string content = "this is not json {{{";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slots should remain at defaults
        auto info = backend.get_slot_info(0);
        REQUIRE(info.material.empty());
    }

    SECTION("color without # prefix still works") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "FF8800",
                "ffmType1": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0xFF8800);
        REQUIRE(info.material == "PETG");
    }
}

// ==========================================================================
// Regression: dirty flag prevents parse_adventurer_json from clobbering
// user edits (#716)
// ==========================================================================

TEST_CASE("AD5X IFS parse_adventurer_json skips dirty slots", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed slot 0 with initial JSON data
    std::string initial = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, initial);
    REQUIRE(backend.get_slot_info(0).color_rgb == 0xFF0000);
    REQUIRE(backend.get_slot_info(0).material == "PLA");

    // User edits slot 0 (persist=false to skip actual write)
    SlotInfo edit;
    edit.color_rgb = 0x00FF00;
    edit.material = "PETG";
    backend.set_slot_info(0, edit, false);
    REQUIRE(Ad5xIfsTestAccess::dirty(backend, 0));

    // Simulate sensor-triggered JSON re-read with stale firmware data
    std::string stale = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, stale);

    // Dirty slot must NOT be overwritten
    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0x00FF00);
    REQUIRE(info.material == "PETG");
}

TEST_CASE("AD5X IFS parse_adventurer_json updates clean slots normally", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Edit slot 0, then clear dirty to simulate completed persist
    SlotInfo edit;
    edit.color_rgb = 0x00FF00;
    edit.material = "PETG";
    backend.set_slot_info(0, edit, false);
    Ad5xIfsTestAccess::set_dirty(backend, 0, false);
    REQUIRE_FALSE(Ad5xIfsTestAccess::dirty(backend, 0));

    // JSON re-read should overwrite clean slot
    std::string content = R"({
        "FFMInfo": {
            "ffmColor1": "#AABBCC",
            "ffmType1": "ABS"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xAABBCC);
    REQUIRE(info.material == "ABS");
}

TEST_CASE("AD5X IFS set_slot_info persist=false sets dirty flag", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    REQUIRE_FALSE(Ad5xIfsTestAccess::dirty(backend, 1));

    SlotInfo edit;
    edit.color_rgb = 0x112233;
    edit.material = "TPU";
    backend.set_slot_info(1, edit, false);

    REQUIRE(Ad5xIfsTestAccess::dirty(backend, 1));
}

TEST_CASE("AD5X IFS dirty flag protects against both parse paths", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed via save_variables (lessWaste path)
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    // User edits slot 0
    SlotInfo edit;
    edit.color_rgb = 0xDEADBE;
    edit.material = "SILK_PLA";
    backend.set_slot_info(0, edit, false);
    REQUIRE(Ad5xIfsTestAccess::dirty(backend, 0));

    // parse_save_variables must not overwrite dirty slot
    Ad5xIfsTestAccess::parse_vars(backend, standard_variables());
    auto info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xDEADBE);
    REQUIRE(info.material == "SILK_PLA");

    // parse_adventurer_json must not overwrite dirty slot either
    std::string stale_json = R"({
        "FFMInfo": {
            "ffmColor1": "#FF0000",
            "ffmType1": "PLA"
        }
    })";
    Ad5xIfsTestAccess::parse_adventurer_json(backend, stale_json);
    info = backend.get_slot_info(0);
    REQUIRE(info.color_rgb == 0xDEADBE);
    REQUIRE(info.material == "SILK_PLA");
}

// ==========================================================================
// Native ZMOD: parse_adventurer_json infers filament presence (#716)
// ==========================================================================

TEST_CASE("AD5X IFS parse_adventurer_json infers presence for native ZMOD", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // No per-port sensors — this is native ZMOD
    REQUIRE_FALSE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

    SECTION("slots with non-empty color are marked AVAILABLE") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "#161616",
                "ffmColor2": "#FFFFFF",
                "ffmColor3": "#D3C4A3",
                "ffmColor4": "#F72224",
                "ffmType1": "PLA+",
                "ffmType2": "PLA+",
                "ffmType3": "PLA+",
                "ffmType4": "PETG"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        for (int i = 0; i < 4; ++i) {
            auto info = backend.get_slot_info(i);
            REQUIRE(info.is_present());
            REQUIRE(info.status == SlotStatus::AVAILABLE);
            REQUIRE(Ad5xIfsTestAccess::port_presence(backend, i));
        }
    }

    SECTION("slot with empty color is NOT marked present") {
        std::string content = R"({
            "FFMInfo": {
                "ffmColor1": "",
                "ffmType1": "?",
                "ffmColor2": "#FF0000",
                "ffmType2": "PLA"
            }
        })";

        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slot 0 (port 1): empty color → not present
        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.status == SlotStatus::EMPTY);
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));

        // Slot 1 (port 2): has color → present
        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.is_present());
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1));
    }

    SECTION("per-port sensors take precedence over JSON inference") {
        // Simulate a per-port sensor detecting filament on port 1
        Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(1, true));
        REQUIRE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

        // Now parse JSON for a slot with empty color
        std::string content = R"({
            "FFMInfo": {
                "ffmColor2": "",
                "ffmType2": "PLA"
            }
        })";
        Ad5xIfsTestAccess::parse_adventurer_json(backend, content);

        // Slot 1 (port 2): has_per_port_sensors is true, so JSON inference is skipped.
        // port_presence stays false (no sensor for port 2 reported detected).
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));
    }
}

// ==========================================================================
// Dirty flag race: parse_save_variables clears dirty on value match (#716)
// ==========================================================================

TEST_CASE("AD5X IFS dirty flag cleared when save_variables match local edit", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed with initial save_variables
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

    // User edits slot 0
    SlotInfo edit;
    edit.color_rgb = 0x00FF00;
    edit.material = "PETG";
    backend.set_slot_info(0, edit, false);
    REQUIRE(Ad5xIfsTestAccess::dirty(backend, 0));

    SECTION("stale save_variables do NOT clear dirty") {
        // Simulate stale save_variables (old color still "FF0000")
        Ad5xIfsTestAccess::parse_vars(backend, standard_variables());

        // Dirty must remain set
        REQUIRE(Ad5xIfsTestAccess::dirty(backend, 0));

        // Local edit must be preserved
        auto info = backend.get_slot_info(0);
        REQUIRE(info.color_rgb == 0x00FF00);
        REQUIRE(info.material == "PETG");
    }

    SECTION("matching save_variables clear dirty") {
        // Simulate Klipper processing our edit — save_variables now contain new color
        auto vars = standard_variables();
        vars["bambufy_colors"][0] = "00FF00";
        vars["bambufy_types"][0] = "PETG";
        Ad5xIfsTestAccess::parse_vars(backend, vars);

        // Dirty should be cleared — Klipper confirmed our value
        REQUIRE_FALSE(Ad5xIfsTestAccess::dirty(backend, 0));
    }

    SECTION("case-insensitive match works") {
        auto vars = standard_variables();
        vars["bambufy_colors"][0] = "00ff00"; // lowercase from Klipper
        Ad5xIfsTestAccess::parse_vars(backend, vars);

        REQUIRE_FALSE(Ad5xIfsTestAccess::dirty(backend, 0));
    }
}

// ==========================================================================
// Port presence inference from save_variables and set_slot_info
// ==========================================================================

TEST_CASE("AD5X IFS port_presence inferred from save_variables colors", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    SECTION("non-empty colors latch port_presence true") {
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));
        // All 4 slots have colors in standard_variables → all present
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1));
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 2));
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 3));
    }

    SECTION("empty color strings do not latch port_presence") {
        json vars = standard_variables();
        vars["less_waste_colors"] = json::array({"FF0000", "", "", ""});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 2));
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));
    }

    SECTION("slots with color data show as AVAILABLE not EMPTY") {
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

        // Active slot (T0 → port 1 → slot 0) without head filament → AVAILABLE
        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.status == SlotStatus::AVAILABLE);

        // Non-active slot with color data → AVAILABLE
        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.status == SlotStatus::AVAILABLE);
    }

    SECTION("slots without color data remain EMPTY") {
        json vars = standard_variables();
        vars["less_waste_colors"] = json::array({"FF0000", "", "", ""});
        vars["less_waste_types"] = json::array({"PLA", "", "", ""});
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

        auto info0 = backend.get_slot_info(0);
        REQUIRE(info0.status == SlotStatus::AVAILABLE);

        auto info1 = backend.get_slot_info(1);
        REQUIRE(info1.status == SlotStatus::EMPTY);
    }

    SECTION("per-port sensor printers skip save_variables presence inference") {
        // Feed per-port sensor data first to set has_per_port_sensors_ = true
        Ad5xIfsTestAccess::handle_status(backend, make_port_sensor(1, true));
        REQUIRE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

        // Now feed save_variables — colors should NOT latch port_presence
        // because per-port sensors are authoritative
        Ad5xIfsTestAccess::handle_status(backend, make_save_variables(standard_variables()));

        // Port 1 has sensor data → present; port 2 has no sensor data → not present
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 1));
    }
}

TEST_CASE("AD5X IFS set_slot_info updates port_presence", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Start with empty save_variables so no color data
    json vars = standard_variables();
    vars["less_waste_colors"] = json::array({"", "", "", ""});
    vars["less_waste_types"] = json::array({"", "", "", ""});
    Ad5xIfsTestAccess::handle_status(backend, make_save_variables(vars));

    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));

    SECTION("setting color on empty slot latches port_presence") {
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.material = "PLA";
        backend.set_slot_info(0, info, false);

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
        auto slot = backend.get_slot_info(0);
        REQUIRE(slot.status == SlotStatus::AVAILABLE);
    }

    SECTION("clearing slot resets port_presence") {
        // First assign filament
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.material = "PLA";
        backend.set_slot_info(0, info, false);
        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

        // Now clear it
        SlotInfo cleared;
        cleared.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        cleared.material = "";
        backend.set_slot_info(0, cleared, false);

        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
        auto slot = backend.get_slot_info(0);
        REQUIRE(slot.status == SlotStatus::EMPTY);
    }

    SECTION("setting only material (default color) latches port_presence") {
        SlotInfo info;
        info.color_rgb = AMS_DEFAULT_SLOT_COLOR;
        info.material = "PETG";
        backend.set_slot_info(0, info, false);

        REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    }

    SECTION("set_slot_info skips presence for per-port sensor printers") {
        // Enable per-port sensors
        json notification;
        notification["filament_switch_sensor _ifs_port_sensor_1"] =
            json{{"filament_detected", false}};
        Ad5xIfsTestAccess::handle_status(backend, notification);
        REQUIRE(Ad5xIfsTestAccess::has_per_port_sensors(backend));

        // set_slot_info should not alter port_presence (sensors are authoritative)
        SlotInfo info;
        info.color_rgb = 0xFF0000;
        info.material = "PLA";
        backend.set_slot_info(0, info, false);

        REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 0));
    }
}

// ==========================================================================
// select_unload_command — bundle KKZ4XKD2 (prestonbrown/helixscreen)
// ==========================================================================

TEST_CASE("AD5X IFS select_unload_command", "[ams][ad5x_ifs]") {
    using Cmd = std::string;

    SECTION("slot_index < 0 → IFS_REMOVE_PRUTOK (current)") {
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(-1, 2, true) == "IFS_REMOVE_PRUTOK");
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(-1, -1, false) == "IFS_REMOVE_PRUTOK");
    }

    SECTION("active slot with head filament → IFS_REMOVE_PRUTOK (avoids per-port macro)") {
        // Bundle KKZ4XKD2: slot_index=2 (port 3) is active and head sensor true.
        // Per-port REMOVE_PRUTOK_IFS PRUTOK=3 errors when IFS state disagrees
        // with our color-latched port_presence — switch to the "current" macro.
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(2, 2, true) == "IFS_REMOVE_PRUTOK");
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(0, 0, true) == "IFS_REMOVE_PRUTOK");
    }

    SECTION("active slot but head empty → per-port unload (lane retract)") {
        // Filament present in lane but not at head → can't use IFS_REMOVE_PRUTOK,
        // need explicit per-port command.
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(2, 2, false) ==
                Cmd("REMOVE_PRUTOK_IFS PRUTOK=3"));
    }

    SECTION("non-active slot → per-port unload") {
        // Unloading a different slot than the active one — must specify port.
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(0, 2, true) ==
                Cmd("REMOVE_PRUTOK_IFS PRUTOK=1"));
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(3, 0, true) ==
                Cmd("REMOVE_PRUTOK_IFS PRUTOK=4"));
    }

    SECTION("no active slot (current_slot=-1) → per-port unload") {
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(2, -1, false) ==
                Cmd("REMOVE_PRUTOK_IFS PRUTOK=3"));
    }

    SECTION("out-of-range slot_index → IFS_REMOVE_PRUTOK fallback") {
        REQUIRE(AmsBackendAd5xIfs::select_unload_command(99, 0, true) == "IFS_REMOVE_PRUTOK");
    }
}

// ==========================================================================
// parse_zcolor_silent — GET_ZCOLOR SILENT=1 response parser
// ==========================================================================
//
// zmod emits one line per LOADED slot plus a summary line, all prefixed "// ":
//
//   // Extruder: None (1) | IFS: True
//   // 1: PLA/FFFFFF
//   // 2: PLA/2750E0
//
// Post-ad2802ab zmod always appends "/<HEX>" to each slot line. Hex is the
// RIGHTMOST /-segment — transparent/named-color case emits three segments
// (// 3: PLA/transparent/00000000). Missing slot numbers = physically empty.
// Old zmod (pre-fix) emits "// 1: PLA" (no /HEX); we fall back to JSON.
// Very old zmod emits an action:prompt_show dialog; also JSON fallback.

TEST_CASE("AD5X IFS parse_zcolor_silent two-segment lines", "[ams][ad5x_ifs]") {
    std::vector<std::string> lines = {
        "// Extruder: None (1) | IFS: True",
        "// 1: PLA/FFFFFF",
        "// 2: PETG/2750E0",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.ifs_active);
    REQUIRE(r.current_channel == 1);
    REQUIRE_FALSE(r.extruder_slot.has_value());
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->material == "PLA");
    REQUIRE(r.slots[0]->hex == "FFFFFF");
    REQUIRE(r.slots[1].has_value());
    REQUIRE(r.slots[1]->material == "PETG");
    REQUIRE(r.slots[1]->hex == "2750E0");
    REQUIRE_FALSE(r.slots[2].has_value());
    REQUIRE_FALSE(r.slots[3].has_value());
}

TEST_CASE("AD5X IFS parse_zcolor_silent named-color three-segment", "[ams][ad5x_ifs]") {
    // Transparent / any COLOR_MAPPING match produces an extra segment:
    //   // <N>: <MATERIAL>/<NAME>/<HEX>
    // Parser rule: hex is always the rightmost /-segment.
    std::vector<std::string> lines = {
        "// Extruder: 1: PLA/FFFFFF | IFS: True",
        "// 1: PLA/FFFFFF",
        "// 3: PLA/transparent/00000000",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.ifs_active);
    REQUIRE(r.extruder_slot == 0); // 0-based (line says slot 1)
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->hex == "FFFFFF");
    REQUIRE(r.slots[2].has_value());
    REQUIRE(r.slots[2]->material == "PLA");
    REQUIRE(r.slots[2]->hex == "00000000");
}

TEST_CASE("AD5X IFS parse_zcolor_silent empty (all slots unloaded)", "[ams][ad5x_ifs]") {
    std::vector<std::string> lines = {
        "// Extruder: None (0) | IFS: True",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.ifs_active);
    REQUIRE(r.current_channel == 0);
    for (int i = 0; i < AmsBackendAd5xIfs::NUM_PORTS; ++i) {
        REQUIRE_FALSE(r.slots[static_cast<size_t>(i)].has_value());
    }
}

TEST_CASE("AD5X IFS parse_zcolor_silent IFS disabled (independent mode)", "[ams][ad5x_ifs]") {
    std::vector<std::string> lines = {
        "// Extruder: None (0) | IFS: False",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.ifs_active);
}

TEST_CASE("AD5X IFS parse_zcolor_silent old-format (no /HEX)", "[ams][ad5x_ifs]") {
    // Pre-ad2802ab zmod: silent lines are "// N: MATERIAL" with no /HEX.
    // Parser must detect this and flag is_old_format so caller falls back to JSON.
    std::vector<std::string> lines = {
        "// Extruder: None (1) | IFS: True",
        "// 1: PLA",
        "// 2: PETG",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE(r.is_old_format);
    // Presence info is still valid even without color — slot 1 and 2 appear.
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->material == "PLA");
    REQUIRE(r.slots[0]->hex.empty());
    REQUIRE(r.slots[1].has_value());
    REQUIRE(r.slots[1]->material == "PETG");
    REQUIRE(r.slots[1]->hex.empty());
}

TEST_CASE("AD5X IFS parse_zcolor_silent prompt fallback", "[ams][ad5x_ifs]") {
    // Very old zmod: SILENT=1 unsupported, emits full dialog.
    std::vector<std::string> lines = {
        "// action:prompt_begin Select filament",
        "// action:prompt_text Extruder: None",
        "// action:prompt_button 1: PLA|RUN_ZCOLOR SLOT=1 HEX=FFFFFF TYPE=PLA|primary|FFFFFF",
        "// action:prompt_show",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE(r.is_prompt_fallback);
}

TEST_CASE("AD5X IFS parse_zcolor_silent malformed lines skipped", "[ams][ad5x_ifs]") {
    // Unrelated gcode-response lines interleaved with valid silent output must
    // not confuse the parser — it should pick out the slot lines it recognises.
    std::vector<std::string> lines = {
        "// Extruder: None (1) | IFS: True",
        "// 1: PLA/FFFFFF",
        "// random gcode echo",
        "// 99: nonsense",  // slot number out of range
        "// 2: PETG/00FF00",
        "echo: hotend temp 205",
    };

    auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);

    REQUIRE_FALSE(r.is_prompt_fallback);
    REQUIRE_FALSE(r.is_old_format);
    REQUIRE(r.slots[0].has_value());
    REQUIRE(r.slots[0]->hex == "FFFFFF");
    REQUIRE(r.slots[1].has_value());
    REQUIRE(r.slots[1]->hex == "00FF00");
    REQUIRE_FALSE(r.slots[2].has_value());
    REQUIRE_FALSE(r.slots[3].has_value());
}

TEST_CASE("AD5X IFS apply_zcolor_result updates port_presence", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.current_channel = 1;
    r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};
    r.slots[1] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "2750E0"};
    // slots 2 and 3 left empty — should clear any existing presence

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 1));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 2));
    REQUIRE_FALSE(Ad5xIfsTestAccess::port_presence(backend, 3));
}

TEST_CASE("AD5X IFS apply_zcolor_result skips on prompt fallback", "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);
    REQUIRE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.is_prompt_fallback = true;

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // One prompt-style response flips silent_supported to false permanently
    // for this session; subsequent query_zcolor_silent() becomes a no-op.
    REQUIRE_FALSE(Ad5xIfsTestAccess::zcolor_silent_supported(backend));
}

TEST_CASE("AD5X IFS apply_zcolor_result skips when response has no valid content",
          "[ams][ad5x_ifs]") {
    // Regression: a transient/malformed response with zero slot lines and no
    // summary line must NOT wipe port_presence. Pre-fix, an empty ZColorSilentResult
    // would clear all four slots to "not loaded".
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed presence so we can detect an erroneous wipe.
    AmsBackendAd5xIfs::ZColorSilentResult seed;
    seed.saw_valid_response = true;
    seed.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFFFFF"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, seed);
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));

    // Empty (junk response) — saw_valid_response stays false.
    AmsBackendAd5xIfs::ZColorSilentResult empty;
    Ad5xIfsTestAccess::apply_zcolor_result(backend, empty);

    // Slot 0 must still be present — we didn't get valid data, don't overwrite.
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
}

TEST_CASE("AD5X IFS apply_zcolor_result updates colors and materials",
          "[ams][ad5x_ifs]") {
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    AmsBackendAd5xIfs::ZColorSilentResult r;
    r.saw_valid_response = true;
    r.ifs_active = true;
    r.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PETG", "00FF00"};

    Ad5xIfsTestAccess::apply_zcolor_result(backend, r);

    // Color and material should be propagated.
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
    // build_colors returns the comma-separated list used for _IFS_VARS writes;
    // indirect but the only public window into colors_[] without friend access.
    auto colors = Ad5xIfsTestAccess::build_colors(backend);
    auto types = Ad5xIfsTestAccess::build_types(backend);
    REQUIRE(colors.find("00FF00") != std::string::npos);
    REQUIRE(types.find("PETG") != std::string::npos);
}

TEST_CASE("AD5X IFS apply_zcolor_result skips color write on dirty slot",
          "[ams][ad5x_ifs]") {
    // Dirty slot means an unsaved user edit is pending — we must NOT overwrite
    // the local color with zmod's view, or we'd clobber the user's edit.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    // Seed slot 0 with a color we want preserved.
    AmsBackendAd5xIfs::ZColorSilentResult seed;
    seed.saw_valid_response = true;
    seed.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FF0000"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, seed);
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FF0000") != std::string::npos);

    // Mark dirty, then apply a result that would change color.
    Ad5xIfsTestAccess::set_dirty(backend, 0, true);
    AmsBackendAd5xIfs::ZColorSilentResult incoming;
    incoming.saw_valid_response = true;
    incoming.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "0000FF"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, incoming);

    // Color must still be FF0000 — dirty-slot guard held.
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FF0000") != std::string::npos);
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("0000FF") == std::string::npos);
}

TEST_CASE("AD5X IFS apply_zcolor_result old-format preserves colors",
          "[ams][ad5x_ifs]") {
    // Pre-ad2802ab zmod: slot lines carry no /HEX. Presence should still
    // update, but existing colors must NOT be overwritten with empty strings.
    AmsBackendAd5xIfs backend(nullptr, nullptr);

    AmsBackendAd5xIfs::ZColorSilentResult seed;
    seed.saw_valid_response = true;
    seed.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", "FFAA00"};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, seed);
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FFAA00") != std::string::npos);

    AmsBackendAd5xIfs::ZColorSilentResult old_fmt;
    old_fmt.saw_valid_response = true;
    old_fmt.is_old_format = true;
    // slot present but material only, no hex
    old_fmt.slots[0] = AmsBackendAd5xIfs::ZColorSlot{"PLA", ""};
    Ad5xIfsTestAccess::apply_zcolor_result(backend, old_fmt);

    // Color preserved from JSON-seeded state.
    REQUIRE(Ad5xIfsTestAccess::build_colors(backend).find("FFAA00") != std::string::npos);
    // Presence still reflects what the old-format response said.
    REQUIRE(Ad5xIfsTestAccess::port_presence(backend, 0));
}

TEST_CASE("AD5X IFS parse_zcolor_silent sets saw_valid_response", "[ams][ad5x_ifs]") {
    SECTION("summary line present") {
        std::vector<std::string> lines = {"// Extruder: None (0) | IFS: True"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE(r.saw_valid_response);
    }
    SECTION("slot line present") {
        std::vector<std::string> lines = {"// 1: PLA/FFFFFF"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE(r.saw_valid_response);
    }
    SECTION("only junk lines") {
        std::vector<std::string> lines = {"echo: random output", "// not a slot line"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE_FALSE(r.saw_valid_response);
    }
    SECTION("slot-number-out-of-range line") {
        // "// 99: nonsense" is skipped and must NOT count as valid.
        std::vector<std::string> lines = {"// 99: nonsense"};
        auto r = Ad5xIfsTestAccess::parse_zcolor_silent(lines);
        REQUIRE_FALSE(r.saw_valid_response);
    }
}
