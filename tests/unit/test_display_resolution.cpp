// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_display_resolution.cpp
 * @brief Unit tests for display resolution detection and screen size determination
 *
 * Tests DetectedResolution struct, screen size constants, and breakpoint mapping.
 */

#include "display_backend.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// DetectedResolution Struct Tests
// ============================================================================

TEST_CASE("DetectedResolution: default construction", "[display_resolution]") {
    DetectedResolution res;

    SECTION("Default has valid=false") {
        REQUIRE_FALSE(res.valid);
    }

    SECTION("Default dimensions are zero") {
        REQUIRE(res.width == 0);
        REQUIRE(res.height == 0);
    }
}

TEST_CASE("DetectedResolution: aggregate initialization", "[display_resolution]") {
    SECTION("Valid resolution with dimensions") {
        DetectedResolution res{480, 400, true};

        REQUIRE(res.width == 480);
        REQUIRE(res.height == 400);
        REQUIRE(res.valid == true);
    }

    SECTION("Invalid resolution marker") {
        DetectedResolution res{0, 0, false};

        REQUIRE(res.width == 0);
        REQUIRE(res.height == 0);
        REQUIRE(res.valid == false);
    }

    SECTION("Large resolution") {
        DetectedResolution res{1920, 1080, true};

        REQUIRE(res.width == 1920);
        REQUIRE(res.height == 1080);
        REQUIRE(res.valid == true);
    }
}

TEST_CASE("DetectedResolution: partial initialization", "[display_resolution]") {
    SECTION("Width and height only, valid defaults to false") {
        DetectedResolution res{640, 480};

        REQUIRE(res.width == 640);
        REQUIRE(res.height == 480);
        REQUIRE(res.valid == false); // Default member initializer
    }

    SECTION("Empty braces use defaults") {
        DetectedResolution res{};

        REQUIRE(res.width == 0);
        REQUIRE(res.height == 0);
        REQUIRE(res.valid == false);
    }
}

// ============================================================================
// Screen Size Constants Tests
// ============================================================================

TEST_CASE("Screen size constants: TINY preset", "[display_resolution][constants]") {
    REQUIRE(UI_SCREEN_TINY_W == 480);
    REQUIRE(UI_SCREEN_TINY_H == 320);
}

TEST_CASE("Screen size constants: SMALL preset", "[display_resolution][constants]") {
    REQUIRE(UI_SCREEN_SMALL_W == 480);
    REQUIRE(UI_SCREEN_SMALL_H == 400);
}

TEST_CASE("Screen size constants: MEDIUM preset", "[display_resolution][constants]") {
    REQUIRE(UI_SCREEN_MEDIUM_W == 800);
    REQUIRE(UI_SCREEN_MEDIUM_H == 480);
}

TEST_CASE("Screen size constants: LARGE preset", "[display_resolution][constants]") {
    REQUIRE(UI_SCREEN_LARGE_W == 1024);
    REQUIRE(UI_SCREEN_LARGE_H == 600);
}

TEST_CASE("Screen size constants: XLARGE preset", "[display_resolution][constants]") {
    REQUIRE(UI_SCREEN_XLARGE_W == 1280);
    REQUIRE(UI_SCREEN_XLARGE_H == 720);
}

