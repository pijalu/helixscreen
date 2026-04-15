// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for helix::bluetooth::BusThread — the single-threaded owner of an
// sd_bus* connection. Tests exercise lifecycle, serialization, and shutdown
// semantics without requiring a real BlueZ/D-Bus stack.
//
// Bus acquisition: tests construct BusThread with nullptr. A null bus is a
// legal configuration meaning "no bus, idle worker" — the loop runs and
// processes queued work items but skips all sd_bus_* calls. This lets the
// tests exercise the work-queue, lifecycle, and serialization logic — which
// is what the plan calls for. Full D-Bus behaviour is covered by integration
// tests against real BlueZ on target hardware.
//
// Source inclusion: we compile bt_bus_thread.cpp directly into this translation
// unit (same pattern as test_ui_switch.cpp) because src/bluetooth/*.cpp is
// excluded from APP_SRCS (built into the libhelix-bluetooth.so plugin instead).

#include "../catch_amalgamated.hpp"

#include <systemd/sd-bus.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

// Include implementation directly — src/bluetooth/ is excluded from APP_SRCS
// (it lives in the Bluetooth plugin .so), so the object isn't available to
// link against for tests.
#include "../../src/bluetooth/bt_bus_thread.cpp"

using helix::bluetooth::BusThread;
using namespace std::chrono_literals;

TEST_CASE("BusThread starts and stops cleanly", "[bt][slow]") {
    BusThread bt(nullptr);
    bt.start();
    std::this_thread::sleep_for(50ms);
    bt.stop();
    // stop() is idempotent
    bt.stop();
    SUCCEED("start/stop completed without hang or crash");
}

TEST_CASE("BusThread submit runs the work item on the thread", "[bt][slow]") {
    BusThread bt(nullptr);
    bt.start();

    std::thread::id caller_id = std::this_thread::get_id();
    std::thread::id work_id{};

    bt.submit([&](sd_bus*) { work_id = std::this_thread::get_id(); }).get();

    REQUIRE(work_id != std::thread::id{});
    REQUIRE(work_id != caller_id);

    bt.stop();
}

TEST_CASE("BusThread run_sync blocks until work completes", "[bt][slow]") {
    BusThread bt(nullptr);
    bt.start();

    std::atomic<int> counter{0};
    bt.run_sync([&](sd_bus*) {
        std::this_thread::sleep_for(20ms);
        counter.store(42);
    });
    // run_sync must not return before the work completes.
    REQUIRE(counter.load() == 42);

    bt.stop();
}

TEST_CASE("BusThread serializes interleaved work", "[bt][slow]") {
    BusThread bt(nullptr);
    bt.start();

    std::atomic<int> in_flight{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> completed{0};

    constexpr int kThreads = 3;
    constexpr int kPerThread = 20;

    std::vector<std::thread> submitters;
    submitters.reserve(kThreads);

    // Collect futures on the main thread to avoid racing on a shared container.
    std::mutex futures_mu;
    std::vector<std::future<void>> futures;
    futures.reserve(kThreads * kPerThread);

    for (int t = 0; t < kThreads; ++t) {
        submitters.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                auto fut = bt.submit([&](sd_bus*) {
                    int now = in_flight.fetch_add(1) + 1;
                    int prev_max = max_concurrent.load();
                    while (now > prev_max &&
                           !max_concurrent.compare_exchange_weak(prev_max, now)) {
                        // retry
                    }
                    // Small amount of work to widen the window for any racing work items.
                    std::this_thread::sleep_for(100us);
                    in_flight.fetch_sub(1);
                    completed.fetch_add(1);
                });
                std::lock_guard<std::mutex> lk(futures_mu);
                futures.push_back(std::move(fut));
            }
        });
    }

    for (auto& t : submitters) t.join();

    // Drain: run_sync returns only after all previously-queued items run
    // (FIFO order, single worker).
    bt.run_sync([](sd_bus*) {});

    for (auto& f : futures) {
        REQUIRE_NOTHROW(f.get());
    }

    REQUIRE(completed.load() == kThreads * kPerThread);
    REQUIRE(max_concurrent.load() == 1);
    REQUIRE(in_flight.load() == 0);

    bt.stop();
}

TEST_CASE("BusThread stop breaks pending futures", "[bt][slow]") {
    BusThread t(nullptr);
    t.start();

    auto slow = t.submit([](sd_bus*) { std::this_thread::sleep_for(100ms); });
    auto pending = t.submit([](sd_bus*) {});
    // stop() runs while the worker is still inside the slow item; pending stays queued
    // and gets its promise broken in stop()'s post-join drain.
    std::this_thread::sleep_for(10ms);  // ensure worker has picked up slow
    t.stop();

    REQUIRE_NOTHROW(slow.get());
    REQUIRE_THROWS(pending.get());
}

TEST_CASE("BusThread start is idempotent", "[bt][slow]") {
    BusThread t(nullptr);
    t.start();
    t.start();  // must be a no-op, no second thread spawned
    t.start();
    t.stop();
    SUCCEED();  // didn't crash or deadlock
}

TEST_CASE("BusThread run_sync inside a work item runs inline (no deadlock)", "[bt][slow]") {
    BusThread t(nullptr);
    t.start();

    std::atomic<int> outer_tid_seen{0};
    std::atomic<int> inner_tid_seen{0};
    t.run_sync([&](sd_bus*) {
        // We are on the bus thread.
        std::thread::id outer = std::this_thread::get_id();
        outer_tid_seen.store(1);
        t.run_sync([&](sd_bus*) {
            // Should run inline on the same thread (no queue, no deadlock).
            REQUIRE(std::this_thread::get_id() == outer);
            inner_tid_seen.store(1);
        });
    });
    REQUIRE(outer_tid_seen.load() == 1);
    REQUIRE(inner_tid_seen.load() == 1);
    t.stop();
}
