// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_exclude_object_map_view.cpp
 * @brief Tests for the exclude-object overhead map view feature
 *
 * Task 1: Verifies that object color palette tokens (object_color_1 through
 * object_color_8) are registered and return non-black colors after theme init.
 *
 * Uses XMLTestFixture because theme_manager_init() must have been called for
 * lv_xml_register_const tokens to be accessible via theme_manager_get_color().
 */

#include "theme_manager.h"

#include "../catch_amalgamated.hpp"
#include "../test_fixtures.h"

// ============================================================================
// Object color palette token tests
// ============================================================================

TEST_CASE_METHOD(XMLTestFixture, "object_color_1 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_1");
    // #7c8aff — periwinkle blue: red=0x7c, non-black
    REQUIRE(color.red != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_2 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_2");
    // #4ecdc4 — teal: green=0xcd, non-black
    REQUIRE(color.green != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_3 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_3");
    // #f9c74f — golden yellow: red=0xf9, non-black
    REQUIRE(color.red != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_4 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_4");
    // #a78bfa — soft purple: red=0xa7, non-black
    REQUIRE(color.red != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_5 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_5");
    // #f472b6 — pink: red=0xf4, non-black
    REQUIRE(color.red != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_6 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_6");
    // #fb923c — orange: red=0xfb, non-black
    REQUIRE(color.red != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_7 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_7");
    // #34d399 — emerald: green=0xd3, non-black
    REQUIRE(color.green != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "object_color_8 token returns non-black color",
                 "[exclude_map][tokens]") {
    lv_color_t color = theme_manager_get_color("object_color_8");
    // #60a5fa — sky blue: blue=0xfa, non-black
    REQUIRE(color.blue != 0);
}

TEST_CASE_METHOD(XMLTestFixture, "all 8 object color tokens are registered",
                 "[exclude_map][tokens]") {
    // Verify all 8 tokens return non-zero colors (not black fallback)
    lv_color_t black = lv_color_hex(0x000000);

    for (int i = 1; i <= 8; ++i) {
        char token[32];
        snprintf(token, sizeof(token), "object_color_%d", i);
        lv_color_t color = theme_manager_get_color(token);

        // At least one channel must be non-zero to distinguish from black fallback
        bool is_non_black = (color.red != black.red) || (color.green != black.green) ||
                            (color.blue != black.blue);
        INFO("Token " << token << " returned black (missing registration)");
        REQUIRE(is_non_black);
    }
}
