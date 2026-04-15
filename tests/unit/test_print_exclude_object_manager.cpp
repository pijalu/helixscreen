// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_exclude_object_manager.cpp
 * @brief Tests for PrintExcludeObjectManager — specifically the status-driven
 *        truth model introduced to fix spurious EXCLUDE_OBJECT timeout toasts.
 *
 * Background: printer.gcode.script blocks until Klipper executes the queued gcode,
 * so during pre-print an EXCLUDE_OBJECT RPC can sit queued for minutes and hit the
 * 60s transport timeout before it returns. The fix:
 *   1. MoonrakerAPI::exclude_object() now uses silent=true + 15min timeout.
 *   2. The request tracker suppresses REQUEST_TIMEOUT events for silent requests.
 *   3. PrintExcludeObjectManager treats TIMEOUT errors as advisory (waits for the
 *      exclude_object status subscription to confirm) and only reverts / toasts for
 *      hard errors (validation, connection lost, JSON-RPC error).
 *
 * These tests pin the manager half of that contract: status subscription promotes
 * awaiting-confirmation objects into confirmed-excluded, and nothing else should
 * silently mutate excluded_objects_ out from under that flow.
 *
 * Per L048: calls that enqueue work via UpdateQueue must be followed by drain().
 * Per L053: static LVGL fixtures reset between tests as needed.
 */

#include "../../include/moonraker_api.h"
#include "../../include/moonraker_client_mock.h"
#include "../../include/moonraker_error.h"
#include "../../include/printer_state.h"
#include "../../include/ui_print_exclude_object_manager.h"
#include "../../include/ui_update_queue.h"
#include "../../lvgl/lvgl.h"
#include "../ui_test_utils.h"

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

struct LVGLInitializerExcludeMgr {
    LVGLInitializerExcludeMgr() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, NULL, sizeof(buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};
static LVGLInitializerExcludeMgr lvgl_init_mgr;

/// Fixture: manager wired to mock client and PrinterState, no gcode_viewer widget.
/// The manager's viewer-touching branches are all guarded by `if (gcode_viewer_)`,
/// so passing nullptr keeps us out of the LVGL widget graph while still exercising
/// the observer, awaiting-confirmation, and excluded_objects_ bookkeeping.
class ExcludeManagerFixture {
  public:
    ExcludeManagerFixture() : mock_client(MoonrakerClientMock::PrinterType::VORON_24) {
        state.init_subjects(false);
        mock_client.connect(
            "ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<MoonrakerAPI>(mock_client, state);
        manager = std::make_unique<PrintExcludeObjectManager>(api.get(), state, nullptr);
        manager->init();
    }

    ~ExcludeManagerFixture() {
        if (manager) {
            manager->deinit();
            manager.reset();
        }
        // Drain any pending UI updates the manager queued so they don't leak into
        // the next test's static singleton state (L053).
        UpdateQueue::instance().drain();
        api.reset();
        mock_client.disconnect();
    }

    MoonrakerClientMock mock_client;
    PrinterState state;
    std::unique_ptr<MoonrakerAPI> api;
    std::unique_ptr<PrintExcludeObjectManager> manager;
};

} // namespace

// ============================================================================
// Status subscription is the source of truth
// ============================================================================

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "status subscription promotes awaiting-confirmation to excluded",
                 "[exclude_object][manager][regression]") {
    // Manager has dispatched an EXCLUDE_OBJECT RPC for "Part_1"; visual is optimistic
    // but excluded_objects_ is still empty because we now wait for Klipper to confirm
    // via the exclude_object.excluded_objects status field.
    manager->add_awaiting_confirmation_for_testing("Part_1");
    REQUIRE(manager->is_awaiting_confirmation_for_testing("Part_1"));
    REQUIRE(manager->get_excluded_objects().count("Part_1") == 0);

    // Klipper pushes a status update via Moonraker subscription: Part_1 is now excluded.
    state.set_excluded_objects({"Part_1"});
    UpdateQueue::instance().drain();

    SECTION("object moves into confirmed excluded set") {
        REQUIRE(manager->get_excluded_objects().count("Part_1") == 1);
    }

    SECTION("awaiting-confirmation is cleared once Klipper confirms") {
        REQUIRE_FALSE(manager->is_awaiting_confirmation_for_testing("Part_1"));
    }
}

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "exclusions from other clients (no prior dispatch) still sync in",
                 "[exclude_object][manager]") {
    // No awaiting entry — this simulates another frontend (e.g. Mainsail) excluding
    // an object. The manager must still pick it up so its UI stays consistent.
    REQUIRE_FALSE(manager->is_awaiting_confirmation_for_testing("Foreign_Part"));

    state.set_excluded_objects({"Foreign_Part"});
    UpdateQueue::instance().drain();

    REQUIRE(manager->get_excluded_objects().count("Foreign_Part") == 1);
}

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "awaiting-confirmation survives a status update that doesn't confirm it",
                 "[exclude_object][manager][regression]") {
    // Manager is waiting on Part_B, but a status push arrives carrying only Part_A
    // (e.g. another object was excluded from a different client). Our pending entry
    // must remain awaiting — clearing it prematurely would let a later TIMEOUT error
    // silently forget the object.
    manager->add_awaiting_confirmation_for_testing("Part_B");

    state.set_excluded_objects({"Part_A"});
    UpdateQueue::instance().drain();

    REQUIRE(manager->get_excluded_objects().count("Part_A") == 1);
    REQUIRE(manager->is_awaiting_confirmation_for_testing("Part_B"));
    REQUIRE(manager->get_excluded_objects().count("Part_B") == 0);
}

