// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file bt_discovery.cpp
 * @brief BlueZ D-Bus device discovery via sd-bus.
 *
 * Discovers Bluetooth devices advertising label printer services:
 *   - SPP UUID 00001101-0000-1000-8000-00805f9b34fb (Brother QL Classic)
 *   - Phomemo UUID 0000ff00-0000-1000-8000-00805f9b34fb (Phomemo BLE)
 *
 * The discover function blocks, running its own sd_bus_process loop.
 * No background thread is used.
 */

#include "bt_context.h"
#include "bluetooth_plugin.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// Label printer service UUIDs we filter for
static const char* SPP_UUID = "00001101-0000-1000-8000-00805f9b34fb";
static const char* PHOMEMO_SERVICE_UUID = "0000ff00-0000-1000-8000-00805f9b34fb";

/// State passed through the signal callback
struct discover_ctx {
    helix_bt_context* bt;
    helix_bt_discover_cb cb;
    void* user_data;
};

/// Check if a UUID string matches one of our target service UUIDs (case-insensitive prefix)
static bool is_label_printer_uuid(const char* uuid)
{
    if (!uuid) return false;
    // SPP
    if (strncasecmp(uuid, "00001101", 8) == 0) return true;
    // Phomemo BLE service
    if (strncasecmp(uuid, "0000ff00", 8) == 0) return true;
    return false;
}

/// Determine if a device is BLE based on its UUID
static bool uuid_is_ble(const char* uuid)
{
    if (!uuid) return false;
    return strncasecmp(uuid, "0000ff00", 8) == 0;
}

