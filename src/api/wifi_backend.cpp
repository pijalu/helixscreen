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
    // Linux: pick between NetworkManager and wpa_supplicant using CHEAP
    // probes (no subprocess, no socket I/O). The actual initialization
    // happens in the caller via start_async().
    //
    // Committing to NM requires BOTH the nmcli binary AND a liveness signal
    // for the NM daemon — its runtime directory. Vanilla Pi OS (and
    // Klipper-derived images like RatOS) ship nmcli in the base image but
    // run dhcpcd + wpa_supplicant as the active network stack, so binary
    // presence alone is not sufficient. NM creates /run/NetworkManager when
    // active and removes it when masked/stopped, making a filesystem probe
    // a reliable proxy without forking a subprocess.
    const bool has_nmcli = (access("/usr/bin/nmcli", X_OK) == 0) ||
                           (access("/bin/nmcli", X_OK) == 0) ||
                           (access("/usr/local/bin/nmcli", X_OK) == 0);
    const bool nm_daemon_active = (access("/run/NetworkManager", F_OK) == 0) ||
                                  (access("/var/run/NetworkManager", F_OK) == 0);

    if (has_nmcli && nm_daemon_active) {
        spdlog::debug(
            "[WifiBackend] Selecting NetworkManager backend (nmcli + /run/NetworkManager){}",
            silent ? " (silent)" : "");
        auto backend = std::make_unique<WifiBackendNetworkManager>();
        backend->set_silent(silent);
        return backend;
    }

    // Default to wpa_supplicant: either nmcli is missing, or it's installed
    // but the NM daemon is inactive (the common Pi / RatOS case). Going
    // straight here avoids the NM→wpa async fallback, which silently fails
    // when the shared WiFiManager is constructed with silent=true.
    spdlog::debug("[WifiBackend] Selecting wpa_supplicant backend "
                  "(nmcli={}, nm_daemon_active={}){}",
                  has_nmcli, nm_daemon_active, silent ? " (silent)" : "");
    auto backend = std::make_unique<WifiBackendWpaSupplicant>();
    backend->set_silent(silent);
    return backend;
#endif
}
