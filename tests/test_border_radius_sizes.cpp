// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "border_radius_sizes.h"

using namespace helix;

TEST_CASE("BorderRadiusSizes: size count", "[theme]") {
    REQUIRE(BorderRadiusSizes::count() == 8);
}

TEST_CASE("BorderRadiusSizes: name lookup", "[theme]") {
    REQUIRE(std::string(BorderRadiusSizes::name(0)) == "None");
    REQUIRE(std::string(BorderRadiusSizes::name(3)) == "Soft");
    REQUIRE(std::string(BorderRadiusSizes::name(7)) == "Full");
}

TEST_CASE("BorderRadiusSizes: pixel value at breakpoint", "[theme]") {
    // "Soft" (index 3) at Large breakpoint = 10
    REQUIRE(BorderRadiusSizes::pixels(3, "_large") == 10);
    // "Soft" at Micro = 4
    REQUIRE(BorderRadiusSizes::pixels(3, "_micro") == 4);
    // "None" is always 0
    REQUIRE(BorderRadiusSizes::pixels(0, "_xxlarge") == 0);
    // "Full" is always 9999
    REQUIRE(BorderRadiusSizes::pixels(7, "_tiny") == 9999);
}

TEST_CASE("BorderRadiusSizes: clamp out-of-range index", "[theme]") {
    // Index 99 should clamp to max valid (7)
    REQUIRE(BorderRadiusSizes::pixels(99, "_large") == 9999);
    // Negative index should clamp to 0
    REQUIRE(BorderRadiusSizes::pixels(-1, "_large") == 0);
}

TEST_CASE("BorderRadiusSizes: nearest_size_index migration", "[theme]") {
    // Raw 12 should map to "Soft" (index 3) — exact match at Large breakpoint
    REQUIRE(BorderRadiusSizes::nearest_size_index(12) == 3);
    // Raw 0 maps to "None"
    REQUIRE(BorderRadiusSizes::nearest_size_index(0) == 0);
    // Raw 40 (old max) maps to "Full" (index 7)
    REQUIRE(BorderRadiusSizes::nearest_size_index(40) == 7);
    // Raw 9 is between Subtle(8) and Soft(12) at XXLarge — closer to Subtle
    REQUIRE(BorderRadiusSizes::nearest_size_index(9) == 2);
    // Raw 14 is between Soft(12) and Rounded(16) — closer to Rounded
    REQUIRE(BorderRadiusSizes::nearest_size_index(14) == 4);
    // Raw 1 maps to Minimal (4 at XXLarge, closest to 1)
    REQUIRE(BorderRadiusSizes::nearest_size_index(1) == 1);
}
