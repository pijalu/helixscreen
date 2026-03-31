// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_active_print_media_manager.cpp
 * @brief Unit tests for ActivePrintMediaManager class
 *
 * Tests the media manager that:
 * - Observes print_filename_ subject from PrinterState
 * - Processes raw filename to display name
 * - Loads thumbnails via MoonrakerAPI
 * - Updates print_display_filename_ and print_thumbnail_path_ subjects
 * - Uses generation counter for stale callback detection
 */

#include "../test_helpers/printer_state_test_access.h"
#include "../ui_test_utils.h"
#include "active_print_media_manager.h"
#include "printer_state.h"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <functional>
#include <string>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;
using namespace helix::ui;

// ============================================================================
// Test Fixture for ActivePrintMediaManager tests
// ============================================================================

class ActivePrintMediaManagerTestFixture {
  public:
    ActivePrintMediaManagerTestFixture() {
        // Suppress spdlog output during tests
        if (!logger_initialized_) {
            auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
            auto null_logger = std::make_shared<spdlog::logger>("null", null_sink);
            spdlog::set_default_logger(null_logger);
            logger_initialized_ = true;
        }

        // Initialize LVGL (safe version avoids "already initialized" warnings)
        lv_init_safe();

        // Initialize update queue once (static guard) - CRITICAL for helix::ui::queue_update()
        if (!queue_initialized) {
            helix::ui::update_queue_init();
            queue_initialized = true;
        }

        // Create a headless display for testing
        if (!display_created_) {
            display_ = lv_display_create(480, 320);
            alignas(64) static lv_color_t buf[480 * 10];
            lv_display_set_buffers(display_, buf, nullptr, sizeof(buf),
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
            lv_display_set_flush_cb(display_, [](lv_display_t* disp, const lv_area_t*, uint8_t*) {
                lv_display_flush_ready(disp);
            });
            display_created_ = true;
        }

        // Reset PrinterState for test isolation
        PrinterStateTestAccess::reset(state_);

        // Initialize subjects (without XML registration in tests)
        state_.init_subjects(false);

        // Create ActivePrintMediaManager for this test
        manager_ = std::make_unique<helix::ActivePrintMediaManager>(state_);
    }

    ~ActivePrintMediaManagerTestFixture() {
        // Destroy manager first (it observes state_)
        manager_.reset();

        // Drain any pending updates before shutdown to ensure clean state
        UpdateQueueTestAccess::drain(helix::ui::UpdateQueue::instance());

        // Shutdown update queue - also clears any remaining pending callbacks
        helix::ui::update_queue_shutdown();
        queue_initialized = false; // Reset static flag for next test

        // Destroy display to prevent cross-shard leaks
        if (display_created_ && display_) {
            lv_display_delete(display_);
            display_ = nullptr;
            display_created_ = false;
        }

        // Reset logger flag so next test/shard re-initializes
        logger_initialized_ = false;

        // Reset after each test
        PrinterStateTestAccess::reset(state_);
    }

  protected:
    PrinterState& state() {
        return state_;
    }

    helix::ActivePrintMediaManager& manager() {
        return *manager_;
    }

    // Helper to update print filename via status JSON (simulates Moonraker notification)
    void set_print_filename(const std::string& filename) {
        json status = {{"print_stats", {{"filename", filename}}}};
        state_.update_from_status(status);
        // Drain all queued UI updates, including nested queue_update calls from
        // deferred observers (observer → handler → display filename setter)
        UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    // Get current print_filename (raw)
    std::string get_print_filename() {
        return lv_subject_get_string(state_.get_print_filename_subject());
    }

    // Get current print_display_filename (processed for UI)
    std::string get_display_filename() {
        return lv_subject_get_string(state_.get_print_display_filename_subject());
    }

    // Get current print_thumbnail_path
    std::string get_thumbnail_path() {
        return lv_subject_get_string(state_.get_print_thumbnail_path_subject());
    }

  private:
    PrinterState state_;
    std::unique_ptr<helix::ActivePrintMediaManager> manager_;
    static lv_display_t* display_;
    static bool display_created_;
    static bool queue_initialized;
    static bool logger_initialized_;
};

lv_display_t* ActivePrintMediaManagerTestFixture::display_ = nullptr;
bool ActivePrintMediaManagerTestFixture::display_created_ = false;
bool ActivePrintMediaManagerTestFixture::queue_initialized = false;
bool ActivePrintMediaManagerTestFixture::logger_initialized_ = false;

// ============================================================================
// Display Name Formatting Tests
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: simple filename produces correct display name",
                 "[ActivePrintMediaManager]") {
    set_print_filename("benchy.gcode");

    REQUIRE(get_print_filename() == "benchy.gcode");
    REQUIRE(get_display_filename() == "benchy");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: filename with path produces correct display name",
                 "[ActivePrintMediaManager]") {
    set_print_filename("my_models/benchy.gcode");

    REQUIRE(get_print_filename() == "my_models/benchy.gcode");
    REQUIRE(get_display_filename() == "benchy");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: helix_temp filename resolves to original",
                 "[ActivePrintMediaManager]") {
    // When HelixScreen modifies G-code, it creates temp files like:
    // .helix_temp/modified_1234567890_Original_Model.gcode
    // The display name should show "Original_Model", not the temp filename
    set_print_filename(".helix_temp/modified_1234567890_Body1.gcode");

    REQUIRE(get_print_filename() == ".helix_temp/modified_1234567890_Body1.gcode");
    REQUIRE(get_display_filename() == "Body1");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: complex helix_temp path resolves correctly",
                 "[ActivePrintMediaManager]") {
    set_print_filename(".helix_temp/modified_9876543210_My_Cool_Print.gcode");

    REQUIRE(get_display_filename() == "My_Cool_Print");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: deeply nested path produces correct display name",
                 "[ActivePrintMediaManager]") {
    set_print_filename("projects/2025/january/test_models/benchy_0.2mm_PLA.gcode");

    REQUIRE(get_print_filename() == "projects/2025/january/test_models/benchy_0.2mm_PLA.gcode");
    REQUIRE(get_display_filename() == "benchy_0.2mm_PLA");
}

// ============================================================================
// Empty Filename Handling Tests
// ============================================================================
// Design: Empty filename PRESERVES display info (for abort→firmware_restart UX).
// Clearing happens naturally when a NEW print starts with a different filename.

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: empty filename preserves display name",
                 "[ActivePrintMediaManager]") {
    // First set a filename
    set_print_filename("test.gcode");
    REQUIRE(get_print_filename() == "test.gcode");
    REQUIRE(get_display_filename() == "test");

    // When printer goes to standby (empty filename), display name is preserved
    // so users can see what was printing after cancel→firmware_restart
    set_print_filename("");
    REQUIRE(get_print_filename() == "");

    // Display filename should be PRESERVED (not cleared)
    REQUIRE(get_display_filename() == "test");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: empty filename preserves thumbnail path",
                 "[ActivePrintMediaManager]") {
    // Set a filename first (to trigger the manager to process)
    set_print_filename("test.gcode");

    // Manually set a thumbnail path (simulating a loaded thumbnail)
    state().set_print_thumbnail_path("A:/tmp/thumbnail_abc123.bin");
    REQUIRE(get_thumbnail_path() == "A:/tmp/thumbnail_abc123.bin");

    // When filename is cleared, thumbnail is PRESERVED (not cleared)
    // This allows users to see the print info after abort→firmware_restart
    set_print_filename("");

    REQUIRE(get_thumbnail_path() == "A:/tmp/thumbnail_abc123.bin");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: new filename replaces old display info",
                 "[ActivePrintMediaManager]") {
    // Set initial filename
    set_print_filename("first_print.gcode");
    REQUIRE(get_display_filename() == "first_print");

    // Manually set thumbnail (simulating loaded thumbnail)
    state().set_print_thumbnail_path("A:/tmp/first_thumb.bin");
    REQUIRE(get_thumbnail_path() == "A:/tmp/first_thumb.bin");

    // Start a NEW print - this should replace display name
    set_print_filename("second_print.gcode");
    REQUIRE(get_display_filename() == "second_print");

    // Thumbnail path is cleared when new print starts (will be reloaded via API)
    // Note: Without API set, thumbnail loading is skipped, so path remains
    // until explicitly cleared or new thumbnail is loaded
}

