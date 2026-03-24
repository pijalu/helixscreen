// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_state.h"
#include "ams_types.h"
#include "app_constants.h"
#include "config.h"
#include "settings_manager.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

using namespace helix;

// ============================================================================
// Helper: Initialize Config with a temp directory for isolated testing
// ============================================================================

namespace {

struct TempConfigFixture {
    std::string temp_dir;
    std::string config_path;

    TempConfigFixture() {
        temp_dir = std::filesystem::temp_directory_path().string() + "/helix_ext_spool_test_" +
                   std::to_string(rand());
        std::filesystem::create_directories(temp_dir);
        config_path = temp_dir + "/settings.json";

        // Remove backup files to prevent cross-test contamination.
        // Config::init() restores from backups when the config file is missing,
        // so stale backup data from a previous test run can leak into this test.
        std::filesystem::remove(AppConstants::Update::config_backup_fallback());
        std::filesystem::remove(AppConstants::Update::legacy_config_backup_fallback());
        std::filesystem::remove(AppConstants::Update::env_backup_fallback());

        // Initialize Config singleton with temp path
        Config::get_instance()->init(config_path);
    }

    ~TempConfigFixture() {
        std::filesystem::remove_all(temp_dir);
    }
};

} // namespace

// ============================================================================
// Step 1: SettingsManager external spool persistence
// ============================================================================

TEST_CASE("get_external_spool_info returns empty default when not set",
          "[external_spool][settings]") {
    TempConfigFixture fixture;

    auto result = SettingsManager::instance().get_external_spool_info();
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("set_external_spool_info stores and retrieves data", "[external_spool][settings]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.color_rgb = 0xFF0000;
    info.material = "PLA";
    info.brand = "eSUN";
    info.nozzle_temp_min = 200;
    info.nozzle_temp_max = 220;
    info.bed_temp = 60;
    info.spoolman_id = 42;
    info.spool_name = "My Spool";
    info.remaining_weight_g = 450;
    info.total_weight_g = 1000;

    settings.set_external_spool_info(info);

    auto result = settings.get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0xFF0000);
    CHECK(result->material == "PLA");
    CHECK(result->brand == "eSUN");
    CHECK(result->nozzle_temp_min == 200);
    CHECK(result->nozzle_temp_max == 220);
    CHECK(result->bed_temp == 60);
    CHECK(result->spoolman_id == 42);
    CHECK(result->spool_name == "My Spool");
    CHECK(result->remaining_weight_g == Catch::Approx(450.0f));
    CHECK(result->total_weight_g == Catch::Approx(1000.0f));
}

TEST_CASE("set_external_spool_info persists across config reload", "[external_spool][settings]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.color_rgb = 0x00FF00;
    info.material = "PETG";
    info.brand = "Polymaker";
    info.nozzle_temp_min = 230;
    info.nozzle_temp_max = 250;
    info.bed_temp = 80;
    info.spoolman_id = 99;
    info.spool_name = "Test Spool";
    info.remaining_weight_g = 800;
    info.total_weight_g = 1000;

    settings.set_external_spool_info(info);

    // Reload config from disk
    Config::get_instance()->init(fixture.config_path);

    auto result = settings.get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0x00FF00);
    CHECK(result->material == "PETG");
    CHECK(result->brand == "Polymaker");
    CHECK(result->spoolman_id == 99);
}

TEST_CASE("clear_external_spool_info removes stored data", "[external_spool][settings]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.color_rgb = 0xFF0000;
    info.material = "PLA";

    settings.set_external_spool_info(info);
    REQUIRE(settings.get_external_spool_info().has_value());

    settings.clear_external_spool_info();
    REQUIRE_FALSE(settings.get_external_spool_info().has_value());
}

TEST_CASE("external spool slot_index is always -2", "[external_spool][settings]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.slot_index = 5; // Pass in a non-sentinel value
    info.color_rgb = 0xFF0000;
    info.material = "PLA";

    settings.set_external_spool_info(info);

    auto result = settings.get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->slot_index == -2);
    CHECK(result->global_index == -2);
}

TEST_CASE("get_external_spool_info with assigned=true returns spool even with black color",
          "[external_spool][settings]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.color_rgb = 0x000000; // Black — previously could fail with -1 sentinel
    info.material = "PLA";

    settings.set_external_spool_info(info);

    auto result = settings.get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0x000000);
    CHECK(result->material == "PLA");
}

TEST_CASE("backward compat: old config without assigned key but with color_rgb",
          "[external_spool][settings]") {
    TempConfigFixture fixture;

    // Manually write old-format config (no "assigned" key, but with color_rgb).
    // Set in-memory only (no save) to avoid contaminating the global backup file.
    Config* config = Config::get_instance();
    std::string prefix = config->df();
    config->set<int>(prefix + "filament/external_spool/color_rgb", 0xFF0000);
    config->set<std::string>(prefix + "filament/external_spool/material", "PETG");

    auto result = SettingsManager::instance().get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0xFF0000);
    CHECK(result->material == "PETG");
}

// ============================================================================
// Step 2: AmsState external spool subject and get/set
// ============================================================================

TEST_CASE("AmsState get_external_spool_info delegates to SettingsManager",
          "[external_spool][ams_state]") {
    TempConfigFixture fixture;
    auto& settings = SettingsManager::instance();

    SlotInfo info;
    info.color_rgb = 0xAABBCC;
    info.material = "ABS";
    info.brand = "Hatchbox";
    settings.set_external_spool_info(info);

    auto result = AmsState::instance().get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0xAABBCC);
    CHECK(result->material == "ABS");
    CHECK(result->brand == "Hatchbox");
}

TEST_CASE("AmsState set_external_spool_info writes to SettingsManager",
          "[external_spool][ams_state]") {
    TempConfigFixture fixture;

    SlotInfo info;
    info.color_rgb = 0x112233;
    info.material = "TPU";
    info.brand = "NinjaTek";

    AmsState::instance().set_external_spool_info(info);

    auto result = SettingsManager::instance().get_external_spool_info();
    REQUIRE(result.has_value());
    CHECK(result->color_rgb == 0x112233);
    CHECK(result->material == "TPU");
    CHECK(result->brand == "NinjaTek");
}

TEST_CASE("AmsState external_spool_color subject updates on set", "[external_spool][ams_state]") {
    TempConfigFixture fixture;
    auto& ams = AmsState::instance();
    ams.init_subjects(false); // false = skip XML registration (no LVGL display)

    SlotInfo info;
    info.color_rgb = 0xDDEEFF;
    info.material = "PLA";

    ams.set_external_spool_info(info);

    int color = lv_subject_get_int(ams.get_external_spool_color_subject());
    CHECK(color == static_cast<int>(0xDDEEFF));
}

TEST_CASE("AmsState external_spool_color subject defaults to 0 when no spool",
          "[external_spool][ams_state]") {
    TempConfigFixture fixture;
    auto& ams = AmsState::instance();
    ams.init_subjects(false);

    // Clear any state from previous tests (singleton persists across test cases)
    ams.clear_external_spool_info();

    int color = lv_subject_get_int(ams.get_external_spool_color_subject());
    CHECK(color == 0);
}
