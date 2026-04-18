// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_state.h"
#include "app_constants.h"
#include "config.h"
#include "filament_database.h"
#include "settings_manager.h"

#include <cstdlib>
#include <filesystem>

#include "../catch_amalgamated.hpp"
#include "../lvgl_test_fixture.h"

using Catch::Matchers::WithinAbs;
using namespace helix;

TEST_CASE("length_to_weight_g: 1.75mm PLA", "[filament][conversion]") {
    // 1.75mm PLA at 1.24 g/cm^3 is the canonical ~2.98 g/m.
    float grams = filament::length_to_weight_g(1000.0f, 1.24f, 1.75f);
    REQUIRE_THAT(grams, WithinAbs(2.982f, 0.01f));
}

TEST_CASE("length_to_weight_g: 2.85mm PLA", "[filament][conversion]") {
    float grams = filament::length_to_weight_g(1000.0f, 1.24f, 2.85f);
    // 2.85mm cross-section is (2.85/1.75)^2 ≈ 2.65x bigger.
    REQUIRE_THAT(grams, WithinAbs(7.91f, 0.05f));
}

TEST_CASE("length_to_weight_g: zero length", "[filament][conversion]") {
    REQUIRE(filament::length_to_weight_g(0.0f, 1.24f, 1.75f) == 0.0f);
}

TEST_CASE("length_to_weight_g: zero density returns zero", "[filament][conversion]") {
    // Callers must pre-check density; the function returns 0 as a safe default
    // instead of propagating NaN/Inf.
    REQUIRE(filament::length_to_weight_g(100.0f, 0.0f, 1.75f) == 0.0f);
}

TEST_CASE("set_external_spool_info_in_memory does not write settings",
          "[filament][ams_state]") {
    // Set up an isolated config directory so settings writes don't leak.
    std::string temp_dir = std::filesystem::temp_directory_path().string() +
                           "/helix_fct_test_" + std::to_string(rand());
    std::filesystem::create_directories(temp_dir);
    std::filesystem::remove(AppConstants::Update::config_backup_fallback());
    std::filesystem::remove(AppConstants::Update::legacy_config_backup_fallback());
    std::filesystem::remove(AppConstants::Update::env_backup_fallback());
    Config::get_instance()->init(temp_dir + "/settings.json");
    SettingsManager::instance().clear_external_spool_info();

    LVGLTestFixture fx;
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    auto& settings = SettingsManager::instance();

    // Baseline: clear the external spool.
    ams.clear_external_spool_info();

    SlotInfo info;
    info.material = "PLA";
    info.remaining_weight_g = 750.0f;
    info.total_weight_g = 1000.0f;
    info.color_rgb = 0xFF0000;

    // In-memory write: subject fires, but settings.json record stays absent.
    ams.set_external_spool_info_in_memory(info);

    auto persisted = settings.get_external_spool_info();
    REQUIRE_FALSE(persisted.has_value());

    // Persistent write: now it lands in settings.
    ams.set_external_spool_info(info);
    persisted = settings.get_external_spool_info();
    REQUIRE(persisted.has_value());
    REQUIRE(persisted->remaining_weight_g == 750.0f);

    ams.clear_external_spool_info();
    std::filesystem::remove_all(temp_dir);
}
