// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("find_widget_def resolves multi-instance IDs", "[panel_widget][multi_instance]") {
    SECTION("Exact match still works for single-instance") {
        REQUIRE(find_widget_def("power") != nullptr);
        REQUIRE(std::string(find_widget_def("power")->id) == "power");
    }

    SECTION("Colon on non-multi_instance def returns nullptr") {
        // "power" exists but is not multi_instance, so "power:1" should fail
        auto* power_def = find_widget_def("power");
        REQUIRE(power_def != nullptr);
        REQUIRE(power_def->multi_instance == false);

        REQUIRE(find_widget_def("power:1") == nullptr);
        REQUIRE(find_widget_def("power:42") == nullptr);
    }

    SECTION("Non-existent base returns nullptr") {
        REQUIRE(find_widget_def("nonexistent:1") == nullptr);
        REQUIRE(find_widget_def("nonexistent") == nullptr);
    }

    SECTION("Plain colon without digits returns nullptr for non-multi_instance") {
        REQUIRE(find_widget_def("power:") == nullptr);
        REQUIRE(find_widget_def("power:abc") == nullptr);
    }
}
