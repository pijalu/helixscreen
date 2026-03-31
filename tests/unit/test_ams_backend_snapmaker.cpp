// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_types.h"
#include "printer_discovery.h"

#include "../catch_amalgamated.hpp"

using namespace helix::printer;
using namespace helix;

TEST_CASE("Snapmaker type enum", "[ams][snapmaker]") {
    SECTION("SNAPMAKER is a valid AmsType") {
        auto t = AmsType::SNAPMAKER;
        REQUIRE(t != AmsType::NONE);
        REQUIRE(static_cast<int>(t) == 7);
    }

    SECTION("SNAPMAKER is both a tool changer and filament system") {
        REQUIRE(is_tool_changer(AmsType::SNAPMAKER));
        REQUIRE(is_filament_system(AmsType::SNAPMAKER));
    }

    SECTION("ams_type_to_string returns Snapmaker") {
        REQUIRE(std::string(ams_type_to_string(AmsType::SNAPMAKER)) == "Snapmaker");
    }

    SECTION("ams_type_from_string parses Snapmaker variants") {
        REQUIRE(ams_type_from_string("snapmaker") == AmsType::SNAPMAKER);
        REQUIRE(ams_type_from_string("Snapmaker") == AmsType::SNAPMAKER);
        REQUIRE(ams_type_from_string("snapswap") == AmsType::SNAPMAKER);
    }
}

TEST_CASE("Snapmaker detection via filament_detect", "[ams][snapmaker]") {
    PrinterDiscovery discovery;

    SECTION("filament_detect triggers SNAPMAKER detection") {
        nlohmann::json objects = nlohmann::json::array({
            "extruder", "extruder1", "extruder2", "extruder3",
            "toolchanger", "filament_detect", "toolhead",
            "heater_bed", "print_task_config"
        });
        discovery.parse_objects(objects);
        REQUIRE(discovery.has_snapmaker());
        REQUIRE(discovery.mmu_type() == AmsType::SNAPMAKER);
    }

    SECTION("empty toolchanger without filament_detect is not SNAPMAKER") {
        nlohmann::json objects = nlohmann::json::array({
            "extruder", "toolchanger", "tool T0", "tool T1", "toolhead"
        });
        discovery.parse_objects(objects);
        REQUIRE_FALSE(discovery.has_snapmaker());
        REQUIRE(discovery.has_tool_changer());
    }
}

// ============================================================================
// Backend Construction Tests
// ============================================================================

#include "ams_backend_snapmaker.h"
#include "hv/json.hpp"

TEST_CASE("AmsBackendSnapmaker construction", "[ams][snapmaker]") {
    SECTION("type returns SNAPMAKER") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        REQUIRE(backend.get_type() == AmsType::SNAPMAKER);
    }

    SECTION("topology is PARALLEL") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        REQUIRE(backend.get_topology() == PathTopology::PARALLEL);
    }

    SECTION("name is Snapmaker SnapSwap") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.type_name == "Snapmaker SnapSwap");
    }

    SECTION("has 4 slots in 1 unit") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.total_slots == 4);
        REQUIRE(info.units.size() == 1);
        REQUIRE(info.units[0].slot_count == 4);
    }
}

// ============================================================================
// Extruder State Parser Tests
// ============================================================================

TEST_CASE("Snapmaker extruder state parsing", "[ams][snapmaker]") {
    SECTION("parses parked extruder") {
        auto j = nlohmann::json::parse(R"({
            "state": "PARKED",
            "park_pin": true,
            "active_pin": false,
            "grab_valid_pin": false,
            "activating_move": false,
            "extruder_offset": [0.073, -0.037, 0.0],
            "switch_count": 86,
            "retry_count": 0,
            "error_count": 1
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state == "PARKED");
        REQUIRE(state.park_pin == true);
        REQUIRE(state.active_pin == false);
        REQUIRE(state.activating_move == false);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(0.073f));
        REQUIRE(state.extruder_offset[1] == Catch::Approx(-0.037f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
        REQUIRE(state.switch_count == 86);
        REQUIRE(state.retry_count == 0);
        REQUIRE(state.error_count == 1);
    }

    SECTION("parses active extruder") {
        auto j = nlohmann::json::parse(R"({
            "state": "ACTIVE",
            "park_pin": false,
            "active_pin": true,
            "activating_move": false,
            "extruder_offset": [0.0, 0.0, 0.0],
            "switch_count": 12,
            "retry_count": 2,
            "error_count": 0
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state == "ACTIVE");
        REQUIRE(state.park_pin == false);
        REQUIRE(state.active_pin == true);
        REQUIRE(state.switch_count == 12);
        REQUIRE(state.retry_count == 2);
        REQUIRE(state.error_count == 0);
    }

    SECTION("parses activating move in progress") {
        auto j = nlohmann::json::parse(R"({
            "state": "ACTIVATING",
            "park_pin": false,
            "active_pin": false,
            "activating_move": true,
            "extruder_offset": [0.0, 0.0, 0.0],
            "switch_count": 5,
            "retry_count": 0,
            "error_count": 0
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state == "ACTIVATING");
        REQUIRE(state.activating_move == true);
    }

    SECTION("handles missing fields gracefully") {
        auto j = nlohmann::json::parse("{}");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state.empty());
        REQUIRE(state.park_pin == false);
        REQUIRE(state.active_pin == false);
        REQUIRE(state.activating_move == false);
        REQUIRE(state.switch_count == 0);
        REQUIRE(state.retry_count == 0);
        REQUIRE(state.error_count == 0);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[1] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
    }

    SECTION("handles partial extruder_offset array") {
        auto j = nlohmann::json::parse(R"({
            "state": "PARKED",
            "extruder_offset": [1.5]
        })");
        auto state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(1.5f));
        // Missing indices stay at default
        REQUIRE(state.extruder_offset[1] == Catch::Approx(0.0f));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0f));
    }
}

