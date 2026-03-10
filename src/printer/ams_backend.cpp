// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend.h"

#include "ams_backend_afc.h"
#include "ams_backend_happy_hare.h"
#ifdef HELIX_ENABLE_MOCKS
#include "ams_backend_mock.h"
#endif
#include "ams_backend_ad5x_ifs.h"
#include "ams_backend_toolchanger.h"
#include "ams_backend_valgace.h"
#include "moonraker_api.h"
#include "runtime_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

using namespace helix;

#ifdef HELIX_ENABLE_MOCKS
// Helper: lowercase a string for case-insensitive comparison
static std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

// Helper to create mock backend with optional features
static std::unique_ptr<AmsBackendMock> create_mock_with_features(int gate_count) {
    auto mock = std::make_unique<AmsBackendMock>(gate_count);

    // ========================================================================
    // HELIX_MOCK_AMS — topology/type selection
    // ========================================================================
    const char* mock_ams_env = std::getenv("HELIX_MOCK_AMS");
    std::string ams_type;

    if (mock_ams_env) {
        ams_type = to_lower(mock_ams_env);
    }

    if (!ams_type.empty()) {
        if (ams_type == "afc" || ams_type == "box_turtle" || ams_type == "boxturtle") {
            mock->set_afc_mode(true);
            spdlog::info("[AMS Backend] Mock AFC mode enabled");
        } else if (ams_type == "toolchanger" || ams_type == "tool_changer" || ams_type == "tc") {
            mock->set_tool_changer_mode(true);
            spdlog::info("[AMS Backend] Mock tool changer mode enabled");
        } else if (ams_type == "mixed") {
            mock->set_mixed_topology_mode(true);
            spdlog::info("[AMS Backend] Mock mixed topology mode enabled");
        } else if (ams_type == "multi") {
            mock->set_multi_unit_mode(true);
            spdlog::info("[AMS Backend] Mock multi-unit mode enabled");
        } else if (ams_type == "vivid") {
            mock->set_vivid_mixed_mode(true);
            spdlog::info("[AMS Backend] Mock ViViD mixed mode enabled");
        } else if (ams_type == "ifs" || ams_type == "ad5x" || ams_type == "ad5x_ifs") {
            mock->set_ifs_mode(true);
            spdlog::info("[AMS Backend] Mock AD5X IFS mode enabled");
        }
    }

    // ========================================================================
    // HELIX_MOCK_AMS_STATE — visual scenario
    // ========================================================================
    const char* mock_state_env = std::getenv("HELIX_MOCK_AMS_STATE");
    std::string state_scenario;

    if (mock_state_env) {
        state_scenario = to_lower(mock_state_env);
    }

    if (!state_scenario.empty() && state_scenario != "idle") {
        // All non-idle scenarios are applied after start() for consistency.
        // loading/bypass: require running_=true (use interruptible sleep + thread)
        // error: applied directly in start() (no thread needed, but deferred for uniformity)
        mock->set_initial_state_scenario(state_scenario);
        spdlog::info("[AMS Backend] Mock state scenario: {}", state_scenario);
    }

    // ========================================================================
    // Orthogonal features (kept separate)
    // ========================================================================

    // Enable mock dryer by default (disable with HELIX_MOCK_DRYER=0)
    const char* dryer_env = std::getenv("HELIX_MOCK_DRYER");
    bool dryer_enabled = !dryer_env || (std::string(dryer_env) != "0" && std::string(dryer_env) != "false");
    if (dryer_enabled) {
        mock->set_dryer_enabled(true);
        spdlog::info("[AMS Backend] Mock dryer enabled");
    }

    // Simulate mid-print tool change progress (3rd of 5 swaps) for visual testing
    mock->set_toolchange_progress(2, 5);

    return mock;
}

// Check if mock mode is requested and not explicitly disabled via HELIX_MOCK_AMS=none
static std::unique_ptr<AmsBackend> try_create_mock() {
    const auto* config = get_runtime_config();
    if (!config->should_mock_ams()) {
        return nullptr;
    }

    const char* mock_ams_env = std::getenv("HELIX_MOCK_AMS");
    if (mock_ams_env && to_lower(mock_ams_env) == "none") {
        spdlog::info("[AMS Backend] Mock AMS disabled via HELIX_MOCK_AMS=none");
        return nullptr;
    }

    spdlog::debug("[AMS Backend] Creating mock backend with {} gates (mock mode enabled)",
                  config->mock_ams_gate_count);
    return create_mock_with_features(config->mock_ams_gate_count);
}
#endif

std::unique_ptr<AmsBackend> AmsBackend::create(AmsType detected_type) {
#ifdef HELIX_ENABLE_MOCKS
    const auto* config = get_runtime_config();
    if (auto mock = try_create_mock()) {
        return mock;
    }
#endif

    // Without API/client dependencies, we can only return mock backends
    switch (detected_type) {
    case AmsType::HAPPY_HARE:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] Happy Hare detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] Happy Hare detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::AFC:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] AFC detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] AFC detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::VALGACE:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] ValgACE detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] ValgACE detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::TOOL_CHANGER:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] Tool changer detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] Tool changer detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::AD5X_IFS:
#ifdef HELIX_ENABLE_MOCKS
        spdlog::warn("[AMS Backend] AD5X IFS detected but no API/client provided - using mock");
        return std::make_unique<AmsBackendMock>(config->mock_ams_gate_count);
#else
        spdlog::warn("[AMS Backend] AD5X IFS detected but no API/client provided");
        return nullptr;
#endif

    case AmsType::NONE:
    default:
        spdlog::debug("[AMS Backend] No AMS detected");
        return nullptr;
    }
}

std::unique_ptr<AmsBackend> AmsBackend::create(AmsType detected_type, MoonrakerAPI* api,
                                               MoonrakerClient* client) {
#ifdef HELIX_ENABLE_MOCKS
    if (auto mock = try_create_mock()) {
        return mock;
    }
#endif

    switch (detected_type) {
    case AmsType::HAPPY_HARE:
        if (!api || !client) {
            spdlog::error("[AMS Backend] Happy Hare requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating Happy Hare backend");
        return std::make_unique<AmsBackendHappyHare>(api, client);

    case AmsType::AFC:
        if (!api || !client) {
            spdlog::error("[AMS Backend] AFC requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating AFC backend");
        return std::make_unique<AmsBackendAfc>(api, client);

    case AmsType::VALGACE:
        if (!api || !client) {
            spdlog::error("[AMS Backend] ValgACE requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating ValgACE backend");
        return std::make_unique<AmsBackendValgACE>(api, client);

    case AmsType::TOOL_CHANGER:
        if (!api || !client) {
            spdlog::error("[AMS Backend] Tool changer requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating Tool Changer backend");
        // Note: Caller must use set_discovered_tools() after creation to set tool names
        return std::make_unique<AmsBackendToolChanger>(api, client);

    case AmsType::AD5X_IFS:
        if (!api || !client) {
            spdlog::error("[AMS Backend] AD5X IFS requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating AD5X IFS backend");
        return std::make_unique<AmsBackendAd5xIfs>(api, client);

    case AmsType::NONE:
    default:
        spdlog::debug("[AMS Backend] No AMS detected");
        return nullptr;
    }
}
