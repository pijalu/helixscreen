// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lvgl_test_fixture.h"
#include "observer_factory.h"
#include "test_helpers/update_queue_test_access.h"

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;
using helix::ui::UpdateQueueTestAccess;

TEST_CASE_METHOD(LVGLTestFixture, "ScopedFreeze discards queued callbacks", "[update_queue]") {
    auto& q = UpdateQueue::instance();
    bool ran = false;

    {
        auto freeze = q.scoped_freeze();
        q.queue([&ran]() { ran = true; });
        UpdateQueueTestAccess::drain(q);
    }

    REQUIRE_FALSE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "drain works before freeze", "[update_queue]") {
    auto& q = UpdateQueue::instance();
    bool first_ran = false;
    bool second_ran = false;

    // Queue and drain before freezing — callback should run
    q.queue([&first_ran]() { first_ran = true; });
    UpdateQueueTestAccess::drain(q);
    REQUIRE(first_ran);

    // Now freeze and queue — callback should be discarded
    {
        auto freeze = q.scoped_freeze();
        q.queue([&second_ran]() { second_ran = true; });
        UpdateQueueTestAccess::drain(q);
    }

    REQUIRE_FALSE(second_ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "ScopedFreeze is RAII — thaw on scope exit", "[update_queue]") {
    auto& q = UpdateQueue::instance();
    bool ran = false;

    // Freeze in inner scope
    {
        auto freeze = q.scoped_freeze();
    }

    // After scope exit, queue should work again
    q.queue([&ran]() { ran = true; });
    UpdateQueueTestAccess::drain(q);

    REQUIRE(ran);
}

TEST_CASE_METHOD(LVGLTestFixture, "queue resumes after thaw", "[update_queue]") {
    auto& q = UpdateQueue::instance();
    bool discarded_ran = false;
    bool resumed_ran = false;

    // Freeze — queued callback should be discarded
    {
        auto freeze = q.scoped_freeze();
        q.queue([&discarded_ran]() { discarded_ran = true; });
    }

    // After thaw, queue a new callback — should run
    q.queue([&resumed_ran]() { resumed_ran = true; });
    UpdateQueueTestAccess::drain(q);

    REQUIRE_FALSE(discarded_ran);
    REQUIRE(resumed_ran);
}

// ---------------------------------------------------------------------------
// observe_int_sync + ScopedFreeze interaction
//
// observe_int_sync defers its initial callback via queue_update(). If the
// observer is created while the queue is frozen (e.g. inside populate_widgets'
// scoped_freeze), the initial fire is silently dropped and the handler never
// runs — unless the subject changes again later.
//
// This documents the root cause of the "carousel fans show 0%" bug: widgets
// that set up observers during populate_widgets() must also call their bind
// function directly, because the deferred initial fire will be discarded.
// ---------------------------------------------------------------------------

namespace {
struct FakePanel {
    int observed_value = -1;
};
} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "observe_int_sync initial callback is lost during ScopedFreeze",
                 "[update_queue][observer]") {
    auto& q = UpdateQueue::instance();
    FakePanel panel;

    lv_subject_t subject;
    lv_subject_init_int(&subject, 42);

    {
        auto freeze = q.scoped_freeze();

        // Create observer while frozen — the initial fire is queued via
        // queue_update(), but the queue silently discards it.
        auto guard = helix::ui::observe_int_sync<FakePanel>(
            &subject, &panel,
            [](FakePanel* p, int value) { p->observed_value = value; });

        // Even draining won't help — the callback was never enqueued.
        UpdateQueueTestAccess::drain(q);
        REQUIRE(panel.observed_value == -1);
    }

    // After thaw, drain again — still nothing, the callback was lost.
    UpdateQueueTestAccess::drain(q);
    REQUIRE(panel.observed_value == -1);

    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "observe_int_sync initial callback works without ScopedFreeze",
                 "[update_queue][observer]") {
    auto& q = UpdateQueue::instance();
    FakePanel panel;

    lv_subject_t subject;
    lv_subject_init_int(&subject, 42);

    {
        // Create observer without freeze — initial fire should be delivered.
        auto guard = helix::ui::observe_int_sync<FakePanel>(
            &subject, &panel,
            [](FakePanel* p, int value) { p->observed_value = value; });

        UpdateQueueTestAccess::drain(q);
        REQUIRE(panel.observed_value == 42);
    }

    lv_subject_deinit(&subject);
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "observe_int_sync subsequent changes are delivered after thaw",
                 "[update_queue][observer]") {
    auto& q = UpdateQueue::instance();
    FakePanel panel;

    lv_subject_t subject;
    lv_subject_init_int(&subject, 0);

    ObserverGuard guard;
    {
        auto freeze = q.scoped_freeze();

        // Initial fire lost during freeze.
        guard = helix::ui::observe_int_sync<FakePanel>(
            &subject, &panel,
            [](FakePanel* p, int value) { p->observed_value = value; });
    }

    // Initial was lost — value still -1.
    UpdateQueueTestAccess::drain(q);
    REQUIRE(panel.observed_value == -1);

    // But a subsequent subject change IS delivered (queue is thawed).
    lv_subject_set_int(&subject, 99);
    UpdateQueueTestAccess::drain(q);
    REQUIRE(panel.observed_value == 99);

    guard.reset();
    lv_subject_deinit(&subject);
}
