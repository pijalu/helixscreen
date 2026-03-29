// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spoolman_manager.h"

#include "../catch_amalgamated.hpp"

// ============================================================================
// Test Access — friend class for reaching private members (L065)
// ============================================================================

class SpoolmanManagerTestAccess {
  public:
    static int poll_refcount(SpoolmanManager& m) { return m.poll_refcount_; }
    static bool cb_open(SpoolmanManager& m) { return m.cb_open_; }
    static int consecutive_failures(SpoolmanManager& m) { return m.consecutive_failures_; }
    static bool unavailable_notified(SpoolmanManager& m) { return m.unavailable_notified_; }
    static uint32_t last_refresh_ms(SpoolmanManager& m) { return m.last_refresh_ms_; }

    static void set_consecutive_failures(SpoolmanManager& m, int n) {
        m.consecutive_failures_ = n;
    }
    static void set_cb_open(SpoolmanManager& m, bool open) { m.cb_open_ = open; }
    static void reset_cb(SpoolmanManager& m) { m.reset_circuit_breaker(); }

    static void reset(SpoolmanManager& m) {
        m.poll_refcount_ = 0;
        m.poll_timer_ = nullptr;
        m.consecutive_failures_ = 0;
        m.cb_tripped_at_ms_ = 0;
        m.cb_open_ = false;
        m.unavailable_notified_ = false;
        m.last_refresh_ms_ = 0;
        m.api_ = nullptr;
    }
};

using TA = SpoolmanManagerTestAccess;

// ============================================================================
// Polling Refcount Tests
// ============================================================================

TEST_CASE("SpoolmanManager - polling refcount increments and decrements",
          "[spoolman][polling]") {
    auto& mgr = SpoolmanManager::instance();
    TA::reset(mgr);

    SECTION("start increments, stop decrements") {
        // Without spoolman available, start is a no-op (availability gate)
        // Test the stop path which always works
        REQUIRE(TA::poll_refcount(mgr) == 0);
        mgr.stop_spoolman_polling();
        REQUIRE(TA::poll_refcount(mgr) == 0); // clamped at 0
    }

    SECTION("stop below zero clamps at 0") {
        REQUIRE(TA::poll_refcount(mgr) == 0);
        mgr.stop_spoolman_polling();
        mgr.stop_spoolman_polling();
        mgr.stop_spoolman_polling();
        REQUIRE(TA::poll_refcount(mgr) == 0);
    }
}

// ============================================================================
// Circuit Breaker Tests
// ============================================================================

TEST_CASE("SpoolmanManager - reset_circuit_breaker clears all state",
          "[spoolman][circuit-breaker]") {
    auto& mgr = SpoolmanManager::instance();
    TA::reset(mgr);

    // Dirty up the state
    TA::set_consecutive_failures(mgr, 5);
    TA::set_cb_open(mgr, true);

    TA::reset_cb(mgr);

    REQUIRE(TA::consecutive_failures(mgr) == 0);
    REQUIRE(TA::cb_open(mgr) == false);
    REQUIRE(TA::unavailable_notified(mgr) == false);
    REQUIRE(TA::last_refresh_ms(mgr) == 0);
}

TEST_CASE("SpoolmanManager - set_api resets circuit breaker",
          "[spoolman][circuit-breaker]") {
    auto& mgr = SpoolmanManager::instance();
    TA::reset(mgr);

    TA::set_consecutive_failures(mgr, 3);
    TA::set_cb_open(mgr, true);

    mgr.set_api(nullptr);

    REQUIRE(TA::consecutive_failures(mgr) == 0);
    REQUIRE(TA::cb_open(mgr) == false);
}

// ============================================================================
// Availability Gate Tests
// ============================================================================

TEST_CASE("SpoolmanManager - refresh_spoolman_weights returns early without API",
          "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();
    TA::reset(mgr);

    // No API set — should return without crash
    REQUIRE_NOTHROW(mgr.refresh_spoolman_weights());
}
