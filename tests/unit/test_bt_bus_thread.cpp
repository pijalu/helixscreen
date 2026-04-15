// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for helix::bluetooth::BusThread — the single-threaded owner of an
// sd_bus* connection. Tests exercise lifecycle, serialization, and shutdown
// semantics without requiring a real BlueZ/D-Bus stack.
//
// Bus acquisition: uses sd_bus_new() to obtain an unstarted bus and sets the
// BusThread's skip_bus_calls_for_test flag so the worker loop bypasses the
// sd_bus_process / sd_bus_get_fd / sd_bus_get_timeout calls. (Empirical finding
// from Option A attempt: sd_bus_process on an unstarted bus returns -ENOTCONN,
// which trips BusThread's error path and kills the worker before the tests
// can exercise it.) The tests exercise only the work-queue, lifecycle, and
// serialization logic — which is what the plan calls for. Full D-Bus behaviour
// is covered by integration tests against real BlueZ on target hardware.
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

namespace {

struct BusHandle {
    sd_bus* bus = nullptr;
    BusHandle() { sd_bus_new(&bus); }
    ~BusHandle() {
        if (bus) sd_bus_unref(bus);
    }
};

} // namespace

TEST_CASE("BusThread starts and stops cleanly", "[bt][slow]") {
    BusHandle h;
    REQUIRE(h.bus != nullptr);

    BusThread bt(h.bus);
    bt.set_skip_bus_calls_for_test(true);
    bt.start();
    std::this_thread::sleep_for(50ms);
    bt.stop();
    // stop() is idempotent
    bt.stop();
    SUCCEED("start/stop completed without hang or crash");
}

TEST_CASE("BusThread submit runs the work item on the thread", "[bt][slow]") {
    BusHandle h;
    REQUIRE(h.bus != nullptr);

    BusThread bt(h.bus);
    bt.set_skip_bus_calls_for_test(true);
    bt.start();

    std::thread::id caller_id = std::this_thread::get_id();
    std::thread::id work_id{};

    bt.submit([&](sd_bus*) { work_id = std::this_thread::get_id(); }).get();

    REQUIRE(work_id != std::thread::id{});
    REQUIRE(work_id != caller_id);

    bt.stop();
}

TEST_CASE("BusThread run_sync blocks until work completes", "[bt][slow]") {
    BusHandle h;
    REQUIRE(h.bus != nullptr);

    BusThread bt(h.bus);
    bt.set_skip_bus_calls_for_test(true);
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
    BusHandle h;
    REQUIRE(h.bus != nullptr);

    BusThread bt(h.bus);
    bt.set_skip_bus_calls_for_test(true);
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
    BusHandle h;
    REQUIRE(h.bus != nullptr);

    BusThread bt(h.bus);
    bt.set_skip_bus_calls_for_test(true);
    bt.start();

    // Kick off a slow item so the worker is busy when stop() arrives.
    auto slow = bt.submit([](sd_bus*) { std::this_thread::sleep_for(100ms); });

    // Submitter thread: keeps pushing items while we call stop(). Any submit()
    // that sees stopping_==true or running_==false after stop() returns gets a
    // broken promise immediately (exception-bearing future). The race is
    // deterministic: the submitter outlives stop() and keeps pushing afterwards.
    std::atomic<bool> submitter_done{false};
    std::vector<std::future<void>> futures;
    std::mutex futures_mu;

    std::thread submitter([&] {
        // Keep pushing items until asked to stop. This guarantees that some
        // submit() calls happen AFTER stop()'s running_.exchange(false), so
        // they take the "BusThread not running" rejection path and return a
        // broken future.
        while (!submitter_done.load()) {
            auto f = bt.submit([](sd_bus*) {});
            {
                std::lock_guard<std::mutex> lk(futures_mu);
                futures.push_back(std::move(f));
            }
            std::this_thread::sleep_for(100us);
        }
    });

    // Give the submitter a moment to fill the queue, then tear down.
    std::this_thread::sleep_for(10ms);
    bt.stop();
    submitter_done.store(true);
    submitter.join();

    // After stop() returns: at least one submit()-returned future must have
    // been broken — either during stop()'s post-join drain (items pushed
    // before running_.exchange(false)) or during the submitter's subsequent
    // push attempts (rejected by the stopping_/!running_ guard).
    (void)slow;

    int broken = 0;
    {
        std::lock_guard<std::mutex> lk(futures_mu);
        for (auto& f : futures) {
            try {
                f.get();
            } catch (const std::exception&) {
                ++broken;
            }
        }
    }
    REQUIRE(broken > 0);
}
