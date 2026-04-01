// SPDX-License-Identifier: GPL-3.0-or-later

#include "z_offset_utils.h"

#include <cstring>

#include "../catch_amalgamated.hpp"

using namespace helix::zoffset;
using helix::ZOffsetCalibrationStrategy;

// ============================================================================
// format_delta tests
// ============================================================================

TEST_CASE("format_delta: zero microns produces empty string", "[zoffset][format]") {
    char buf[32] = "garbage";
    format_delta(0, buf, sizeof(buf));
    REQUIRE(buf[0] == '\0');
}

TEST_CASE("format_delta: positive microns formats with plus sign", "[zoffset][format]") {
    char buf[32] = {};
    format_delta(50, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "+0.050mm");
}

TEST_CASE("format_delta: negative microns formats with minus sign", "[zoffset][format]") {
    char buf[32] = {};
    format_delta(-25, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "-0.025mm");
}

TEST_CASE("format_delta: large positive value", "[zoffset][format]") {
    char buf[32] = {};
    format_delta(1500, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "+1.500mm");
}

// ============================================================================
// format_offset tests
// ============================================================================

TEST_CASE("format_offset: zero microns shows +0.000mm", "[zoffset][format]") {
    char buf[32] = {};
    format_offset(0, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "+0.000mm");
}

TEST_CASE("format_offset: positive microns", "[zoffset][format]") {
    char buf[32] = {};
    format_offset(100, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "+0.100mm");
}

TEST_CASE("format_offset: negative microns", "[zoffset][format]") {
    char buf[32] = {};
    format_offset(-250, buf, sizeof(buf));
    REQUIRE(std::string(buf) == "-0.250mm");
}

// ============================================================================
// is_auto_saved tests
// ============================================================================

TEST_CASE("is_auto_saved: FIRMWARE_MANAGED returns true", "[zoffset][strategy]") {
    REQUIRE(is_auto_saved(ZOffsetCalibrationStrategy::FIRMWARE_MANAGED) == true);
}

TEST_CASE("is_auto_saved: PROBE_CALIBRATE returns false", "[zoffset][strategy]") {
    REQUIRE(is_auto_saved(ZOffsetCalibrationStrategy::PROBE_CALIBRATE) == false);
}

TEST_CASE("is_auto_saved: ENDSTOP returns false", "[zoffset][strategy]") {
    REQUIRE(is_auto_saved(ZOffsetCalibrationStrategy::ENDSTOP) == false);
}
