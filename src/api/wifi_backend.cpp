// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_backend.h"

#include "runtime_config.h"
#include "spdlog/spdlog.h"
#ifdef HELIX_ENABLE_MOCKS
#include "wifi_backend_mock.h"
#endif

#ifdef __APPLE__
#include "wifi_backend_macos.h"
#elif !defined(__ANDROID__)
#include "wifi_backend_networkmanager.h"
#include "wifi_backend_wpa_supplicant.h"
#endif

std::unique_ptr<WifiBackend> WifiBackend::create(bool silent) {
    // In test mode, always use mock unless --real-wifi was specified
#ifdef HELIX_ENABLE_MOCKS
    if (get_runtime_config()->should_mock_wifi()) {
        spdlog::debug("[WifiBackend] Test mode: using mock backend");
        auto mock = std::make_unique<WifiBackendMock>();
        mock->set_silent(silent);
        mock->start();
        return mock;
    }
#endif

#ifdef __APPLE__
    // macOS: Try CoreWLAN backend
    spdlog::debug("[WifiBackend] Attempting CoreWLAN backend for macOS");
    auto backend = std::make_unique<WifiBackendMacOS>();
    backend->set_silent(silent);
    WiFiError start_result = backend->start();

    if (start_result.success()) {
        spdlog::info("[WifiBackend] CoreWLAN backend started successfully");
        return backend;
    }

    // In production mode, don't fallback to mock - WiFi is simply unavailable
    spdlog::warn("[WifiBackend] CoreWLAN backend failed: {} - WiFi unavailable",
                 start_result.technical_msg);
    return nullptr;
#elif defined(__ANDROID__)
    // Android: WiFi managed by the OS, not by us
    spdlog::info("[WifiBackend] Android platform - WiFi not managed natively");
    return nullptr;
#else
    // Linux: Try NetworkManager first (fast probe, most distros use it)
    spdlog::debug("[WifiBackend] Attempting NetworkManager backend for Linux{}",
                  silent ? " (silent mode)" : "");
    auto nm_backend = std::make_unique<WifiBackendNetworkManager>();
    nm_backend->set_silent(true); // Silent during probe - we may fallback to wpa
    WiFiError nm_result = nm_backend->start();

    if (nm_result.success()) {
        spdlog::info("[WifiBackend] NetworkManager backend started successfully");
        return nm_backend;
    }

    spdlog::debug("[WifiBackend] NetworkManager failed: {} - trying wpa_supplicant",
                  nm_result.technical_msg);

    // Fallback: Try wpa_supplicant backend
    auto wpa_backend = std::make_unique<WifiBackendWpaSupplicant>();
    wpa_backend->set_silent(silent);
    WiFiError wpa_result = wpa_backend->start();

    if (wpa_result.success()) {
        spdlog::info("[WifiBackend] wpa_supplicant backend started successfully");
        return wpa_backend;
    }

    // Both backends failed
    spdlog::warn("[WifiBackend] All backends failed - WiFi unavailable");
    spdlog::warn("[WifiBackend]   NetworkManager: {}", nm_result.technical_msg);
    spdlog::warn("[WifiBackend]   wpa_supplicant: {}", wpa_result.technical_msg);
    return nullptr;
#endif
}