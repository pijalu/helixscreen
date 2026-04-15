// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "first_run_tour.h"
#include "tour_steps.h"

#include "../catch_amalgamated.hpp"

using helix::Config;
using helix::tour::build_tour_steps;
using helix::tour::FirstRunTour;
using helix::tour::TourStep;

namespace {
void reset_tour_settings() {
    auto* cfg = Config::get_instance();
    cfg->set<bool>("/tour/completed", false);
    cfg->set<int>("/tour/last_seen_version", 0);
    // is_wizard_required() checks per-printer first, then falls back to root-level.
    // In the singleton-without-init test environment, active_printer_id_ is empty,
    // so the root-level key is the one that's read.
    cfg->set<bool>("/wizard_completed", true);
}
} // namespace

TEST_CASE("FirstRunTour gate: blocks when tour already completed", "[tour]") {
    reset_tour_settings();
    Config::get_instance()->set<bool>("/tour/completed", true);
    Config::get_instance()->set<int>("/tour/last_seen_version",
                                     helix::tour::kTourVersion);
    REQUIRE(FirstRunTour::should_auto_start() == false);
}

TEST_CASE("FirstRunTour gate: blocks when wizard not complete", "[tour]") {
    reset_tour_settings();
    Config::get_instance()->set<bool>("/wizard_completed", false);
    REQUIRE(FirstRunTour::should_auto_start() == false);
}

TEST_CASE("FirstRunTour gate: allows auto-start when fresh and wizards done", "[tour]") {
    reset_tour_settings();
    REQUIRE(FirstRunTour::should_auto_start() == true);
}

TEST_CASE("FirstRunTour gate: re-triggers when last_seen_version is behind kTourVersion",
          "[tour]") {
    reset_tour_settings();
    Config::get_instance()->set<bool>("/tour/completed", true);
    Config::get_instance()->set<int>("/tour/last_seen_version", 0);
    // kTourVersion is 1; last_seen=0 < 1, so tour should re-trigger.
    REQUIRE(FirstRunTour::should_auto_start() == true);
}

TEST_CASE("FirstRunTour mark_completed writes both flags", "[tour]") {
    reset_tour_settings();
    FirstRunTour::mark_completed();
    auto* cfg = Config::get_instance();
    REQUIRE(cfg->get<bool>("/tour/completed", false) == true);
    REQUIRE(cfg->get<int>("/tour/last_seen_version", 0) == helix::tour::kTourVersion);
}

TEST_CASE("Tour steps: always 8 steps regardless of AMS", "[tour]") {
    auto steps_no_ams = build_tour_steps(/*has_ams=*/false);
    auto steps_with_ams = build_tour_steps(/*has_ams=*/true);
    REQUIRE(steps_no_ams.size() == 8);
    REQUIRE(steps_with_ams.size() == 8);
}

TEST_CASE("Tour steps: step 2 sub-spotlights AMS only when present", "[tour]") {
    auto with_ams = build_tour_steps(true);
    auto without = build_tour_steps(false);
    REQUIRE(with_ams[1].sub_spotlights.size() == 3);     // nozzle + fan + ams
    REQUIRE(without[1].sub_spotlights.size() == 2);      // nozzle + fan
}

TEST_CASE("Tour steps: navbar steps 4-8 target nav buttons", "[tour]") {
    auto steps = build_tour_steps(true);
    REQUIRE(steps[3].target_name == "nav_btn_print_select");
    REQUIRE(steps[4].target_name == "nav_btn_controls");
    REQUIRE(steps[5].target_name == "nav_btn_filament");
    REQUIRE(steps[6].target_name == "nav_btn_advanced");
    REQUIRE(steps[7].target_name == "nav_btn_settings");
}

TEST_CASE("Tour steps: welcome and customize steps have correct targets", "[tour]") {
    auto steps = build_tour_steps(true);
    REQUIRE(steps[0].target_name.empty());              // Welcome is centered
    REQUIRE(steps[2].target_name == "carousel_host");   // Long-press step
}
