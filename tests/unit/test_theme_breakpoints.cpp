// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_theme_breakpoints.cpp
 * @brief Unit tests for breakpoint suffix selection and responsive token fallback
 *
 * Tests the 6-tier breakpoint system: MICRO (≤272), TINY (≤390), SMALL (391-460),
 * MEDIUM (461-550), LARGE (551-700), XLARGE (>700) and the _micro→_tiny→_small /
 * _tiny→_small / _xlarge→_large fallback behavior.
 */

#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// Breakpoint suffix selection
// ============================================================================

TEST_CASE("Breakpoint suffix returns _micro for heights ≤272", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(272)) == "_micro");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(200)) == "_micro");
}

TEST_CASE("Breakpoint suffix returns _tiny for heights 273-390", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(273)) == "_tiny");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(320)) == "_tiny");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(390)) == "_tiny");
}

TEST_CASE("Breakpoint suffix returns _small for heights 391-460", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(391)) == "_small");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(400)) == "_small");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(460)) == "_small");
}

TEST_CASE("Breakpoint suffix returns _medium for heights 461-550", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(461)) == "_medium");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(480)) == "_medium");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(550)) == "_medium");
}

TEST_CASE("Breakpoint suffix returns _large for heights 551-700", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(551)) == "_large");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(600)) == "_large");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(700)) == "_large");
}

TEST_CASE("Breakpoint suffix returns _xlarge for heights >700", "[theme][breakpoints]") {
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(701)) == "_xlarge");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(720)) == "_xlarge");
    REQUIRE(std::string(theme_manager_get_breakpoint_suffix(1080)) == "_xlarge");
}

TEST_CASE("Breakpoint constants have correct values", "[theme][breakpoints]") {
    REQUIRE(UI_BREAKPOINT_MICRO_MAX == 272);
    REQUIRE(UI_BREAKPOINT_TINY_MAX == 390);
    REQUIRE(UI_BREAKPOINT_SMALL_MAX == 460);
    REQUIRE(UI_BREAKPOINT_MEDIUM_MAX == 550);
    REQUIRE(UI_BREAKPOINT_LARGE_MAX == 700);
}

TEST_CASE("Breakpoint index enum has correct values", "[theme][breakpoints]") {
    REQUIRE(to_int(UiBreakpoint::Micro) == 0);
    REQUIRE(to_int(UiBreakpoint::Tiny) == 1);
    REQUIRE(to_int(UiBreakpoint::Small) == 2);
    REQUIRE(to_int(UiBreakpoint::Medium) == 3);
    REQUIRE(to_int(UiBreakpoint::Large) == 4);
    REQUIRE(to_int(UiBreakpoint::XLarge) == 5);
}

// Backward compatibility: legacy UI_BP_* macros still work
TEST_CASE("Legacy UI_BP_* macros match UiBreakpoint enum values", "[theme][breakpoints]") {
    REQUIRE(UI_BP_MICRO == to_int(UiBreakpoint::Micro));
    REQUIRE(UI_BP_TINY == to_int(UiBreakpoint::Tiny));
    REQUIRE(UI_BP_SMALL == to_int(UiBreakpoint::Small));
    REQUIRE(UI_BP_MEDIUM == to_int(UiBreakpoint::Medium));
    REQUIRE(UI_BP_LARGE == to_int(UiBreakpoint::Large));
    REQUIRE(UI_BP_XLARGE == to_int(UiBreakpoint::XLarge));
}

// ============================================================================
// Responsive token fallback behavior (XML-based, uses test fixtures)
// ============================================================================

TEST_CASE("Responsive token discovery includes _tiny suffix", "[theme][breakpoints]") {
    // Verify that _tiny tokens are discoverable from XML
    auto tiny_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_tiny");

    // fan_card_base_width_tiny and fan_card_height_tiny defined in fan_dial.xml
    REQUIRE(tiny_tokens.count("fan_card_base_width") > 0);
    REQUIRE(tiny_tokens.count("fan_card_height") > 0);
}

TEST_CASE("Tokens without _tiny variant still have _small available", "[theme][breakpoints]") {
    // space_2xl has _small/_medium/_large but no _tiny — verify _small exists for fallback
    auto small_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_small");
    REQUIRE(small_tokens.count("space_2xl") > 0);

    auto tiny_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_tiny");
    REQUIRE(tiny_tokens.count("space_2xl") == 0);
}

TEST_CASE("Validation does not require _tiny for complete sets", "[theme][breakpoints]") {
    // _tiny is optional — validation should not warn about missing _tiny
    auto warnings = theme_manager_validate_constant_sets("ui_xml");

    for (const auto& warning : warnings) {
        // No warning should complain about missing _tiny
        REQUIRE(warning.find("_tiny") == std::string::npos);
    }
}

TEST_CASE("Validation does not require _xlarge for complete sets", "[theme][breakpoints]") {
    // _xlarge is optional — validation should not warn about missing _xlarge
    auto warnings = theme_manager_validate_constant_sets("ui_xml");

    for (const auto& warning : warnings) {
        // No warning should complain about missing _xlarge
        REQUIRE(warning.find("_xlarge") == std::string::npos);
    }
}

// ============================================================================
// _micro → _tiny → _small fallback chain
// ============================================================================

TEST_CASE("Token parsing discovers _micro tokens", "[theme][breakpoints]") {
    // Verify that _micro tokens are discoverable from XML
    auto micro_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_micro");

    // button_height_micro, header_height_micro etc. defined in globals.xml
    REQUIRE(micro_tokens.count("button_height") > 0);
    REQUIRE(micro_tokens.count("header_height") > 0);
}

TEST_CASE("Fallback chain: _micro falls back to _tiny then _small", "[theme][breakpoints]") {
    // button_height has _micro, _tiny, _small, _medium, _large — verify all exist
    auto micro_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_micro");
    auto tiny_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_tiny");
    auto small_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_small");

    // button_height should exist in all three
    REQUIRE(small_tokens.count("button_height") > 0);
    REQUIRE(tiny_tokens.count("button_height") > 0);
    REQUIRE(micro_tokens.count("button_height") > 0);

    // Verify the fallback chain values differ (otherwise fallback test is meaningless)
    REQUIRE(micro_tokens.at("button_height") != tiny_tokens.at("button_height"));
    REQUIRE(tiny_tokens.at("button_height") != small_tokens.at("button_height"));
}

TEST_CASE("Fallback chain: _micro uses _small when neither _micro nor _tiny exist",
          "[theme][breakpoints]") {
    // space_2xl has _small/_medium/_large but no _micro or _tiny
    auto micro_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_micro");
    auto tiny_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_tiny");
    auto small_tokens = theme_manager_parse_all_xml_for_suffix("ui_xml", "px", "_small");

    REQUIRE(small_tokens.count("space_2xl") > 0);
    REQUIRE(tiny_tokens.count("space_2xl") == 0);
    REQUIRE(micro_tokens.count("space_2xl") == 0);
    // Fallback: _micro → _tiny → _small means space_2xl would use _small value on MICRO
}

TEST_CASE("Validation does not require _micro for complete sets", "[theme][breakpoints]") {
    // _micro is optional — validation should not warn about missing _micro
    auto warnings = theme_manager_validate_constant_sets("ui_xml");

    for (const auto& warning : warnings) {
        // No warning should complain about missing _micro
        REQUIRE(warning.find("_micro") == std::string::npos);
    }
}