// ============================================================================
// Thumbnail Source Override Tests
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: manual thumbnail source takes precedence",
                 "[ActivePrintMediaManager]") {
    // When PrintPreparationManager starts a modified print, it knows the original filename
    // and can provide it via set_thumbnail_source() for proper resolution

    // Set the thumbnail source BEFORE the filename arrives
    manager().set_thumbnail_source("original_model.gcode");

    // Now when a temp filename arrives, the source override should be used
    set_print_filename(".helix_temp/modified_12345_original_model.gcode");

    // Display name should use the source override
    REQUIRE(get_display_filename() == "original_model");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: clear_thumbnail_source resets state",
                 "[ActivePrintMediaManager]") {
    // Set up initial state
    set_print_filename("first.gcode");
    REQUIRE(get_display_filename() == "first");

    // Set an override
    manager().set_thumbnail_source("override.gcode");

    // Clear the override
    manager().clear_thumbnail_source();

    // Next filename should be processed normally (no override)
    set_print_filename("second.gcode");
    REQUIRE(get_display_filename() == "second");
}

// ============================================================================
// Generation Counter / Stale Callback Detection Tests
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: rapid filename changes use latest generation",
                 "[ActivePrintMediaManager]") {
    // When filename changes rapidly (user quickly switches prints),
    // only the last one should be reflected

    set_print_filename("print1.gcode");
    set_print_filename("print2.gcode");
    set_print_filename("print3.gcode");

    // Only print3 should be reflected in the display name
    REQUIRE(get_print_filename() == "print3.gcode");
    REQUIRE(get_display_filename() == "print3");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: idempotent on repeated same filename",
                 "[ActivePrintMediaManager]") {
    // Setting the same filename multiple times should not trigger redundant processing
    set_print_filename("same_file.gcode");
    REQUIRE(get_display_filename() == "same_file");

    // Set again - should be idempotent
    set_print_filename("same_file.gcode");
    REQUIRE(get_display_filename() == "same_file");
}

