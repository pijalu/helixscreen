// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_cfs.h"
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

using helix::printer::CfsMaterialDb;

TEST_CASE("CFS material database", "[ams][cfs]") {
    const auto& db = CfsMaterialDb::instance();

    SECTION("known material lookup") {
        auto info = db.lookup("01001");
        REQUIRE(info != nullptr);
        REQUIRE(info->brand == "Creality");
        REQUIRE(info->name == "Hyper PLA");
        REQUIRE(info->material_type == "PLA");
        REQUIRE(info->min_temp == 190);
        REQUIRE(info->max_temp == 240);
    }

    SECTION("unknown material returns nullptr") {
        auto info = db.lookup("99999");
        REQUIRE(info == nullptr);
    }

    SECTION("code stripping: 101001 to 01001") {
        auto id = CfsMaterialDb::strip_code("101001");
        REQUIRE(id == "01001");
    }

    SECTION("short code returned as-is") {
        auto id = CfsMaterialDb::strip_code("01001");
        REQUIRE(id == "01001");
    }

    SECTION("sentinel -1 returns empty") {
        auto id = CfsMaterialDb::strip_code("-1");
        REQUIRE(id.empty());
    }
}

TEST_CASE("CFS color parsing", "[ams][cfs]") {
    SECTION("parse 0RRGGBB format") {
        REQUIRE(CfsMaterialDb::parse_color("0C12E1F") == 0xC12E1F);
        REQUIRE(CfsMaterialDb::parse_color("0FFFFFF") == 0xFFFFFF);
        REQUIRE(CfsMaterialDb::parse_color("0000000") == 0x000000);
    }

    SECTION("sentinel values return default") {
        REQUIRE(CfsMaterialDb::parse_color("-1") == 0x808080);
        REQUIRE(CfsMaterialDb::parse_color("None") == 0x808080);
    }
}

TEST_CASE("CFS slot addressing", "[ams][cfs]") {
    SECTION("global index to TNN name") {
        REQUIRE(CfsMaterialDb::slot_to_tnn(0) == "T1A");
        REQUIRE(CfsMaterialDb::slot_to_tnn(1) == "T1B");
        REQUIRE(CfsMaterialDb::slot_to_tnn(3) == "T1D");
        REQUIRE(CfsMaterialDb::slot_to_tnn(4) == "T2A");
        REQUIRE(CfsMaterialDb::slot_to_tnn(7) == "T2D");
        REQUIRE(CfsMaterialDb::slot_to_tnn(15) == "T4D");
    }

    SECTION("TNN name to global index") {
        REQUIRE(CfsMaterialDb::tnn_to_slot("T1A") == 0);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T1D") == 3);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T2A") == 4);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T4D") == 15);
    }

    SECTION("invalid TNN returns -1") {
        REQUIRE(CfsMaterialDb::tnn_to_slot("invalid") == -1);
        REQUIRE(CfsMaterialDb::tnn_to_slot("T5A") == -1);
    }
}
