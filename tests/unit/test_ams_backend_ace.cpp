// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_ace.h"
#include "ams_types.h"

#include <json.hpp> // nlohmann/json from libhv

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;

/**
 * @brief Test helper class providing access to AmsBackendAce internals
 *
 * This class provides controlled access to private members for unit testing.
 * It does NOT start the backend (no Moonraker connection needed).
 */
class AmsBackendAceTestHelper : public AmsBackendAce {
  public:
    AmsBackendAceTestHelper() : AmsBackendAce(nullptr, nullptr) {}

    // Parse response helpers - call the protected parsing methods
    void test_parse_info_response(const json& data) {
        parse_info_response(data);
    }

    bool test_parse_status_response(const json& data) {
        return parse_status_response(data);
    }

    bool test_parse_slots_response(const json& data) {
        return parse_slots_response(data);
    }

    // State accessors for verification
    AmsSystemInfo get_test_system_info() const {
        return get_system_info();
    }

    DryerInfo get_test_dryer_info() const {
        return get_dryer_info();
    }
};

// ============================================================================
// Type and Topology Tests
// ============================================================================

TEST_CASE("ACE returns correct type", "[ams][ace][type]") {
    AmsBackendAceTestHelper helper;
    REQUIRE(helper.get_type() == AmsType::ACE);
}

TEST_CASE("ACE uses hub topology", "[ams][ace][topology]") {
    // ACE uses hub topology (4 slots merge to single output)
    AmsBackendAceTestHelper helper;
    REQUIRE(helper.get_topology() == PathTopology::HUB);
}

TEST_CASE("ACE bypass not supported", "[ams][ace][bypass]") {
    AmsBackendAceTestHelper helper;
    REQUIRE(helper.is_bypass_active() == false);

    auto err = helper.enable_bypass();
    REQUIRE(!err.success());
    REQUIRE(err.result == AmsResult::NOT_SUPPORTED);

    err = helper.disable_bypass();
    REQUIRE(!err.success());
    REQUIRE(err.result == AmsResult::NOT_SUPPORTED);
}

// ============================================================================
// Dryer Default State Tests
// ============================================================================

TEST_CASE("ACE dryer defaults", "[ams][ace][dryer]") {
    AmsBackendAceTestHelper helper;
    DryerInfo dryer = helper.get_test_dryer_info();

    // ACE always reports dryer as supported
    REQUIRE(dryer.supported == true);
    REQUIRE(dryer.allows_during_print == false); // Safe default: block during print

    // Default state should be inactive
    REQUIRE(dryer.active == false);

    // Should have reasonable temperature limits
    REQUIRE(dryer.min_temp_c >= 30.0f);
    REQUIRE(dryer.min_temp_c <= 40.0f);
    REQUIRE(dryer.max_temp_c >= 50.0f);
    REQUIRE(dryer.max_temp_c <= 80.0f);

    // Should have reasonable duration limit
    REQUIRE(dryer.max_duration_min >= 480);  // At least 8 hours
    REQUIRE(dryer.max_duration_min <= 1440); // At most 24 hours
}

TEST_CASE("ACE dryer progress calculation", "[ams][ace][dryer]") {
    DryerInfo dryer;
    dryer.supported = true;
    dryer.active = true;
    dryer.duration_min = 240;  // 4 hours
    dryer.remaining_min = 120; // 2 hours left

    // Should be 50% complete
    REQUIRE(dryer.get_progress_pct() == 50);

    // When not active, progress should be -1
    dryer.active = false;
    REQUIRE(dryer.get_progress_pct() == -1);
}

TEST_CASE("ACE drying presets available", "[ams][ace][dryer]") {
    AmsBackendAceTestHelper helper;
    auto presets = helper.get_drying_presets();

    // Should have at least 3 presets (PLA, PETG, ABS)
    REQUIRE(presets.size() >= 3);

    // Verify PLA preset exists and has reasonable values
    bool found_pla = false;
    for (const auto& preset : presets) {
        if (preset.name == "PLA") {
            found_pla = true;
            REQUIRE(preset.temp_c >= 40.0f);
            REQUIRE(preset.temp_c <= 50.0f);
            REQUIRE(preset.duration_min >= 180); // At least 3 hours
            break;
        }
    }
    REQUIRE(found_pla);
}

// ============================================================================
// Info Response Parsing Tests
// ============================================================================

TEST_CASE("ACE parse_info_response: valid response", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {{"model", "ACE Pro"}, {"version", "1.2.3"}, {"slot_count", 4}};

    helper.test_parse_info_response(data);
    auto info = helper.get_test_system_info();

    REQUIRE(info.type_name == "ACE");
    REQUIRE(info.units[0].name == "ACE Pro");
    REQUIRE(info.version == "1.2.3");
    REQUIRE(info.total_slots == 4);
    REQUIRE(info.units.size() == 1);
    REQUIRE(info.units[0].slots.size() == 4);
}