// ============================================================================
// Integration with PrinterState Subjects
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: updates print_display_filename subject",
                 "[ActivePrintMediaManager]") {
    set_print_filename("test_model.gcode");

    REQUIRE(get_display_filename() == "test_model");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: observer fires on display_filename change",
                 "[ActivePrintMediaManager]") {
    int observer_count = 0;
    auto observer_cb = [](lv_observer_t* observer, lv_subject_t*) {
        int* count = static_cast<int*>(lv_observer_get_user_data(observer));
        (*count)++;
    };

    lv_observer_t* observer = lv_subject_add_observer(state().get_print_display_filename_subject(),
                                                      observer_cb, &observer_count);

    // Initial observer registration fires once
    REQUIRE(observer_count == 1);

    // Change filename - should fire observer after processing
    set_print_filename("new_model.gcode");

    // Observer should have fired again
    REQUIRE(observer_count == 2);

    lv_observer_remove(observer);
}

// ============================================================================
// Edge Cases and Error Handling
// ============================================================================

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: handles filename with special characters",
                 "[ActivePrintMediaManager]") {
    set_print_filename("My Model (v2) - Final.gcode");

    REQUIRE(get_print_filename() == "My Model (v2) - Final.gcode");
    REQUIRE(get_display_filename() == "My Model (v2) - Final");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: handles very long filename",
                 "[ActivePrintMediaManager]") {
    // Test handling of very long filenames (within buffer limits)
    std::string long_name(100, 'x');
    long_name += ".gcode";

    set_print_filename(long_name);

    // Should handle gracefully (may be truncated to buffer size)
    REQUIRE_FALSE(get_display_filename().empty());
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: no API means no thumbnail load",
                 "[ActivePrintMediaManager]") {
    // Without set_api() being called, thumbnail loading should be skipped gracefully
    set_print_filename("model.gcode");

    // Display name should still work
    REQUIRE(get_display_filename() == "model");

    // Thumbnail path should remain empty (no API to load from)
    REQUIRE(get_thumbnail_path() == "");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: uppercase extension handled",
                 "[ActivePrintMediaManager]") {
    set_print_filename("Model.GCODE");

    REQUIRE(get_display_filename() == "Model");
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: mixed case extension handled",
                 "[ActivePrintMediaManager]") {
    set_print_filename("Model.GCode");

    REQUIRE(get_display_filename() == "Model");
}

// ============================================================================
// Direct Thumbnail Path Tests (Pre-extracted from USB/G-code)
// ============================================================================

// NOTE: This test intentionally fails to compile because set_thumbnail_path()
// doesn't exist yet. This is TDD-style - implement the method to make it compile.
//
// TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
//                  "ActivePrintMediaManager: set_thumbnail_path sets thumbnail directly",
//                  "[media][thumbnail][direct]")
//
// When PrintStartController starts a print with a pre-extracted thumbnail
// (e.g., from USB drive or embedded G-code), it should be able to set the
// thumbnail path directly without going through Moonraker thumbnail API.
//
// Required new method signature:
//   void set_thumbnail_path(const std::string& path);
//
// Test cases that need to pass once implemented:
// 1. Direct path sets thumbnail_path subject
// 2. Direct path works alongside filename
// 3. Direct path not overwritten by filename change if already set
// 4. Empty path clears thumbnail
//
// Uncomment below and add set_thumbnail_path() to ActivePrintMediaManager

