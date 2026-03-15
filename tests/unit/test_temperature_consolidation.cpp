// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_temperature_utils.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui::temperature;

// ============================================================================
// heater_display() - Off state
// ============================================================================

TEST_CASE("heater_display: off state when target is 0", "[temperature][heater_display]") {
    auto result = heater_display(250, 0); // 25°C, off
    REQUIRE(result.temp == "25°C");
    REQUIRE(result.status == "Off");
    REQUIRE(result.pct == 0);
}

TEST_CASE("heater_display: off state when target is negative", "[temperature][heater_display]") {
    auto result = heater_display(250, -10); // 25°C, negative target
    REQUIRE(result.status == "Off");
    REQUIRE(result.pct == 0);
}

// ============================================================================
// heater_display() - Heating state
// ============================================================================

TEST_CASE("heater_display: heating state", "[temperature][heater_display]") {
    // 150°C current, 200°C target -> 75%
    auto result = heater_display(1500, 2000);
    REQUIRE(result.temp == "150 / 200°C");
    REQUIRE(result.status == "Heating...");
    REQUIRE(result.pct == 75);
}

TEST_CASE("heater_display: heating from zero", "[temperature][heater_display]") {
    auto result = heater_display(0, 2000);
    REQUIRE(result.temp == "0 / 200°C");
    REQUIRE(result.pct == 0);
    REQUIRE(result.status == "Heating...");
}

// ============================================================================
// heater_display() - Ready state (within tolerance)
// ============================================================================

TEST_CASE("heater_display: ready state within tolerance", "[temperature][heater_display]") {
    // 198°C with 200°C target -> within +/-2 -> Ready
    auto result = heater_display(1980, 2000);
    REQUIRE(result.temp == "198 / 200°C");
    REQUIRE(result.status == "Ready");
    REQUIRE(result.pct == 99);
}

TEST_CASE("heater_display: ready state at exact target", "[temperature][heater_display]") {
    auto result = heater_display(2000, 2000);
    REQUIRE(result.status == "Ready");
    REQUIRE(result.pct == 100);
}

// ============================================================================
// heater_display() - Cooling state
// ============================================================================

TEST_CASE("heater_display: cooling state above tolerance", "[temperature][heater_display]") {
    // 210°C with 200°C target -> 210 > 202 -> Cooling
    auto result = heater_display(2100, 2000);
    REQUIRE(result.status == "Cooling");
    REQUIRE(result.pct == 100);
}

// ============================================================================
// heater_display() - Tolerance boundaries
// ============================================================================

TEST_CASE("heater_display: exactly at lower tolerance boundary is Ready",
          "[temperature][heater_display]") {
    // 198°C with 200°C target -> 198 >= 200-2 -> Ready
    auto result = heater_display(1980, 2000);
    REQUIRE(result.status == "Ready");
}

TEST_CASE("heater_display: exactly at upper tolerance boundary is Ready",
          "[temperature][heater_display]") {
    // 202°C with 200°C target -> 202 <= 200+2 -> Ready
    auto result = heater_display(2020, 2000);
    REQUIRE(result.status == "Ready");
}

TEST_CASE("heater_display: just below lower tolerance boundary is Heating",
          "[temperature][heater_display]") {
    // 197°C with 200°C target -> 197 < 198 -> Heating
    auto result = heater_display(1970, 2000);
    REQUIRE(result.status == "Heating...");
}

TEST_CASE("heater_display: just above upper tolerance boundary is Cooling",
          "[temperature][heater_display]") {
    // 203°C with 200°C target -> 203 > 202 -> Cooling
    auto result = heater_display(2030, 2000);
    REQUIRE(result.status == "Cooling");
}

// ============================================================================
// heater_display() - Percentage clamping
// ============================================================================

TEST_CASE("heater_display: percentage clamps to 100 when over target",
          "[temperature][heater_display]") {
    auto result = heater_display(2500, 2000);
    REQUIRE(result.pct == 100);
}

TEST_CASE("heater_display: percentage clamps to 0 for negative temps",
          "[temperature][heater_display]") {
    auto result = heater_display(-10, 2000);
    REQUIRE(result.pct == 0);
}

// ============================================================================
// heater_display() - Color field matches get_heating_state_color()
// ============================================================================

TEST_CASE("heater_display: color matches get_heating_state_color for off state",
          "[temperature][heater_display]") {
    auto result = heater_display(250, 0);
    auto expected_color = get_heating_state_color(25, 0);
    REQUIRE(result.color.red == expected_color.red);
    REQUIRE(result.color.green == expected_color.green);
    REQUIRE(result.color.blue == expected_color.blue);
}

TEST_CASE("heater_display: color matches get_heating_state_color for heating state",
          "[temperature][heater_display]") {
    auto result = heater_display(1500, 2000);
    auto expected_color = get_heating_state_color(150, 200);
    REQUIRE(result.color.red == expected_color.red);
    REQUIRE(result.color.green == expected_color.green);
    REQUIRE(result.color.blue == expected_color.blue);
}

TEST_CASE("heater_display: color matches get_heating_state_color for ready state",
          "[temperature][heater_display]") {
    auto result = heater_display(1990, 2000);
    auto expected_color = get_heating_state_color(199, 200);
    REQUIRE(result.color.red == expected_color.red);
    REQUIRE(result.color.green == expected_color.green);
    REQUIRE(result.color.blue == expected_color.blue);
}

TEST_CASE("heater_display: color matches get_heating_state_color for cooling state",
          "[temperature][heater_display]") {
    auto result = heater_display(2100, 2000);
    auto expected_color = get_heating_state_color(210, 200);
    REQUIRE(result.color.red == expected_color.red);
    REQUIRE(result.color.green == expected_color.green);
    REQUIRE(result.color.blue == expected_color.blue);
}