/// Parse a single device's properties from a D-Bus message iterator and invoke callback
/// if it matches a label printer UUID.
/// The message iterator should be positioned at the properties dict for
/// "org.bluez.Device1" interface.
static void parse_device_properties(sd_bus_message* msg, discover_ctx* dctx)
{
    int r;
    // We are inside a dict entry for an interface.
    // The properties are an array of dict entries: a{sv}
    r = sd_bus_message_enter_container(msg, 'a', "{sv}");
    if (r < 0) return;

    std::string address, name;
    bool paired = false;
    std::vector<std::string> uuids;

    while ((r = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
        const char* prop = nullptr;
        r = sd_bus_message_read(msg, "s", &prop);
        if (r < 0) {
            sd_bus_message_exit_container(msg);
            continue;
        }

        if (strcmp(prop, "Address") == 0) {
            const char* val = nullptr;
            r = sd_bus_message_enter_container(msg, 'v', "s");
            if (r >= 0) {
                sd_bus_message_read(msg, "s", &val);
                if (val) address = val;
                sd_bus_message_exit_container(msg);
            }
        } else if (strcmp(prop, "Name") == 0 || strcmp(prop, "Alias") == 0) {
            const char* val = nullptr;
            r = sd_bus_message_enter_container(msg, 'v', "s");
            if (r >= 0) {
                sd_bus_message_read(msg, "s", &val);
                if (val && name.empty()) name = val;
                sd_bus_message_exit_container(msg);
            }
        } else if (strcmp(prop, "Paired") == 0) {
            r = sd_bus_message_enter_container(msg, 'v', "b");
            if (r >= 0) {
                int val = 0;
                sd_bus_message_read(msg, "b", &val);
                paired = (val != 0);
                sd_bus_message_exit_container(msg);
            }
        } else if (strcmp(prop, "UUIDs") == 0) {
            r = sd_bus_message_enter_container(msg, 'v', "as");
            if (r >= 0) {
                r = sd_bus_message_enter_container(msg, 'a', "s");
                if (r >= 0) {
                    const char* val = nullptr;
                    while (sd_bus_message_read(msg, "s", &val) > 0) {
                        if (val) uuids.emplace_back(val);
                    }
                    sd_bus_message_exit_container(msg);
                }
                sd_bus_message_exit_container(msg);
            }
        } else {
            // Skip unknown properties
            sd_bus_message_skip(msg, "v");
        }

        sd_bus_message_exit_container(msg);  // dict entry "sv"
    }
    sd_bus_message_exit_container(msg);  // array "a{sv}"

    // Check if any UUID matches our label printer services
    for (const auto& uuid : uuids) {
        if (is_label_printer_uuid(uuid.c_str())) {
            helix_bt_device dev = {};
            dev.mac = address.c_str();
            dev.name = name.empty() ? address.c_str() : name.c_str();
            dev.paired = paired;
            dev.is_ble = uuid_is_ble(uuid.c_str());
            dev.service_uuid = uuid.c_str();

            if (dctx->cb) {
                dctx->cb(&dev, dctx->user_data);
            }
            break;  // One callback per device
        }
    }
}

/// Signal handler for org.freedesktop.DBus.ObjectManager.InterfacesAdded
static int on_interfaces_added(sd_bus_message* msg, void* userdata,
                               sd_bus_error* /*ret_error*/)
{
    auto* dctx = static_cast<discover_ctx*>(userdata);
    if (!dctx || !dctx->bt->discovering.load()) return 0;

    int r;
    const char* path = nullptr;
    r = sd_bus_message_read(msg, "o", &path);
    if (r < 0) return 0;

    // Enter interfaces dict: a{sa{sv}}
    r = sd_bus_message_enter_container(msg, 'a', "{sa{sv}}");
    if (r < 0) return 0;

    while ((r = sd_bus_message_enter_container(msg, 'e', "sa{sv}")) > 0) {
        const char* iface = nullptr;
        r = sd_bus_message_read(msg, "s", &iface);
        if (r < 0) {
            sd_bus_message_exit_container(msg);
            continue;
        }

        if (strcmp(iface, "org.bluez.Device1") == 0) {
            parse_device_properties(msg, dctx);
        } else {
            // Skip properties for other interfaces
            sd_bus_message_skip(msg, "a{sv}");
        }

        sd_bus_message_exit_container(msg);  // dict entry
    }
    sd_bus_message_exit_container(msg);  // array

    return 0;
}

/// Find the BlueZ adapter path (typically /org/bluez/hci0)
static std::string find_adapter_path(sd_bus* bus)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    int r = sd_bus_call_method(bus,
                               "org.bluez",
                               "/",
                               "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects",
                               &error,
                               &reply,
                               "");
    if (r < 0) {
        sd_bus_error_free(&error);
        return {};
    }

    std::string adapter_path;

    // Parse response: a{oa{sa{sv}}}
    r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    if (r < 0) goto done;

    while ((r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
        const char* path = nullptr;
        r = sd_bus_message_read(reply, "o", &path);
        if (r < 0) {
            sd_bus_message_exit_container(reply);
            continue;
        }

        // Check interfaces for Adapter1
        r = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
        if (r < 0) {
            sd_bus_message_exit_container(reply);
            continue;
        }

        while ((r = sd_bus_message_enter_container(reply, 'e', "sa{sv}")) > 0) {
            const char* iface = nullptr;
            r = sd_bus_message_read(reply, "s", &iface);
            if (r >= 0 && iface && strcmp(iface, "org.bluez.Adapter1") == 0) {
                adapter_path = path;
            }
            sd_bus_message_skip(reply, "a{sv}");
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);  // interfaces array
        sd_bus_message_exit_container(reply);  // object entry

        if (!adapter_path.empty()) break;
    }
    sd_bus_message_exit_container(reply);  // top-level array

done:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return adapter_path;
}

/// Enumerate already-known devices from BlueZ's object manager
static void enumerate_known_devices(sd_bus* bus, discover_ctx* dctx)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;

    int r = sd_bus_call_method(bus,
                               "org.bluez",
                               "/",
                               "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects",
                               &error,
                               &reply,
                               "");
    if (r < 0) {
        sd_bus_error_free(&error);
        return;
    }

    // Parse: a{oa{sa{sv}}}
    r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
    if (r < 0) goto done;

    while ((r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
        const char* path = nullptr;
        r = sd_bus_message_read(reply, "o", &path);
        if (r < 0) {
            sd_bus_message_exit_container(reply);
            continue;
        }

        // Check interfaces for Device1
        r = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
        if (r < 0) {
            sd_bus_message_exit_container(reply);
            continue;
        }

        while ((r = sd_bus_message_enter_container(reply, 'e', "sa{sv}")) > 0) {
            const char* iface = nullptr;
            r = sd_bus_message_read(reply, "s", &iface);
            if (r >= 0 && iface && strcmp(iface, "org.bluez.Device1") == 0) {
                parse_device_properties(reply, dctx);
            } else {
                sd_bus_message_skip(reply, "a{sv}");
            }
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);  // interfaces array
        sd_bus_message_exit_container(reply);  // object entry
    }
    sd_bus_message_exit_container(reply);

done:
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" int helix_bt_discover(helix_bt_context* ctx, int timeout_ms,
                                  helix_bt_discover_cb cb, void* user_data)
{
    if (!ctx) return -EINVAL;
    if (!ctx->bus) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "bus not initialized";
        return -ENODEV;
    }

    // Find adapter
    std::string adapter = find_adapter_path(ctx->bus);
    if (adapter.empty()) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "no BlueZ adapter found";
        return -ENODEV;
    }

    fprintf(stderr, "[bt] using adapter: %s\n", adapter.c_str());

    // Heap-allocate discover context (freed on all exit paths)
    auto* dctx = new (std::nothrow) discover_ctx{ctx, cb, user_data};
    if (!dctx) {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "failed to allocate discovery context";
        return -ENOMEM;
    }

    ctx->discovering.store(true);

    // Enumerate already-known devices first
    enumerate_known_devices(ctx->bus, dctx);

    // Add InterfacesAdded signal match
    int r = sd_bus_match_signal(ctx->bus,
                                &ctx->discovery_slot,
                                "org.bluez",
                                "/",
                                "org.freedesktop.DBus.ObjectManager",
                                "InterfacesAdded",
                                on_interfaces_added,
                                dctx);
    if (r < 0) {
        fprintf(stderr, "[bt] failed to add signal match: %s\n", strerror(-r));
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->last_error = "failed to add InterfacesAdded match";
        ctx->discovering.store(false);
        delete dctx;
        return r;
    }

    // Start discovery
    sd_bus_error error = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(ctx->bus,
                           "org.bluez",
                           adapter.c_str(),
                           "org.bluez.Adapter1",
                           "StartDiscovery",
                           &error,
                           nullptr,
                           "");
    if (r < 0) {
        // InProgress is acceptable — discovery may already be running
        if (!sd_bus_error_has_name(&error, "org.bluez.Error.InProgress")) {
            fprintf(stderr, "[bt] StartDiscovery failed: %s\n",
                    error.message ? error.message : strerror(-r));
            std::lock_guard<std::mutex> lock(ctx->mutex);
            ctx->last_error = error.message ? error.message : "StartDiscovery failed";
            sd_bus_error_free(&error);
            sd_bus_slot_unref(ctx->discovery_slot);
            ctx->discovery_slot = nullptr;
            ctx->discovering.store(false);
            delete dctx;
            return r;
        }
    }
    sd_bus_error_free(&error);

    fprintf(stderr, "[bt] discovery started (timeout=%dms)\n", timeout_ms);

    // Poll loop: sd_bus_process + sd_bus_wait
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (ctx->discovering.load()) {
        r = sd_bus_process(ctx->bus, nullptr);
        if (r < 0) {
            fprintf(stderr, "[bt] sd_bus_process error: %s\n", strerror(-r));
            break;
        }
        if (r > 0) continue;  // More to process

        // Check timeout
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                          (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= timeout_ms) {
            fprintf(stderr, "[bt] discovery timeout reached (%ldms)\n", elapsed_ms);
            break;
        }

        // Wait for more events (up to 100ms to remain responsive to stop requests)
        long remaining = timeout_ms - elapsed_ms;
        uint64_t wait_us = (remaining > 100 ? 100 : remaining) * 1000ULL;
        sd_bus_wait(ctx->bus, wait_us);
    }

    // Stop discovery
    sd_bus_error error2 = SD_BUS_ERROR_NULL;
    sd_bus_call_method(ctx->bus,
                       "org.bluez",
                       adapter.c_str(),
                       "org.bluez.Adapter1",
                       "StopDiscovery",
                       &error2,
                       nullptr,
                       "");
    sd_bus_error_free(&error2);

    // Cleanup
    if (ctx->discovery_slot) {
        sd_bus_slot_unref(ctx->discovery_slot);
        ctx->discovery_slot = nullptr;
    }
    ctx->discovering.store(false);
    delete dctx;

    fprintf(stderr, "[bt] discovery finished\n");
    return 0;
}

extern "C" void helix_bt_stop_discovery(helix_bt_context* ctx)
{
    if (!ctx) return;
    ctx->discovering.store(false);
}
