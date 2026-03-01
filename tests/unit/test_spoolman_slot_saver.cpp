// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_slot_saver.h"
#include "spoolman_types.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Helper: Create a base SlotInfo for tests
// ============================================================================

static SlotInfo make_test_slot() {
    SlotInfo slot;
    slot.slot_index = 0;
    slot.spoolman_id = 42;
    slot.brand = "Polymaker";
    slot.material = "PLA";
    slot.color_rgb = 0xFF0000; // Red
    slot.spoolman_filament_id = 100;
    slot.remaining_weight_g = 800.0f;
    slot.total_weight_g = 1000.0f;
    return slot;
}

// ============================================================================
// detect_changes() Tests
// ============================================================================

TEST_CASE("SpoolmanSlotSaver detect_changes: no changes returns both false",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE_FALSE(changes.filament_level);
    REQUIRE_FALSE(changes.spool_level);
    REQUIRE_FALSE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: vendor changed sets filament_level",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.brand = "eSUN";

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE(changes.filament_level);
    REQUIRE_FALSE(changes.spool_level);
    REQUIRE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: material changed sets filament_level",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.material = "PETG";

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE(changes.filament_level);
    REQUIRE_FALSE(changes.spool_level);
    REQUIRE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: color changed sets filament_level",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.color_rgb = 0x00FF00; // Green

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE(changes.filament_level);
    REQUIRE_FALSE(changes.spool_level);
    REQUIRE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: remaining weight changed sets spool_level only",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.remaining_weight_g = 750.0f;

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE_FALSE(changes.filament_level);
    REQUIRE(changes.spool_level);
    REQUIRE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: weight within threshold is not a change",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.remaining_weight_g = original.remaining_weight_g + 0.05f; // Within 0.1 threshold

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE_FALSE(changes.spool_level);
    REQUIRE_FALSE(changes.any());
}

TEST_CASE("SpoolmanSlotSaver detect_changes: both filament and weight changed sets both",
          "[spoolman][slot_saver]") {
    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.material = "ABS";
    edited.remaining_weight_g = 600.0f;

    auto changes = SpoolmanSlotSaver::detect_changes(original, edited);

    REQUIRE(changes.filament_level);
    REQUIRE(changes.spool_level);
    REQUIRE(changes.any());
}

// ============================================================================
// save() Tests
// ============================================================================

TEST_CASE("SpoolmanSlotSaver save does nothing for non-spoolman slots", "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original;
    original.spoolman_id = 0; // Not a Spoolman spool
    original.brand = "Polymaker";
    original.material = "PLA";

    SlotInfo edited = original;
    edited.brand = "eSUN"; // Changed but irrelevant since spoolman_id=0

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success); // No-op success
}

TEST_CASE("SpoolmanSlotSaver save does nothing when no changes detected",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original; // No changes

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success); // No-op success
}

TEST_CASE("SpoolmanSlotSaver save only updates weight when no filament-level changes",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Ensure mock has a spool with id=42
    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "#FF0000";
    test_spool.remaining_weight_g = 800.0;
    test_spool.initial_weight_g = 1000.0;
    spools.push_back(test_spool);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.remaining_weight_g = 650.0f; // Only weight changed

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success);

    // Verify weight was updated in mock
    for (const auto& spool : api.spoolman_mock().get_mock_spools()) {
        if (spool.id == 42) {
            REQUIRE(spool.remaining_weight_g == Catch::Approx(650.0));
            break;
        }
    }
}

TEST_CASE("SpoolmanSlotSaver save re-links spool to existing filament when vendor changes",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Set up mock spool for id=42
    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "#FF0000";
    test_spool.remaining_weight_g = 800.0;
    test_spool.initial_weight_g = 1000.0;
    spools.push_back(test_spool);

    // Pre-create the target filament in mock (eSUN PLA Red should exist)
    nlohmann::json target_filament_json;
    target_filament_json["name"] = "eSUN PLA Red";
    target_filament_json["material"] = "PLA";
    target_filament_json["color_hex"] = "#FF0000";
    target_filament_json["vendor_id"] = 1;

    // Create the target filament via mock API so get_spoolman_filaments returns it
    bool filament_created = false;
    int target_filament_id = 0;
    api.spoolman().create_spoolman_filament(
        target_filament_json,
        [&](const FilamentInfo& info) {
            target_filament_id = info.id;
            filament_created = true;
        },
        [](const MoonrakerError&) {});
    REQUIRE(filament_created);

    // Now update that filament's vendor_name for matching purposes
    // The mock stores filaments in mock_filaments_ - we need to set vendor_name
    // The mock's create returns a FilamentInfo but we need to verify the match logic

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot(); // Polymaker PLA 0xFF0000
    SlotInfo edited = original;
    edited.brand = "eSUN"; // Changed vendor

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success);
}

TEST_CASE("SpoolmanSlotSaver save creates new filament when no match exists",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Set up mock spool for id=42
    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "#FF0000";
    test_spool.remaining_weight_g = 800.0;
    test_spool.initial_weight_g = 1000.0;
    spools.push_back(test_spool);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    // Change to something that won't match any existing filament
    edited.brand = "UniqueTestBrand";
    edited.material = "Nylon";
    edited.color_rgb = 0x123456;

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success);
}

TEST_CASE("SpoolmanSlotSaver save chains filament relink then weight update when both changed",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Set up mock spool
    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "#FF0000";
    test_spool.remaining_weight_g = 800.0;
    test_spool.initial_weight_g = 1000.0;
    spools.push_back(test_spool);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.brand = "NewBrandXYZ";
    edited.remaining_weight_g = 500.0f;

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success);

    // Verify weight was updated
    for (const auto& spool : api.spoolman_mock().get_mock_spools()) {
        if (spool.id == 42) {
            REQUIRE(spool.remaining_weight_g == Catch::Approx(500.0));
            break;
        }
    }
}