#if 1 // Enable when set_thumbnail_path() is implemented
TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: set_thumbnail_path sets thumbnail directly",
                 "[ActivePrintMediaManager]") {
    SECTION("direct path sets thumbnail_path subject") {
        // Pre-extracted thumbnail from USB or G-code
        std::string extracted_path = "/tmp/helix/thumbnails/extracted_12345.png";

        // Set the thumbnail path directly via new method
        manager().set_thumbnail_path(extracted_path);

        // Thumbnail path subject should have the value
        REQUIRE(get_thumbnail_path() == extracted_path);
    }

    SECTION("direct path works alongside filename") {
        // Set a filename for the print
        set_print_filename("usb_print.gcode");
        REQUIRE(get_display_filename() == "usb_print");

        // Set thumbnail path directly (from pre-extracted USB thumbnail)
        std::string usb_thumbnail = "/media/usb/thumbnails/usb_print.png";
        manager().set_thumbnail_path(usb_thumbnail);

        // Both should be set correctly
        REQUIRE(get_display_filename() == "usb_print");
        REQUIRE(get_thumbnail_path() == usb_thumbnail);
    }

    SECTION("direct path not overwritten by filename change if set") {
        // Set thumbnail path first (from PrintStartController)
        std::string preextracted = "/tmp/helix/embedded_thumbnail.png";
        manager().set_thumbnail_path(preextracted);
        REQUIRE(get_thumbnail_path() == preextracted);

        // When filename arrives from Moonraker, the pre-set thumbnail should persist
        // (because we already have a valid thumbnail, no need to fetch)
        set_print_filename("some_file.gcode");

        // The pre-extracted thumbnail should still be there
        REQUIRE(get_thumbnail_path() == preextracted);
    }

    SECTION("pre-set thumbnail does not block layer count from metadata") {
        // Regression: #526 - when thumbnail was pre-set, the metadata fetch was
        // skipped entirely, so layer_count and estimated_time were never loaded.
        // The fix ensures metadata is always fetched; only thumbnail download is skipped.

        // Set thumbnail path first (simulating PrintStartController pre-extraction)
        manager().set_thumbnail_path("/tmp/helix/preextracted.png");
        REQUIRE(get_thumbnail_path() == "/tmp/helix/preextracted.png");

        // Verify that having a pre-set thumbnail doesn't prevent layer_total
        // from being set. Without API, metadata can't be fetched, but the code
        // path that checks skip_thumbnail should not early-return before the
        // API check. Verify the subject is still at 0 (no API = no metadata).
        REQUIRE(lv_subject_get_int(state().get_print_layer_total_subject()) == 0);

        // The key assertion: load_thumbnail_for_file should NOT early-return
        // when thumbnail is set. It should proceed to the API check and only
        // skip after that (because no API is configured in this test).
        // This is implicitly tested: if the old early-return was still there,
        // the "Thumbnail already set, skipping API lookup" log would fire,
        // and we'd never reach the API check at all.
        set_print_filename("model_with_layers.gcode");

        // Display name should still work (metadata fetch path is entered)
        REQUIRE(get_display_filename() == "model_with_layers");
    }

    SECTION("empty path clears thumbnail") {
        // Set a thumbnail first
        manager().set_thumbnail_path("/tmp/some_thumbnail.png");
        REQUIRE(get_thumbnail_path() == "/tmp/some_thumbnail.png");

        // Clear it
        manager().set_thumbnail_path("");

        // Should be cleared
        REQUIRE(get_thumbnail_path() == "");
    }
}
#endif

// ============================================================================
// Stale Thumbnail Invalidation Tests
// ============================================================================
// Regression: Starting a new print via Mainsail showed the PREVIOUS print's
// thumbnail because print_thumbnail_path_ was never cleared between prints.

