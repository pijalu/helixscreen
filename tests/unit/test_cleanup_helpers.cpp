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

#include "test_helpers/update_queue_test_access.h"
#include "ui_utils.h"

using helix::ui::UpdateQueue;
using helix::ui::UpdateQueueTestAccess;

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_deferred nullifies pointer immediately",
                 "[cleanup][cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    REQUIRE(obj != nullptr);

    helix::ui::safe_delete_deferred(obj);

    REQUIRE(obj == nullptr);

    // Drain queue to clean up the pending deletion
    UpdateQueueTestAccess::drain(UpdateQueue::instance());
}

TEST_CASE_METHOD(LVGLTestFixture, "safe_delete_deferred deletes object after drain",
                 "[cleanup][cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_t* raw_copy = obj;
    REQUIRE(lv_obj_is_valid(raw_copy));

    helix::ui::safe_delete_deferred(obj);

    // Pointer is nullified but object still exists until drain
    REQUIRE(obj == nullptr);
    REQUIRE(lv_obj_is_valid(raw_copy));

    // After drain, the object is deleted
    UpdateQueueTestAccess::drain(UpdateQueue::instance());
    REQUIRE_FALSE(lv_obj_is_valid(raw_copy));
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "multiple safe_delete_deferred in same batch does not crash",
                 "[cleanup][cleanup_helpers]") {
    constexpr int COUNT = 5;
    lv_obj_t* objs[COUNT];
    lv_obj_t* raw_copies[COUNT];

    for (int i = 0; i < COUNT; ++i) {
        objs[i] = lv_obj_create(test_screen());
        raw_copies[i] = objs[i];
    }

    // Delete all in quick succession (simulates the #356 crash scenario)
    for (int i = 0; i < COUNT; ++i) {
        helix::ui::safe_delete_deferred(objs[i]);
    }

    // All pointers nullified immediately
    for (int i = 0; i < COUNT; ++i) {
        REQUIRE(objs[i] == nullptr);
    }

    // Drain should not crash
    REQUIRE_NOTHROW(UpdateQueueTestAccess::drain(UpdateQueue::instance()));

    // All objects deleted after drain
    for (int i = 0; i < COUNT; ++i) {
        REQUIRE_FALSE(lv_obj_is_valid(raw_copies[i]));
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "hide plus deferred deletion pattern",
                 "[cleanup][cleanup_helpers]") {
    lv_obj_t* obj = lv_obj_create(test_screen());
    lv_obj_t* raw_copy = obj;
    REQUIRE(lv_obj_is_valid(raw_copy));

    // Apply the hide + deferred deletion pattern:
    // 1. Hide immediately so the widget disappears from the UI
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    REQUIRE(lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN));

    // 2. Queue deletion for the next drain cycle
    helix::ui::queue_update("test_deferred_delete", [raw_copy]() {
        if (lv_obj_is_valid(raw_copy)) {
            lv_obj_delete(raw_copy);
        }
    });

    // Object still exists before drain
    REQUIRE(lv_obj_is_valid(raw_copy));

    // After drain, the deferred deletion executes
    UpdateQueueTestAccess::drain(UpdateQueue::instance());

    // Object should be deleted after processing
    REQUIRE_FALSE(lv_obj_is_valid(raw_copy));
}
