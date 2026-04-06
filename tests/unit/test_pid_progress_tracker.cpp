// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/pid_progress_tracker.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("PidProgressTracker heating phase detection", "[pid_progress]") {
    PidProgressTracker tracker;
    tracker.start(PidProgressTracker::Heater::EXTRUDER, 200, 25.0f);

    REQUIRE(tracker.phase() == PidProgressTracker::Phase::HEATING);

    // Simulate temperature climbing
    tracker.on_temperature(50.0f, 1000);
    REQUIRE(tracker.phase() == PidProgressTracker::Phase::HEATING);
    REQUIRE(tracker.progress_percent() > 0);
    REQUIRE(tracker.progress_percent() < 40);

    // Overshoot past target
    tracker.on_temperature(210.0f, 60000);
    REQUIRE(tracker.phase() == PidProgressTracker::Phase::HEATING);
    // Still heating — haven't crossed back down yet

    // Cross back below target — oscillation begins
    tracker.on_temperature(195.0f, 75000);
    REQUIRE(tracker.phase() == PidProgressTracker::Phase::OSCILLATING);
    REQUIRE(tracker.progress_percent() >= 40);
}

TEST_CASE("PidProgressTracker oscillation counting", "[pid_progress]") {
    PidProgressTracker tracker;
    tracker.start(PidProgressTracker::Heater::EXTRUDER, 200, 25.0f);

    // Fast-forward through heating phase
    tracker.on_temperature(210.0f, 30000); // overshoot
    tracker.on_temperature(195.0f, 45000); // first downward crossing -> oscillating

    REQUIRE(tracker.oscillation_count() == 1);

    // Simulate 4 more oscillation cycles (up-down pairs)
    uint32_t t = 45000;
    for (int i = 0; i < 4; i++) {
        t += 15000;
        tracker.on_temperature(205.0f, t); // above target
        t += 15000;
        tracker.on_temperature(195.0f, t); // below target — downward crossing
    }

    REQUIRE(tracker.oscillation_count() == 5);
    // After 5 cycles, progress should be at or near 95%
    REQUIRE(tracker.progress_percent() >= 93);
    REQUIRE(tracker.progress_percent() <= 95);
}

TEST_CASE("PidProgressTracker heating ETA calculation", "[pid_progress]") {
    PidProgressTracker tracker;
    tracker.start(PidProgressTracker::Heater::EXTRUDER, 200, 25.0f);

    // After 10 seconds, temp went from 25 to 50 (1 deg/sec)
    tracker.on_temperature(50.0f, 10000);

    // Remaining: 150 degrees at 1 deg/sec = 150s heating + default oscillation time
    auto eta = tracker.eta_seconds();
    REQUIRE(eta.has_value());
    // Heating remaining should be ~150s, plus oscillation estimate
    REQUIRE(*eta > 140);
    REQUIRE(*eta < 300); // heating + oscillation
}

TEST_CASE("PidProgressTracker oscillation ETA", "[pid_progress]") {
    PidProgressTracker tracker;
    tracker.start(PidProgressTracker::Heater::EXTRUDER, 200, 25.0f);

    // Enter oscillation phase
    tracker.on_temperature(210.0f, 30000);
    tracker.on_temperature(195.0f, 45000);

    // Complete one full cycle (30 seconds per cycle)
    tracker.on_temperature(205.0f, 60000);
    tracker.on_temperature(195.0f, 75000); // cycle 2 complete at 75s

    // Measured cycle period: 30 seconds (75000 - 45000) / 1 completed cycle after first
    // Remaining: 3 cycles * 30s = 90s
    auto eta = tracker.eta_seconds();
    REQUIRE(eta.has_value());
    REQUIRE(*eta >= 80);
    REQUIRE(*eta <= 100);
}

TEST_CASE("PidProgressTracker with history provides immediate ETA", "[pid_progress]") {
    PidProgressTracker tracker;
    tracker.set_history(2.0f, 300.0f); // 2 s/°C, 300s oscillation
    tracker.start(PidProgressTracker::Heater::BED, 100, 25.0f);

    // Even before any temperature data, we should have an ETA
    // from history: 75°C * 2 s/°C + 300s = 450s
    auto eta = tracker.eta_seconds();
    REQUIRE(eta.has_value());
    REQUIRE(*eta >= 440);
    REQUIRE(*eta <= 460);
}

TEST_CASE("PidProgressTracker default ETA for first-ever calibration", "[pid_progress]") {
    PidProgressTracker tracker;
    // No history set — uses smart defaults
    tracker.start(PidProgressTracker::Heater::EXTRUDER, 200, 25.0f);

    // Feed one temperature reading to establish timing
    tracker.on_temperature(26.0f, 1000);

    auto eta = tracker.eta_seconds();
    REQUIRE(eta.has_value());
    // Default extruder: 0.5 s/°C * 174°C + 120s = ~207s
    REQUIRE(*eta > 150);
    REQUIRE(*eta < 300);
}

TEST_CASE("PidProgressTracker progress never exceeds 95 before completion", "[pid_progress]") {
    PidProgressTracker tracker;
    tracker.start(PidProgressTracker::Heater::EXTRUDER, 200, 25.0f);

    // Enter oscillation
    tracker.on_temperature(210.0f, 30000);
    tracker.on_temperature(195.0f, 45000);

    // Do 6 oscillations (more than expected 5)
    uint32_t t = 45000;
    for (int i = 0; i < 6; i++) {
        t += 15000;
        tracker.on_temperature(205.0f, t);
        t += 15000;
        tracker.on_temperature(195.0f, t);
    }

    // Still capped at 95 until mark_complete
    REQUIRE(tracker.progress_percent() == 95);

    tracker.mark_complete();
    REQUIRE(tracker.progress_percent() == 100);
}

TEST_CASE("PidProgressTracker measured values for persistence", "[pid_progress]") {
    PidProgressTracker tracker;
    tracker.start(PidProgressTracker::Heater::BED, 100, 25.0f);

    // Simulate heating: establish start tick, then 75 degrees in 150 seconds = 2.0 s/°C
    tracker.on_temperature(25.0f, 1000);    // establishes start_tick_ = 1000
    tracker.on_temperature(100.0f, 151000); // 75°C in 150s → 2.0 s/°C
    REQUIRE(tracker.measured_heat_rate() > 1.9f);
    REQUIRE(tracker.measured_heat_rate() < 2.1f);

    // Enter oscillation phase
    tracker.on_temperature(105.0f, 160000); // overshoot
    tracker.on_temperature(95.0f, 180000);  // first downward crossing

    // 4 more cycles, 60 seconds each
    uint32_t t = 180000;
    for (int i = 0; i < 4; i++) {
        t += 30000;
        tracker.on_temperature(105.0f, t);
        t += 30000;
        tracker.on_temperature(95.0f, t);
    }

    // 5 oscillations total, 4 measured intervals of 60s each = 240s total
    float osc_duration = tracker.measured_oscillation_duration();
    REQUIRE(osc_duration > 230.0f);
    REQUIRE(osc_duration < 250.0f);
}
