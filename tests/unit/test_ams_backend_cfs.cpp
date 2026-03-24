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
