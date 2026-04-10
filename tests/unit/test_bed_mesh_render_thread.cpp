// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_bed_mesh_render_thread.cpp
 * @brief Unit tests for BedMeshRenderThread
 *
 * Tests thread lifecycle safety and API contracts.
 * No real renderer is used -- these verify start/stop, double-buffering
 * state, and request coalescing without actual rendering.
 */

#include "bed_mesh_render_thread.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "../catch_amalgamated.hpp"

using helix::mesh::BedMeshRenderThread;

// ============================================================================
// Lifecycle tests
// ============================================================================

TEST_CASE("BedMeshRenderThread stop without start is safe", "[bed_mesh]") {
    BedMeshRenderThread thread;
    REQUIRE_FALSE(thread.is_running());
    thread.stop(); // should be a no-op
    REQUIRE_FALSE(thread.is_running());
}

TEST_CASE("BedMeshRenderThread start and stop", "[bed_mesh]") {
    BedMeshRenderThread thread;
    thread.start(100, 100);
    REQUIRE(thread.is_running());
    thread.stop();
    REQUIRE_FALSE(thread.is_running());
}

TEST_CASE("BedMeshRenderThread double stop is safe", "[bed_mesh]") {
    BedMeshRenderThread thread;
    thread.start(100, 100);
    REQUIRE(thread.is_running());
    thread.stop();
    REQUIRE_FALSE(thread.is_running());
    thread.stop(); // second stop -- must not crash or hang
    REQUIRE_FALSE(thread.is_running());
}

TEST_CASE("BedMeshRenderThread destructor stops cleanly", "[bed_mesh]") {
    auto thread = std::make_unique<BedMeshRenderThread>();
    thread->start(100, 100);
    REQUIRE(thread->is_running());
    thread.reset(); // destructor should join without hanging
}

// ============================================================================
// Buffer access tests
// ============================================================================

TEST_CASE("BedMeshRenderThread buffer state before any render", "[bed_mesh]") {
    BedMeshRenderThread thread;
    thread.start(64, 64);

    SECTION("has_ready_buffer is false initially") {
        REQUIRE_FALSE(thread.has_ready_buffer());
    }

    SECTION("get_ready_buffer returns nullptr when no frame rendered") {
        REQUIRE(thread.get_ready_buffer() == nullptr);
    }

    SECTION("last_render_time_ms is zero initially") {
        REQUIRE(thread.last_render_time_ms() == Catch::Approx(0.0f));
    }

    thread.stop();
}

// ============================================================================
// Request coalescing / no-crash tests
// ============================================================================

TEST_CASE("BedMeshRenderThread request without renderer does not crash", "[bed_mesh]") {
    BedMeshRenderThread thread;
    thread.start(64, 64);

    // No renderer set -- request_render should not crash (render loop
    // will attempt render, fail, and go back to waiting).
    thread.request_render();

    // Give the thread a moment to process
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    REQUIRE(thread.is_running());
    thread.stop();
}

TEST_CASE("BedMeshRenderThread multiple rapid requests do not deadlock", "[bed_mesh]") {
    BedMeshRenderThread thread;
    thread.start(64, 64);

    // Fire many requests rapidly -- they should coalesce
    for (int i = 0; i < 100; i++) {
        thread.request_render();
    }

    // The thread should still be alive and responsive
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(thread.is_running());

    thread.stop();
}

TEST_CASE("BedMeshRenderThread frame ready callback is invocable", "[bed_mesh]") {
    BedMeshRenderThread thread;

    std::atomic<int> callback_count{0};
    thread.set_frame_ready_callback([&callback_count]() { callback_count++; });

    thread.start(64, 64);
    // No renderer, so callback won't fire, but setting it should be safe
    thread.stop();

    // Callback may or may not have been called (no renderer = render fails),
    // but we should not have crashed.
    SUCCEED("No crash during callback lifecycle");
}

TEST_CASE("BedMeshRenderThread set_colors is safe while running", "[bed_mesh]") {
    BedMeshRenderThread thread;
    thread.start(64, 64);

    bed_mesh_render_colors_t colors{};
    colors.bg_r = 30;
    colors.bg_g = 30;
    colors.bg_b = 30;
    colors.grid_r = 60;
    colors.grid_g = 60;
    colors.grid_b = 60;

    // Should be safe to call from main thread while render thread is alive
    thread.set_colors(colors);

    thread.stop();
}