// ============================================================================
// RFID Info Parser Tests
// ============================================================================

TEST_CASE("Snapmaker RFID info parsing", "[ams][snapmaker]") {
    SECTION("parses full RFID tag data") {
        // ARGB 0xFF080A0D -> RGB 0x080A0D
        auto j = nlohmann::json::parse(R"({
            "VERSION": 1,
            "VENDOR": "Snapmaker",
            "MANUFACTURER": "Polymaker",
            "MAIN_TYPE": "PLA",
            "SUB_TYPE": "SnapSpeed",
            "ARGB_COLOR": 4278716941,
            "DIAMETER": 175,
            "WEIGHT": 500,
            "HOTEND_MAX_TEMP": 230,
            "HOTEND_MIN_TEMP": 190,
            "BED_TEMP": 60,
            "OFFICIAL": true
        })");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type == "PLA");
        REQUIRE(info.sub_type == "SnapSpeed");
        REQUIRE(info.manufacturer == "Polymaker");
        REQUIRE(info.vendor == "Snapmaker");
        REQUIRE(info.hotend_min_temp == 190);
        REQUIRE(info.hotend_max_temp == 230);
        REQUIRE(info.bed_temp == 60);
        REQUIRE(info.weight_g == 500);
        // ARGB 4278716941 = 0xFF080A0D → mask off alpha → 0x080A0D
        REQUIRE(info.color_rgb == 0x080A0Du);
    }

    SECTION("ARGB alpha byte is masked to produce RGB") {
        // 0xFF0000FF (opaque blue) -> 0x0000FF
        auto j = nlohmann::json::parse(R"({"ARGB_COLOR": 4278190335})");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.color_rgb == 0x0000FFu);
    }

    SECTION("stores both MANUFACTURER and VENDOR independently") {
        auto j = nlohmann::json::parse(R"({
            "VENDOR": "Generic",
            "MANUFACTURER": "",
            "MAIN_TYPE": "PETG"
        })");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        // Parser stores fields as-is; brand fallback logic is in handle_status_update
        REQUIRE(info.vendor == "Generic");
        REQUIRE(info.manufacturer.empty());
        REQUIRE(info.main_type == "PETG");
    }

    SECTION("handles missing RFID fields with safe defaults") {
        auto j = nlohmann::json::parse("{}");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type.empty());
        REQUIRE(info.sub_type.empty());
        REQUIRE(info.manufacturer.empty());
        REQUIRE(info.vendor.empty());
        REQUIRE(info.hotend_min_temp == 0);
        REQUIRE(info.hotend_max_temp == 0);
        REQUIRE(info.bed_temp == 0);
        REQUIRE(info.weight_g == 0);
        // Default color is 0x808080 (grey)
        REQUIRE(info.color_rgb == 0x808080u);
    }

    SECTION("parses PETG with different temperatures") {
        auto j = nlohmann::json::parse(R"({
            "MANUFACTURER": "Generic3D",
            "MAIN_TYPE": "PETG",
            "SUB_TYPE": "Basic",
            "HOTEND_MIN_TEMP": 220,
            "HOTEND_MAX_TEMP": 250,
            "BED_TEMP": 80,
            "WEIGHT": 1000
        })");
        auto info = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(info.main_type == "PETG");
        REQUIRE(info.sub_type == "Basic");
        REQUIRE(info.manufacturer == "Generic3D");
        REQUIRE(info.hotend_min_temp == 220);
        REQUIRE(info.hotend_max_temp == 250);
        REQUIRE(info.bed_temp == 80);
        REQUIRE(info.weight_g == 1000);
    }
}
