// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "first_run_tour.h"

#include "../catch_amalgamated.hpp"

using helix::Config;
using helix::tour::FirstRunTour;

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
