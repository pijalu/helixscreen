// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file bt_plugin.cpp
 * @brief Plugin core — C ABI exports for init/deinit/info/error.
 *
 * All exported functions use extern "C" to match bluetooth_plugin.h typedefs.
 * Logging uses fprintf(stderr) — NOT spdlog (plugin is a standalone .so).
 */

#include "bt_context.h"
#include "bluetooth_plugin.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Shared helper: MAC to D-Bus path
// ---------------------------------------------------------------------------

std::string mac_to_dbus_path(const char* mac)
{
    // "AA:BB:CC:DD:EE:FF" -> "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"
    std::string path = "/org/bluez/hci0/dev_";
    if (mac) {
        std::string m(mac);
        std::replace(m.begin(), m.end(), ':', '_');
        path += m;
    }
    return path;
}

// ---------------------------------------------------------------------------
// Plugin info
// ---------------------------------------------------------------------------

static helix_bt_plugin_info plugin_info = {
    .api_version = HELIX_BT_API_VERSION,
    .name = "helix-bluetooth",
    .has_classic = true,
    .has_ble = true,
};

extern "C" helix_bt_plugin_info* helix_bt_get_info(void)
{
    return &plugin_info;
}

// ---------------------------------------------------------------------------
// Init / Deinit
// ---------------------------------------------------------------------------

extern "C" helix_bt_context* helix_bt_init(void)
{
    auto* ctx = new (std::nothrow) helix_bt_context;
    if (!ctx) {
        fprintf(stderr, "[bt] failed to allocate context\n");
        return nullptr;
    }

    int r = sd_bus_open_system(&ctx->bus);
    if (r < 0) {
        fprintf(stderr, "[bt] failed to open system bus: %s\n", strerror(-r));
        delete ctx;
        return nullptr;
    }

    // Set a 5-second D-Bus method call timeout (default is 25 seconds which
    // freezes the UI when is_paired/is_connected checks are called synchronously)
    sd_bus_set_method_call_timeout(ctx->bus, 5 * 1000000ULL);  // microseconds

    fprintf(stderr, "[bt] plugin initialized (api_version=%d)\n", HELIX_BT_API_VERSION);
    return ctx;
}

extern "C" void helix_bt_deinit(helix_bt_context* ctx)
{
    if (!ctx) return;

    // Close BLE acquired fds
    {
        std::lock_guard<std::mutex> lock(ctx->ble_mutex);
        for (auto& conn : ctx->ble_connections) {
            if (conn.acquired_fd >= 0) {
                close(conn.acquired_fd);
                conn.acquired_fd = -1;
            }
            conn.active = false;
        }
        ctx->ble_connections.clear();
    }

    // Close tracked RFCOMM fds
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        for (int fd : ctx->rfcomm_fds) {
            close(fd);
        }
        ctx->rfcomm_fds.clear();
    }

    // Free discovery slot if still held
    if (ctx->discovery_slot) {
        sd_bus_slot_unref(ctx->discovery_slot);
        ctx->discovery_slot = nullptr;
    }

    // Close the bus
    if (ctx->bus) {
        sd_bus_flush_close_unref(ctx->bus);
        ctx->bus = nullptr;
    }

    fprintf(stderr, "[bt] plugin deinitialized\n");
    delete ctx;
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

extern "C" const char* helix_bt_last_error(helix_bt_context* ctx)
{
    if (!ctx) return "null context";
    std::lock_guard<std::mutex> lock(ctx->mutex);
    return ctx->last_error.c_str();
}