// ============================================================================
// RPC error branching — TIMEOUT is advisory, other errors are fatal-to-the-dispatch
// ============================================================================

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "RPC TIMEOUT error leaves awaiting entry intact for status to confirm later",
                 "[exclude_object][manager][regression]") {
    manager->add_awaiting_confirmation_for_testing("LateExclude");

    MoonrakerError err;
    err.type = MoonrakerErrorType::TIMEOUT;
    err.method = "printer.gcode.script";
    err.message = "Printer command 'printer.gcode.script' timed out after 900000ms";

    manager->handle_rpc_error_for_testing("LateExclude", err);
    UpdateQueue::instance().drain();

    SECTION("awaiting entry remains — Klipper may still run the gcode") {
        REQUIRE(manager->is_awaiting_confirmation_for_testing("LateExclude"));
    }

    SECTION("excluded_objects_ is not mutated by the error path") {
        REQUIRE(manager->get_excluded_objects().empty());
    }

    // Simulate Klipper eventually confirming via the status subscription.
    state.set_excluded_objects({"LateExclude"});
    UpdateQueue::instance().drain();

    SECTION("late status arrival still promotes to confirmed") {
        REQUIRE(manager->get_excluded_objects().count("LateExclude") == 1);
        REQUIRE_FALSE(manager->is_awaiting_confirmation_for_testing("LateExclude"));
    }
}

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "RPC VALIDATION_ERROR clears awaiting and reverts",
                 "[exclude_object][manager][regression]") {
    manager->add_awaiting_confirmation_for_testing("BadName");

    MoonrakerError err;
    err.type = MoonrakerErrorType::VALIDATION_ERROR;
    err.method = "exclude_object";
    err.message = "Invalid object name contains illegal characters";

    manager->handle_rpc_error_for_testing("BadName", err);
    UpdateQueue::instance().drain();

    // The error path removes the awaiting entry so no late status push can resurrect it.
    REQUIRE_FALSE(manager->is_awaiting_confirmation_for_testing("BadName"));
    REQUIRE(manager->get_excluded_objects().count("BadName") == 0);
}

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "RPC CONNECTION_LOST clears awaiting",
                 "[exclude_object][manager]") {
    manager->add_awaiting_confirmation_for_testing("OrphanedExclude");

    MoonrakerError err;
    err.type = MoonrakerErrorType::CONNECTION_LOST;
    err.method = "printer.gcode.script";
    err.message = "WebSocket disconnected before response";

    manager->handle_rpc_error_for_testing("OrphanedExclude", err);
    UpdateQueue::instance().drain();

    // Connection loss is a real failure — drop the awaiting entry. Self-healing case:
    // on reconnect, status subscription will re-sync any exclusions that did run.
    REQUIRE_FALSE(manager->is_awaiting_confirmation_for_testing("OrphanedExclude"));
}

TEST_CASE_METHOD(ExcludeManagerFixture,
                 "clear_excluded_objects() also clears awaiting-confirmation",
                 "[exclude_object][manager]") {
    // A new print starts — both confirmed exclusions AND any in-flight dispatches
    // must reset, otherwise a stale awaiting entry could promote itself when the
    // next print's first status push arrives.
    manager->add_awaiting_confirmation_for_testing("StalePart");
    state.set_excluded_objects({"Confirmed"});
    UpdateQueue::instance().drain();
    REQUIRE(manager->get_excluded_objects().count("Confirmed") == 1);

    manager->clear_excluded_objects();

    REQUIRE(manager->get_excluded_objects().empty());
    REQUIRE_FALSE(manager->is_awaiting_confirmation_for_testing("StalePart"));
}