TEST_CASE("ACE parse_info_response: missing fields", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    // Empty response should not crash
    json data = json::object();
    helper.test_parse_info_response(data);

    auto info = helper.get_test_system_info();
    // Type name should be ACE
    REQUIRE(info.type == AmsType::ACE);
}

TEST_CASE("ACE parse_info_response: wrong types ignored", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    // String where int expected should be ignored, not crash
    json data = {
        {"model", 12345},      // Wrong type (number instead of string)
        {"version", true},     // Wrong type (bool instead of string)
        {"slot_count", "four"} // Wrong type (string instead of int)
    };

    // Should not throw or crash
    REQUIRE_NOTHROW(helper.test_parse_info_response(data));
}

TEST_CASE("ACE parse_info_response: excessive slot count rejected", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {
        {"slot_count", 100} // Unreasonable value
    };

    helper.test_parse_info_response(data);
    auto info = helper.get_test_system_info();

    // Should reject unreasonable slot count
    REQUIRE(info.total_slots != 100);
}

// ============================================================================
// Status Response Parsing Tests
// ============================================================================

TEST_CASE("ACE parse_status_response: loaded slot", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {{"loaded_slot", 2}, {"action", "idle"}};

    bool changed = helper.test_parse_status_response(data);
    REQUIRE(changed == true);

    auto info = helper.get_test_system_info();
    REQUIRE(info.current_slot == 2);
    REQUIRE(info.current_tool == 2); // 1:1 mapping
    REQUIRE(info.filament_loaded == true);
}

TEST_CASE("ACE parse_status_response: no filament loaded", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {{"loaded_slot", -1}};

    helper.test_parse_status_response(data);
    auto info = helper.get_test_system_info();

    REQUIRE(info.current_slot == -1);
    REQUIRE(info.filament_loaded == false);
}

TEST_CASE("ACE parse_status_response: action states", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    // Test loading action
    json data = {{"action", "loading"}};
    helper.test_parse_status_response(data);
    REQUIRE(helper.get_test_system_info().action == AmsAction::LOADING);

    // Test unloading action
    data = {{"action", "unloading"}};
    helper.test_parse_status_response(data);
    REQUIRE(helper.get_test_system_info().action == AmsAction::UNLOADING);

    // Test error action
    data = {{"action", "error"}};
    helper.test_parse_status_response(data);
    REQUIRE(helper.get_test_system_info().action == AmsAction::ERROR);

    // Test idle action
    data = {{"action", "idle"}};
    helper.test_parse_status_response(data);
    REQUIRE(helper.get_test_system_info().action == AmsAction::IDLE);
}

TEST_CASE("ACE parse_status_response: dryer state", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {{"dryer",
                  {{"active", true},
                   {"current_temp", 45.5},
                   {"target_temp", 55.0},
                   {"remaining_minutes", 180},
                   {"duration_minutes", 240}}}};

    helper.test_parse_status_response(data);
    auto dryer = helper.get_test_dryer_info();

    REQUIRE(dryer.active == true);
    REQUIRE(dryer.current_temp_c == Catch::Approx(45.5f));
    REQUIRE(dryer.target_temp_c == Catch::Approx(55.0f));
    REQUIRE(dryer.remaining_min == 180);
    REQUIRE(dryer.duration_min == 240);
}

TEST_CASE("ACE parse_status_response: dryer not active", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = {{"dryer", {{"active", false}, {"current_temp", 25.0}, {"target_temp", 0}}}};

    helper.test_parse_status_response(data);
    auto dryer = helper.get_test_dryer_info();

    REQUIRE(dryer.active == false);
    REQUIRE(dryer.target_temp_c == Catch::Approx(0.0f));
}

// ============================================================================
// Slots Response Parsing Tests
// ============================================================================

TEST_CASE("ACE parse_slots_response: valid slots", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    // First initialize with info response to set slot count
    json info = {{"slot_count", 4}};
    helper.test_parse_info_response(info);

    // Colors must be strings - ACE API returns hex strings like "#FF0000"
    json data = {
        {"slots",
         {{{"index", 0}, {"color", "#FF0000"}, {"material", "PLA"}, {"status", "available"}},
          {{"index", 1}, {"color", "#00FF00"}, {"material", "PETG"}, {"status", "empty"}},
          {{"index", 2}, {"color", "#0000FF"}, {"material", "ABS"}, {"status", "loaded"}},
          {{"index", 3}, {"color", "#FFFFFF"}, {"material", ""}, {"status", "unknown"}}}}};

    bool changed = helper.test_parse_slots_response(data);
    REQUIRE(changed == true);

    // Verify first slot
    auto slot0 = helper.get_slot_info(0);
    REQUIRE(slot0.color_rgb == 0xFF0000);
    REQUIRE(slot0.material == "PLA");
    REQUIRE(slot0.status == SlotStatus::AVAILABLE);

    // Verify empty slot
    auto slot1 = helper.get_slot_info(1);
    REQUIRE(slot1.status == SlotStatus::EMPTY);

    // Verify "loaded" status - ACE maps both "available" and "loaded" to AVAILABLE
    // (LOADED enum is for when filament is actively in the extruder path)
    auto slot2 = helper.get_slot_info(2);
    REQUIRE(slot2.status == SlotStatus::AVAILABLE);
}

