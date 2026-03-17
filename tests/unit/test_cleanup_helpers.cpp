// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_cleanup_helpers.cpp
 * @brief Tests for safe_delete_obj() and safe_delete_timer() helpers
 *
 * These helpers eliminate the if-delete-null pattern repeated in panel destructors.
 */

#include "../lvgl_test_fixture.h"
#include "ui/ui_cleanup_helpers.h"

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

// ============================================================================
// safe_delete_obj() tests
// ============================================================================

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_obj deletes valid object and nulls pointer",
                 "[cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    REQUIRE(obj != nullptr);

    safe_delete_obj(obj);

    REQUIRE(obj == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_obj is safe with nullptr", "[cleanup_helpers]") {
    lv_obj_t* obj = nullptr;

    // Should not crash
    safe_delete_obj(obj);

    REQUIRE(obj == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_obj can be called multiple times safely",
                 "[cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());

    safe_delete_obj(obj);
    REQUIRE(obj == nullptr);

    // Second call should be safe (no double-free)
    safe_delete_obj(obj);
    REQUIRE(obj == nullptr);
}

// ============================================================================
// safe_delete_timer() tests
// ============================================================================

static void dummy_timer_cb(lv_timer_t*) {
    // No-op callback for test timers
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_timer deletes valid timer and nulls pointer",
                 "[cleanup_helpers]") {
    lv_timer_t* timer = lv_timer_create(dummy_timer_cb, 1000, nullptr);
    REQUIRE(timer != nullptr);

    safe_delete_timer(timer);

    REQUIRE(timer == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_timer is safe with nullptr", "[cleanup_helpers]") {
    lv_timer_t* timer = nullptr;

    // Should not crash
    safe_delete_timer(timer);

    REQUIRE(timer == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_timer can be called multiple times safely",
                 "[cleanup_helpers]") {
    lv_timer_t* timer = lv_timer_create(dummy_timer_cb, 1000, nullptr);

    safe_delete_timer(timer);
    REQUIRE(timer == nullptr);

    // Second call should be safe (no double-free)
    safe_delete_timer(timer);
    REQUIRE(timer == nullptr);
}

// ============================================================================
// safe_delete_deferred() tests
// ============================================================================

#include "ui_utils.h"

#include "misc/lv_timer_private.h"

/// Process pending lv_async_call / lv_obj_delete_async one-shot timers.
/// Unlike lv_timer_handler(), this only fires one-shot timers and avoids
/// the infinite-loop problem with display refresh timers in the test fixture.
static void process_async_timers() {
    for (int safety = 0; safety < 100; safety++) {
        bool found = false;
        lv_timer_t* t = lv_timer_get_next(nullptr);
        while (t) {
            lv_timer_t* next = lv_timer_get_next(t);
            if (t->repeat_count > 0 && t->timer_cb) {
                t->timer_cb(t);
                found = true;
                break; // Restart — list may have changed
            }
            t = next;
        }
        if (!found)
            break;
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_deferred nullifies pointer immediately",
                 "[cleanup][cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    REQUIRE(obj != nullptr);

    helix::ui::safe_delete_deferred(obj);

    REQUIRE(obj == nullptr);

    // Process timers to execute the pending lv_obj_delete_async
    process_async_timers();
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_deferred deletes object after timer tick",
                 "[cleanup][cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_t* raw_copy = obj;
    REQUIRE(lv_obj_is_valid(raw_copy));

    helix::ui::safe_delete_deferred(obj);

    // Pointer is nullified but object still exists until timer fires
    REQUIRE(obj == nullptr);
    // Object is hidden immediately
    REQUIRE(lv_obj_has_flag(raw_copy, LV_OBJ_FLAG_HIDDEN));

    // After timer tick, the async deletion executes
    process_async_timers();
    REQUIRE_FALSE(lv_obj_is_valid(raw_copy));
}

TEST_CASE_METHOD(LVGLTestFixture, "multiple safe_delete_deferred in same batch does not crash",
                 "[cleanup][cleanup_helpers]") {
    constexpr int COUNT = 5;
    lv_obj_t* objs[COUNT];
    lv_obj_t* raw_copies[COUNT];

    for (int i = 0; i < COUNT; ++i) {
        objs[i] = lv_obj_create(test_screen());
        raw_copies[i] = objs[i];
    }

    // Delete all in quick succession (simulates the #356 crash scenario).
    // Now uses lv_obj_delete_async so multiple deletes in the same tick
    // are safe — LVGL processes them individually, not in a batch.
    for (int i = 0; i < COUNT; ++i) {
        helix::ui::safe_delete_deferred(objs[i]);
    }

    // All pointers nullified immediately
    for (int i = 0; i < COUNT; ++i) {
        REQUIRE(objs[i] == nullptr);
    }

    // Timer tick should not crash — processes all async deletions
    REQUIRE_NOTHROW(process_async_timers());

    // All objects deleted after timer tick
    for (int i = 0; i < COUNT; ++i) {
        REQUIRE_FALSE(lv_obj_is_valid(raw_copies[i]));
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_deferred hides and defers deletion",
                 "[cleanup][cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_t* raw_copy = obj;
    REQUIRE(lv_obj_is_valid(raw_copy));

    // safe_delete_deferred hides immediately and defers via lv_obj_delete_async
    helix::ui::safe_delete_deferred(obj);

    // Pointer nullified immediately
    REQUIRE(obj == nullptr);
    // Object hidden immediately but still exists
    REQUIRE(lv_obj_is_valid(raw_copy));
    REQUIRE(lv_obj_has_flag(raw_copy, LV_OBJ_FLAG_HIDDEN));

    // After timer tick, the async deletion executes
    process_async_timers();
    REQUIRE_FALSE(lv_obj_is_valid(raw_copy));
}
