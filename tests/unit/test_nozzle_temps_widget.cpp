// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_nozzle_temps_widget.cpp
 * @brief Tests for NozzleTempsWidget registration and metadata
 *
 * Verifies that the nozzle_temps widget is registered correctly in the
 * panel widget registry with expected metadata and hardware gating.
 */

#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

#include <cstring>

using namespace helix;

TEST_CASE("NozzleTempsWidget: widget def exists in registry", "[nozzle_temps][panel_widget]") {
    const auto* def = find_widget_def("nozzle_temps");
    REQUIRE(def != nullptr);

    SECTION("has correct id") {
        REQUIRE(std::strcmp(def->id, "nozzle_temps") == 0);
    }

    SECTION("has display name") {
        REQUIRE(def->display_name != nullptr);
        REQUIRE(std::strlen(def->display_name) > 0);
    }

    SECTION("has icon") {
        REQUIRE(def->icon != nullptr);
        REQUIRE(std::strcmp(def->icon, "thermometer") == 0);
    }

    SECTION("has description") {
        REQUIRE(def->description != nullptr);
        REQUIRE(std::strlen(def->description) > 0);
    }
}

TEST_CASE("NozzleTempsWidget: no hardware gate (visible to all printers)",
          "[nozzle_temps][panel_widget]") {
    const auto* def = find_widget_def("nozzle_temps");
    REQUIRE(def != nullptr);
    REQUIRE(def->hardware_gate_subject == nullptr);
}

TEST_CASE("NozzleTempsWidget: default rowspan is 2 (1x2 preferred)", "[nozzle_temps][panel_widget]") {
    const auto* def = find_widget_def("nozzle_temps");
    REQUIRE(def != nullptr);

    REQUIRE(def->colspan == 1);
    REQUIRE(def->rowspan == 2);

    // Can scale from 1x2 to 2x3 (min_rowspan=2 to prevent unusable 1x1)
    REQUIRE(def->effective_min_colspan() == 1);
    REQUIRE(def->effective_min_rowspan() == 2);
    REQUIRE(def->effective_max_colspan() == 2);
    REQUIRE(def->effective_max_rowspan() == 3);
}

TEST_CASE("NozzleTempsWidget: not enabled by default (requires multi-tool)",
          "[nozzle_temps][panel_widget]") {
    const auto* def = find_widget_def("nozzle_temps");
    REQUIRE(def != nullptr);
    REQUIRE_FALSE(def->default_enabled);
}
