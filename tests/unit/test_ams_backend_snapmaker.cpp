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
