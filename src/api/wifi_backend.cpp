// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "wifi_backend.h"

#include "runtime_config.h"
#include "spdlog/spdlog.h"

#include <unistd.h>
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
        // Non-blocking: mock fires READY immediately from start_async().
        // We intentionally do NOT call it here — the test case explicitly
        // invokes start_async() after registering its READY callback. For
        // production callers, WiFiManager will call start_async() after
        // registering its own event handlers.
        return mock;
    }
#endif

#ifdef __APPLE__
    // macOS: Construct CoreWLAN backend and return immediately. The
    // base-class default start_async() falls back to a synchronous start()
    // — acceptable on macOS until we port the async pattern there.
    spdlog::debug("[WifiBackend] Constructing CoreWLAN backend for macOS");
    auto backend = std::make_unique<WifiBackendMacOS>();
    backend->set_silent(silent);
    return backend;
#elif defined(__ANDROID__)
    // Android: WiFi managed by the OS, not by us
    spdlog::info("[WifiBackend] Android platform - WiFi not managed natively");
    return nullptr;
#else
    // Linux: pick between NetworkManager and wpa_supplicant using a CHEAP
    // file-existence probe (no subprocess, no socket I/O). The actual
    // initialization happens in the caller via start_async().
    const bool has_nmcli = (access("/usr/bin/nmcli", X_OK) == 0) ||
                           (access("/bin/nmcli", X_OK) == 0) ||
                           (access("/usr/local/bin/nmcli", X_OK) == 0);

    if (has_nmcli) {
        spdlog::debug(
            "[WifiBackend] Selecting NetworkManager backend (nmcli available){}",
            silent ? " (silent)" : "");
        auto backend = std::make_unique<WifiBackendNetworkManager>();
        backend->set_silent(silent);
        return backend;
    }

    // No nmcli — fall back to wpa_supplicant. Its start_async() will fire
    // INIT_FAILED if sockets are missing too; callers decide what to do.
    spdlog::debug("[WifiBackend] Selecting wpa_supplicant backend (no nmcli){}",
                  silent ? " (silent)" : "");
    auto backend = std::make_unique<WifiBackendWpaSupplicant>();
    backend->set_silent(silent);
    return backend;
#endif
}
