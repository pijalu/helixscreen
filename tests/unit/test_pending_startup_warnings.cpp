// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_pending_startup_warnings.cpp
 * @brief Unit tests for the deferred startup-warnings queue.
 *
 * Validates:
 * 1. Empty queue starts empty and drain is a no-op
 * 2. Enqueue preserves order; drain returns entries in FIFO order
 * 3. clear() resets state
 * 4. Thread-safety: concurrent enqueue from multiple threads does not crash
 *    or lose entries (run under TSAN where available)
 */

#include "../../include/pending_startup_warnings.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
// Test helper: capture drain callbacks into a local vector (instead of calling
// ToastManager, which is not available in a unit test context).
struct CapturedWarning {
    PendingStartupWarnings::Severity severity;
    std::string message;
};
} // namespace

TEST_CASE("PendingStartupWarnings: empty queue drains to empty", "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    std::vector<CapturedWarning> captured;
    q.drain([&](PendingStartupWarnings::Severity s, const std::string& m) {
        captured.push_back({s, m});
    });
    REQUIRE(captured.empty());
}

TEST_CASE("PendingStartupWarnings: enqueue preserves FIFO order", "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    q.enqueue(PendingStartupWarnings::Severity::WARNING, "first");
    q.enqueue(PendingStartupWarnings::Severity::ERROR, "second");
    q.enqueue(PendingStartupWarnings::Severity::INFO, "third");

    std::vector<CapturedWarning> captured;
    q.drain([&](PendingStartupWarnings::Severity s, const std::string& m) {
        captured.push_back({s, m});
    });

    REQUIRE(captured.size() == 3);
    REQUIRE(captured[0].message == "first");
    REQUIRE(captured[0].severity == PendingStartupWarnings::Severity::WARNING);
    REQUIRE(captured[1].message == "second");
    REQUIRE(captured[1].severity == PendingStartupWarnings::Severity::ERROR);
    REQUIRE(captured[2].message == "third");
    REQUIRE(captured[2].severity == PendingStartupWarnings::Severity::INFO);
}

TEST_CASE("PendingStartupWarnings: drain empties the queue", "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    q.enqueue(PendingStartupWarnings::Severity::ERROR, "only");

    int count1 = 0;
    q.drain([&](auto, auto&) { count1++; });
    REQUIRE(count1 == 1);

    int count2 = 0;
    q.drain([&](auto, auto&) { count2++; });
    REQUIRE(count2 == 0);
}

TEST_CASE("PendingStartupWarnings: clear() removes all entries", "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    q.enqueue(PendingStartupWarnings::Severity::ERROR, "a");
    q.enqueue(PendingStartupWarnings::Severity::ERROR, "b");
    q.clear();

    int count = 0;
    q.drain([&](auto, auto&) { count++; });
    REQUIRE(count == 0);
}

TEST_CASE("PendingStartupWarnings: concurrent enqueue is safe", "[startup_warnings]") {
    auto& q = PendingStartupWarnings::instance();
    q.clear();

    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; t++) {
        workers.emplace_back([&q, t]() {
            for (int i = 0; i < kPerThread; i++) {
                q.enqueue(PendingStartupWarnings::Severity::WARNING,
                          "t" + std::to_string(t) + "_" + std::to_string(i));
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    int count = 0;
    q.drain([&](auto, auto&) { count++; });
    REQUIRE(count == kThreads * kPerThread);
}