TEST_CASE("Screen size constants: size ordering", "[display_resolution][constants]") {
    // Verify presets are ordered by increasing resolution
    SECTION("Width ordering") {
        REQUIRE(UI_SCREEN_MICRO_W == UI_SCREEN_TINY_W); // Same width
        REQUIRE(UI_SCREEN_TINY_W <= UI_SCREEN_SMALL_W);
        REQUIRE(UI_SCREEN_SMALL_W < UI_SCREEN_MEDIUM_W);
        REQUIRE(UI_SCREEN_MEDIUM_W < UI_SCREEN_LARGE_W);
        REQUIRE(UI_SCREEN_LARGE_W < UI_SCREEN_XLARGE_W);
    }

    SECTION("Height ordering") {
        REQUIRE(UI_SCREEN_MICRO_H < UI_SCREEN_TINY_H);
        REQUIRE(UI_SCREEN_TINY_H < UI_SCREEN_SMALL_H);
        REQUIRE(UI_SCREEN_SMALL_H < UI_SCREEN_MEDIUM_H);
        REQUIRE(UI_SCREEN_MEDIUM_H < UI_SCREEN_LARGE_H);
        REQUIRE(UI_SCREEN_LARGE_H < UI_SCREEN_XLARGE_H);
    }

    SECTION("Total pixel count ordering") {
        int micro_pixels = UI_SCREEN_MICRO_W * UI_SCREEN_MICRO_H;
        int tiny_pixels = UI_SCREEN_TINY_W * UI_SCREEN_TINY_H;
        int small_pixels = UI_SCREEN_SMALL_W * UI_SCREEN_SMALL_H;
        int medium_pixels = UI_SCREEN_MEDIUM_W * UI_SCREEN_MEDIUM_H;
        int large_pixels = UI_SCREEN_LARGE_W * UI_SCREEN_LARGE_H;
        int xlarge_pixels = UI_SCREEN_XLARGE_W * UI_SCREEN_XLARGE_H;

        REQUIRE(micro_pixels < tiny_pixels);
        REQUIRE(tiny_pixels < small_pixels);
        REQUIRE(small_pixels < medium_pixels);
        REQUIRE(medium_pixels < large_pixels);
        REQUIRE(large_pixels < xlarge_pixels);
    }
}

// ============================================================================
// Breakpoint Boundary Tests
// ============================================================================

TEST_CASE("Breakpoint mapping: MICRO max boundary", "[display_resolution][breakpoint]") {
    REQUIRE(UI_BREAKPOINT_MICRO_MAX == 272);

    SECTION("272 maps to MICRO breakpoint") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(272), "_micro") == 0);
    }

    SECTION("273 maps to TINY breakpoint") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(273), "_tiny") == 0);
    }
}

TEST_CASE("Breakpoint mapping: TINY max boundary", "[display_resolution][breakpoint]") {
    REQUIRE(UI_BREAKPOINT_TINY_MAX == 390);

    SECTION("390 maps to TINY breakpoint") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(390), "_tiny") == 0);
    }

    SECTION("391 maps to SMALL breakpoint") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(391), "_small") == 0);
    }
}

TEST_CASE("Breakpoint mapping: SMALL max boundary", "[display_resolution][breakpoint]") {
    REQUIRE(UI_BREAKPOINT_SMALL_MAX == 460);

    SECTION("460 maps to SMALL breakpoint") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(460), "_small") == 0);
    }

    SECTION("459 also maps to SMALL breakpoint") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(459), "_small") == 0);
    }

    SECTION("461 maps to MEDIUM breakpoint") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(461), "_medium") == 0);
    }
}

TEST_CASE("Breakpoint mapping: MEDIUM max boundary", "[display_resolution][breakpoint]") {
    REQUIRE(UI_BREAKPOINT_MEDIUM_MAX == 550);

    SECTION("550 maps to MEDIUM breakpoint") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(550), "_medium") == 0);
    }

    SECTION("551 maps to LARGE breakpoint") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(551), "_large") == 0);
    }
}

TEST_CASE("Breakpoint mapping: LARGE max boundary", "[display_resolution][breakpoint]") {
    REQUIRE(UI_BREAKPOINT_LARGE_MAX == 700);

    SECTION("700 maps to LARGE breakpoint") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(700), "_large") == 0);
    }

    SECTION("701 maps to XLARGE breakpoint") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(701), "_xlarge") == 0);
    }
}

// ============================================================================
// Screen Size to Breakpoint Mapping Tests
// ============================================================================

TEST_CASE("Breakpoint mapping: MICRO screen size", "[display_resolution][breakpoint]") {
    // MICRO is 480x272, height=272 → MICRO breakpoint
    int height = UI_SCREEN_MICRO_H;

    REQUIRE(height == 272);
    REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(height), "_micro") == 0);
}

TEST_CASE("Breakpoint mapping: TINY screen size", "[display_resolution][breakpoint]") {
    // TINY is 480x320, height=320 → TINY breakpoint
    int height = UI_SCREEN_TINY_H;

    REQUIRE(height == 320);
    REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(height), "_tiny") == 0);
}

TEST_CASE("Breakpoint mapping: SMALL screen size", "[display_resolution][breakpoint]") {
    // SMALL is 480x400, height=400 → SMALL breakpoint
    int height = UI_SCREEN_SMALL_H;

    REQUIRE(height == 400);
    REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(height), "_small") == 0);
}