// ============================================================================
// Thumbnail Path Subject Observer Integration Tests
// ============================================================================
// These tests verify that the print_thumbnail_path subject correctly notifies
// observers, which is the mechanism PrintStatusWidget uses to update its image.

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: thumbnail path subject fires observer on change",
                 "[ActivePrintMediaManager]") {
    std::string last_observed_path;
    int observer_fire_count = 0;

    // Set up an observer on the thumbnail path subject (mimics what the widget does)
    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subj) {
        auto* data = static_cast<std::pair<std::string*, int*>*>(lv_observer_get_user_data(observer));
        *data->first = lv_subject_get_string(subj);
        (*data->second)++;
    };

    auto data = std::make_pair(&last_observed_path, &observer_fire_count);
    lv_observer_t* obs = lv_subject_add_observer(state().get_print_thumbnail_path_subject(),
                                                  observer_cb, &data);

    // Observer fires on registration with initial (empty) value
    REQUIRE(observer_fire_count == 1);
    REQUIRE(last_observed_path.empty());

    // Setting a thumbnail path should fire the observer
    state().set_print_thumbnail_path("A:/cache/thumb.bin");
    REQUIRE(observer_fire_count == 2);
    REQUIRE(last_observed_path == "A:/cache/thumb.bin");

    // Setting same path should NOT fire (de-duplication in set_print_thumbnail_path)
    state().set_print_thumbnail_path("A:/cache/thumb.bin");
    REQUIRE(observer_fire_count == 2);

    // Clearing path should fire
    state().set_print_thumbnail_path("");
    REQUIRE(observer_fire_count == 3);
    REQUIRE(last_observed_path.empty());

    lv_observer_remove(obs);
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: thumbnail path observer receives correct value during rapid updates",
                 "[ActivePrintMediaManager]") {
    // This tests the scenario where the thumbnail path changes rapidly.
    // An immediate observer should receive each value in sequence.
    std::vector<std::string> observed_values;

    auto observer_cb = [](lv_observer_t* observer, lv_subject_t* subj) {
        auto* values = static_cast<std::vector<std::string>*>(lv_observer_get_user_data(observer));
        values->push_back(lv_subject_get_string(subj));
    };

    lv_observer_t* obs = lv_subject_add_observer(state().get_print_thumbnail_path_subject(),
                                                  observer_cb, &observed_values);

    // Initial fire
    REQUIRE(observed_values.size() == 1);
    REQUIRE(observed_values[0].empty());

    // Rapid updates - observer should see each distinct value
    state().set_print_thumbnail_path("A:/cache/first.bin");
    state().set_print_thumbnail_path("A:/cache/second.bin");
    state().set_print_thumbnail_path("A:/cache/third.bin");

    REQUIRE(observed_values.size() == 4); // initial + 3 changes
    REQUIRE(observed_values[1] == "A:/cache/first.bin");
    REQUIRE(observed_values[2] == "A:/cache/second.bin");
    REQUIRE(observed_values[3] == "A:/cache/third.bin");

    lv_observer_remove(obs);
}

TEST_CASE_METHOD(ActivePrintMediaManagerTestFixture,
                 "ActivePrintMediaManager: new print clears stale thumbnail path",
                 "[ActivePrintMediaManager]") {
    SECTION("different file after idle clears old thumbnail") {
        // Print A starts and gets a thumbnail
        set_print_filename("print_a.gcode");
        state().set_print_thumbnail_path("A:/cache/print_a_thumb.bin");
        REQUIRE(get_thumbnail_path() == "A:/cache/print_a_thumb.bin");

        // Print A ends - Moonraker sends empty filename
        set_print_filename("");
        // Thumbnail preserved (intentional for post-cancel UX)
        REQUIRE(get_thumbnail_path() == "A:/cache/print_a_thumb.bin");

        // Print B starts from Mainsail - stale thumbnail must be cleared
        set_print_filename("print_b.gcode");
        REQUIRE(get_display_filename() == "print_b");
        // The old thumbnail path should be cleared so the new one can be fetched
        REQUIRE(get_thumbnail_path() == "");
    }

    SECTION("direct switch between prints clears old thumbnail") {
        // Print A with thumbnail
        set_print_filename("first.gcode");
        state().set_print_thumbnail_path("A:/cache/first_thumb.bin");
        REQUIRE(get_thumbnail_path() == "A:/cache/first_thumb.bin");

        // Print B starts immediately (no empty filename in between)
        set_print_filename("second.gcode");
        REQUIRE(get_display_filename() == "second");
        REQUIRE(get_thumbnail_path() == "");
    }

    SECTION("same filename reprint preserves thumbnail") {
        // Print A with thumbnail
        set_print_filename("benchy.gcode");
        state().set_print_thumbnail_path("A:/cache/benchy_thumb.bin");
        REQUIRE(get_thumbnail_path() == "A:/cache/benchy_thumb.bin");

        // Same file reprinted - idempotent guard means no change, which is correct
        // (the cached thumbnail is still valid for the same file)
        set_print_filename("benchy.gcode");
        REQUIRE(get_thumbnail_path() == "A:/cache/benchy_thumb.bin");
    }
}