TEST_CASE("ACE parse_slots_response: missing slots array", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    json data = json::object(); // No "slots" key

    bool changed = helper.test_parse_slots_response(data);
    REQUIRE(changed == false);
}

TEST_CASE("ACE parse_slots_response: excessive slots rejected", "[ams][ace][parse]") {
    AmsBackendAceTestHelper helper;

    // Create an array with too many slots
    json slots_array = json::array();
    for (int i = 0; i < 20; ++i) {
        slots_array.push_back({{"index", i}});
    }

    json data = {{"slots", slots_array}};

    bool changed = helper.test_parse_slots_response(data);
    REQUIRE(changed == false); // Should reject excessive count
}

// ============================================================================
// Filament Segment Tests
// ============================================================================

TEST_CASE("ACE filament segment when nothing loaded", "[ams][ace][segment]") {
    AmsBackendAceTestHelper helper;

    // Initialize with slots
    json info = {{"slot_count", 4}};
    helper.test_parse_info_response(info);

    json status = {{"loaded_slot", -1}};
    helper.test_parse_status_response(status);

    REQUIRE(helper.get_filament_segment() == PathSegment::NONE);
}

TEST_CASE("ACE filament segment when loaded", "[ams][ace][segment]") {
    AmsBackendAceTestHelper helper;

    // Initialize with slots
    json info = {{"slot_count", 4}};
    helper.test_parse_info_response(info);

    // Set slot 1 as loaded
    json status = {{"loaded_slot", 1}};
    helper.test_parse_status_response(status);

    // Mark slot 1 as available
    json slots = {{"slots",
                   {{{"index", 0}, {"status", "empty"}},
                    {{"index", 1}, {"status", "loaded"}},
                    {{"index", 2}, {"status", "empty"}},
                    {{"index", 3}, {"status", "empty"}}}}};
    helper.test_parse_slots_response(slots);

    // Overall segment should show filament at nozzle
    REQUIRE(helper.get_filament_segment() == PathSegment::NOZZLE);
}

TEST_CASE("ACE error segment inference", "[ams][ace][segment]") {
    AmsBackendAceTestHelper helper;

    // Set error state
    json status = {{"action", "error"}};
    helper.test_parse_status_response(status);

    // Should infer error at hub
    REQUIRE(helper.infer_error_segment() == PathSegment::HUB);
}

// ============================================================================
// Invalid Slot Handling Tests
// ============================================================================

TEST_CASE("ACE returns invalid markers for out-of-bounds slot", "[ams][ace][slot]") {
    AmsBackendAceTestHelper helper;

    // Before any initialization, getting any slot should return invalid markers
    auto slot = helper.get_slot_info(0);
    REQUIRE(slot.slot_index == -1);
    REQUIRE(slot.global_index == -1);

    // Invalid negative index should also return invalid markers
    slot = helper.get_slot_info(-1);
    REQUIRE(slot.slot_index == -1);
    REQUIRE(slot.global_index == -1);

    // Initialize with slots
    json info = {{"slot_count", 4}};
    helper.test_parse_info_response(info);

    // Valid slot should work
    auto valid_slot = helper.get_slot_info(0);
    REQUIRE(valid_slot.slot_index == 0);
    REQUIRE(valid_slot.global_index == 0);

    // Out-of-bounds should return invalid markers
    auto out_of_bounds = helper.get_slot_info(10);
    REQUIRE(out_of_bounds.slot_index == -1);
    REQUIRE(out_of_bounds.global_index == -1);

    // Negative index should return invalid markers
    auto negative = helper.get_slot_info(-5);
    REQUIRE(negative.slot_index == -1);
    REQUIRE(negative.global_index == -1);
}

// ============================================================================
// Not Running State Tests
// ============================================================================

TEST_CASE("ACE not running initially", "[ams][ace][state]") {
    AmsBackendAceTestHelper helper;
    REQUIRE(helper.is_running() == false);
}

TEST_CASE("ACE operations require API", "[ams][ace][preconditions]") {
    AmsBackendAceTestHelper helper;

    // Without API, operations should fail
    auto err = helper.load_filament(0);
    REQUIRE(!err.success());

    err = helper.unload_filament();
    REQUIRE(!err.success());

    err = helper.start_drying(45.0f, 240);
    REQUIRE(!err.success());
}