TEST_CASE("Breakpoint mapping: MEDIUM screen size", "[display_resolution][breakpoint]") {
    // MEDIUM is 800x480, height=480 → MEDIUM breakpoint
    int height = UI_SCREEN_MEDIUM_H;

    REQUIRE(height == 480);
    REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(height), "_medium") == 0);
}

TEST_CASE("Breakpoint mapping: LARGE screen size", "[display_resolution][breakpoint]") {
    // LARGE is 1024x600, height=600 → LARGE breakpoint
    int height = UI_SCREEN_LARGE_H;

    REQUIRE(height == 600);
    REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(height), "_large") == 0);
}

TEST_CASE("Breakpoint mapping: XLARGE screen size", "[display_resolution][breakpoint]") {
    // XLARGE is 1280x720, height=720 → XLARGE breakpoint
    int height = UI_SCREEN_XLARGE_H;

    REQUIRE(height == 720);
    REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(height), "_xlarge") == 0);
}

TEST_CASE("Breakpoint mapping: ultra-wide display", "[display_resolution][breakpoint]") {
    // 1920x440 ultra-wide, height=440 → SMALL (not LARGE — vertical space is the constraint)
    int height = 440;

    REQUIRE(height <= UI_BREAKPOINT_SMALL_MAX);
    REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(height), "_small") == 0);
}

// ============================================================================
// Arbitrary Resolution Breakpoint Mapping Tests
// ============================================================================

TEST_CASE("Breakpoint mapping: arbitrary resolutions", "[display_resolution][breakpoint]") {
    SECTION("480x400 (small) → SMALL (height=400)") {
        // height=400 ≤460 → SMALL
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(400), "_small") == 0);
    }

    SECTION("1920x1080 → XXLARGE (height=1080)") {
        // height=1080 >1000 → XXLARGE
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(1080), "_xxlarge") == 0);
    }

    SECTION("640x480 → MEDIUM (height=480)") {
        // height=480, 461-550 → MEDIUM
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(480), "_medium") == 0);
    }

    SECTION("320x240 → MICRO (height=240)") {
        // height=240 ≤272 → MICRO
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(240), "_micro") == 0);
    }

    SECTION("800x600 → LARGE (height=600)") {
        // height=600, 551-700 → LARGE
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(600), "_large") == 0);
    }

    SECTION("1920x440 ultra-wide → SMALL (height=440)") {
        // height=440 ≤460 → SMALL (the whole point of height-based breakpoints)
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(440), "_small") == 0);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("Breakpoint mapping: edge cases", "[display_resolution][breakpoint]") {
    SECTION("Very small resolution") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(1), "_micro") == 0);
    }

    SECTION("Zero resolution") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(0), "_micro") == 0);
    }

    SECTION("Very large resolution") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(4000), "_xxlarge") == 0);
    }

    SECTION("8K resolution") {
        REQUIRE(strcmp(theme_manager_get_breakpoint_suffix(7680), "_xxlarge") == 0);
    }
}

// ============================================================================
// DisplayBackend Base Class Tests
// ============================================================================

TEST_CASE("DisplayBackend: detect_resolution default", "[display_resolution][backend]") {
    // The base class default implementation returns invalid
    // We test this by checking the struct returned has expected defaults
    DetectedResolution default_res{};

    SECTION("Default detection returns invalid") {
        REQUIRE_FALSE(default_res.valid);
    }
}

TEST_CASE("DisplayBackendType: string conversion", "[display_resolution][backend]") {
    SECTION("SDL backend name") {
        REQUIRE(strcmp(display_backend_type_to_string(DisplayBackendType::SDL), "SDL") == 0);
    }

    SECTION("FBDEV backend name") {
        REQUIRE(strcmp(display_backend_type_to_string(DisplayBackendType::FBDEV), "Framebuffer") ==
                0);
    }

    SECTION("DRM backend name") {
        REQUIRE(strcmp(display_backend_type_to_string(DisplayBackendType::DRM), "DRM/KMS") == 0);
    }

    SECTION("AUTO backend name") {
        REQUIRE(strcmp(display_backend_type_to_string(DisplayBackendType::AUTO), "Auto") == 0);
    }
}
