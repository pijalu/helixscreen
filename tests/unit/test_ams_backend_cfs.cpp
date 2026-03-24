// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_types.h"

#include "../catch_amalgamated.hpp"

using namespace helix::printer;

TEST_CASE("CFS type enum", "[ams][cfs]") {
    SECTION("CFS is a valid AmsType") {
        auto t = AmsType::CFS;
        REQUIRE(t != AmsType::NONE);
    }

    SECTION("CFS is a filament system, not a tool changer") {
        REQUIRE(is_filament_system(AmsType::CFS));
        REQUIRE_FALSE(is_tool_changer(AmsType::CFS));
    }

    SECTION("ams_type_to_string returns CFS") {
        REQUIRE(std::string(ams_type_to_string(AmsType::CFS)) == "CFS");
    }

    SECTION("ams_type_from_string parses CFS variants") {
        REQUIRE(ams_type_from_string("cfs") == AmsType::CFS);
        REQUIRE(ams_type_from_string("CFS") == AmsType::CFS);
    }
}

TEST_CASE("CFS data model extensions", "[ams][cfs]") {
    SECTION("EnvironmentData defaults") {
        EnvironmentData env;
        REQUIRE(env.temperature_c == 0.0f);
        REQUIRE(env.humidity_pct == 0.0f);
    }

    SECTION("AmsUnit environment is optional") {
        AmsUnit unit;
        REQUIRE_FALSE(unit.environment.has_value());
        unit.environment = EnvironmentData{27.0f, 48.0f};
        REQUIRE(unit.environment->temperature_c == 27.0f);
        REQUIRE(unit.environment->humidity_pct == 48.0f);
    }

    SECTION("SlotInfo remaining length defaults to zero") {
        SlotInfo slot;
        REQUIRE(slot.remaining_length_m == 0.0f);
    }

    SECTION("SlotInfo environment is optional") {
        SlotInfo slot;
        REQUIRE_FALSE(slot.environment.has_value());
    }

    SECTION("AmsAlert fields") {
        AmsAlert alert;
        alert.message = "Nozzle clog detected";
        alert.hint = "Run a cold pull";
        alert.error_code = "CFS-845";
        alert.level = AmsAlertLevel::SYSTEM;
        REQUIRE(alert.level == AmsAlertLevel::SYSTEM);
        REQUIRE(alert.error_code == "CFS-845");
    }

    SECTION("AmsSystemInfo has alerts vector") {
        AmsSystemInfo info;
        REQUIRE(info.alerts.empty());
        info.alerts.push_back(AmsAlert{
            .message = "test",
            .hint = "fix it",
            .error_code = "CFS-831",
            .level = AmsAlertLevel::SYSTEM
        });
        REQUIRE(info.alerts.size() == 1);
    }
}