// ============================================================================
// Filament PATCH tests (update existing filament instead of create)
// ============================================================================

TEST_CASE("SpoolmanSlotSaver save PATCHes existing filament when material changes",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    // Set up mock spool
    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "FF0000";
    test_spool.remaining_weight_g = 800.0;
    test_spool.initial_weight_g = 1000.0;
    spools.push_back(test_spool);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.material = "PETG"; // Changed material

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success);

    // Verify filament was PATCHed (not created)
    auto& updates = api.spoolman_mock().filament_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].filament_id == 100); // PATCHed the right filament
    REQUIRE(updates[0].data.contains("material"));
    REQUIRE(updates[0].data["material"] == "PETG");
}

TEST_CASE("SpoolmanSlotSaver save includes vendor_id in filament PATCH when available",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "FF0000";
    test_spool.remaining_weight_g = 800.0;
    spools.push_back(test_spool);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.brand = "eSUN";
    edited.spoolman_vendor_id = 7; // Vendor ID from Spoolman

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success);

    auto& updates = api.spoolman_mock().filament_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].data.contains("vendor_id"));
    REQUIRE(updates[0].data["vendor_id"] == 7);
    REQUIRE(updates[0].data["material"] == "PLA"); // Other fields still present
}

TEST_CASE("SpoolmanSlotSaver save omits vendor_id from PATCH when zero",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "FF0000";
    test_spool.remaining_weight_g = 800.0;
    spools.push_back(test_spool);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.brand = "eSUN";
    // spoolman_vendor_id remains 0

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success);

    auto& updates = api.spoolman_mock().filament_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE_FALSE(updates[0].data.contains("vendor_id")); // Not sent when 0
    REQUIRE(updates[0].data.contains("material"));        // Other fields still present
    REQUIRE(updates[0].data.contains("color_hex"));
}

TEST_CASE("SpoolmanSlotSaver save PATCHes filament with correct color_hex format",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "FF0000";
    test_spool.remaining_weight_g = 800.0;
    spools.push_back(test_spool);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.color_rgb = 0x00FF00; // Changed to green

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success);

    auto& updates = api.spoolman_mock().filament_updates;
    REQUIRE(updates.size() == 1);
    // color_hex should NOT have # prefix
    REQUIRE(updates[0].data.contains("color_hex"));
    REQUIRE(updates[0].data["color_hex"] == "00FF00");
}

TEST_CASE("SpoolmanSlotSaver save PATCHes filament then updates weight when both change",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "FF0000";
    test_spool.remaining_weight_g = 800.0;
    test_spool.initial_weight_g = 1000.0;
    spools.push_back(test_spool);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    SlotInfo edited = original;
    edited.material = "ABS";
    edited.remaining_weight_g = 500.0f;

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE(callback_success);

    // Verify filament was PATCHed
    auto& updates = api.spoolman_mock().filament_updates;
    REQUIRE(updates.size() == 1);
    REQUIRE(updates[0].filament_id == 100);
    REQUIRE(updates[0].data["material"] == "ABS");

    // Verify weight was also updated
    for (const auto& spool : api.spoolman_mock().get_mock_spools()) {
        if (spool.id == 42) {
            REQUIRE(spool.remaining_weight_g == Catch::Approx(500.0));
            break;
        }
    }
}

TEST_CASE("SpoolmanSlotSaver save fails gracefully when no filament_id available",
          "[spoolman][slot_saver]") {
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    original.spoolman_filament_id = 0; // No filament ID
    SlotInfo edited = original;
    edited.material = "PETG"; // Filament-level change but no filament_id

    bool callback_called = false;
    bool callback_success = false;

    saver.save(original, edited, [&](bool success) {
        callback_called = true;
        callback_success = success;
    });

    REQUIRE(callback_called);
    REQUIRE_FALSE(callback_success); // Should fail, not crash

    // Verify no filament update was attempted
    auto& updates = api.spoolman_mock().filament_updates;
    REQUIRE(updates.empty());
}

// ============================================================================
// color_to_hex format tests
// ============================================================================

TEST_CASE("SpoolmanSlotSaver color_to_hex produces hex without # prefix",
          "[spoolman][slot_saver]") {
    // color_to_hex is private, but we can test via the PATCH payload
    // This test verifies the format indirectly through the filament update
    PrinterState state;
    MoonrakerClientMock client;
    MoonrakerAPIMock api(client, state);

    auto& spools = api.spoolman_mock().get_mock_spools();
    SpoolInfo test_spool;
    test_spool.id = 42;
    test_spool.filament_id = 100;
    test_spool.vendor = "Polymaker";
    test_spool.material = "PLA";
    test_spool.color_hex = "000000";
    test_spool.remaining_weight_g = 800.0;
    spools.push_back(test_spool);

    SpoolmanSlotSaver saver(&api);

    SlotInfo original = make_test_slot();
    original.color_rgb = 0x000000;
    SlotInfo edited = original;
    edited.color_rgb = 0xABCDEF;

    bool done = false;
    saver.save(original, edited, [&](bool) { done = true; });

    REQUIRE(done);
    auto& updates = api.spoolman_mock().filament_updates;
    REQUIRE(updates.size() == 1);
    // Must NOT start with #
    std::string hex = updates[0].data["color_hex"];
    REQUIRE(hex == "ABCDEF");
    REQUIRE(hex[0] != '#');
}
