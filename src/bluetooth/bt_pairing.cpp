// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file bt_pairing.cpp
 * @brief BlueZ device pairing via D-Bus.
 *
 * Implements helix_bt_pair() and helix_bt_is_paired() using
 * org.bluez.Device1.Pair() and the Device1.Paired property.
 *
 * Most label printers use "Just Works" pairing (no PIN required).
 */

#include "bt_context.h"
#include "bluetooth_plugin.h"

#include <cstdio>
#include <cstring>

extern "C" int helix_bt_pair(helix_bt_context* ctx, const char* mac)
{
    if (!ctx) return -EINVAL;
    if (!mac) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "null MAC address";
        return -EINVAL;
    }
    if (!ctx->bus) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "bus not initialized";
        return -ENODEV;
    }

    std::string path = mac_to_dbus_path(mac);
    fprintf(stderr, "[bt] pairing with %s (%s)\n", mac, path.c_str());

    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(ctx->bus,
                               "org.bluez",
                               path.c_str(),
                               "org.bluez.Device1",
                               "Pair",
                               &error,
                               nullptr,
                               "");
    if (r < 0) {
        // AlreadyExists means already paired — treat as success
        if (sd_bus_error_has_name(&error, "org.bluez.Error.AlreadyExists")) {
            fprintf(stderr, "[bt] device %s already paired\n", mac);
            sd_bus_error_free(&error);
            return 0;
        }

        fprintf(stderr, "[bt] pair failed for %s: %s\n", mac,
                error.message ? error.message : strerror(-r));
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            ctx->last_error = error.message ? error.message : "pairing failed";
        }
        sd_bus_error_free(&error);
        return r;
    }

    sd_bus_error_free(&error);
    fprintf(stderr, "[bt] paired successfully with %s\n", mac);
    return 0;
}

extern "C" int helix_bt_is_paired(helix_bt_context* ctx, const char* mac)
{
    if (!ctx) return -EINVAL;
    if (!mac) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "null MAC address";
        return -EINVAL;
    }
    if (!ctx->bus) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "bus not initialized";
        return -ENODEV;
    }

    std::string path = mac_to_dbus_path(mac);

    sd_bus_error error = SD_BUS_ERROR_NULL;
    int paired = 0;
    int r = sd_bus_get_property_trivial(ctx->bus,
                                         "org.bluez",
                                         path.c_str(),
                                         "org.bluez.Device1",
                                         "Paired",
                                         &error,
                                         'b',
                                         &paired);
    if (r < 0) {
        fprintf(stderr, "[bt] is_paired check failed for %s: %s\n", mac,
                error.message ? error.message : strerror(-r));
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            ctx->last_error = error.message ? error.message : "failed to read Paired property";
        }
        sd_bus_error_free(&error);
        return r;
    }

    sd_bus_error_free(&error);
    return paired ? 1 : 0;
}
