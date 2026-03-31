// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_types.h"

#include "../catch_amalgamated.hpp"

using namespace helix::printer;

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
