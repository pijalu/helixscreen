// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/moonraker_types.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using json = nlohmann::json;

// Replicate the parsing logic from moonraker_api_controls.cpp::get_power_devices()
// to verify it handles Moonraker's actual response format (array of device objects).
static std::vector<PowerDevice> parse_power_devices(const json& j) {
    std::vector<PowerDevice> devices;
    if (j.contains("result") && j["result"].contains("devices")) {
        for (const auto& info : j["result"]["devices"]) {
            PowerDevice dev;
            dev.device = info.value("device", "");
            dev.type = info.value("type", "unknown");
            dev.status = info.value("status", "off");
            dev.locked_while_printing = info.value("locked_while_printing", false);
            if (!dev.device.empty()) {
                devices.push_back(dev);
            }
        }
    }
    return devices;
}

// ============================================================================
// Power Device Parsing Tests (prestonbrown/helixscreen#466)
// ============================================================================

TEST_CASE("Power device parsing uses device name from array elements",
          "[power][parsing][moonraker]") {
    // Moonraker returns devices as an array, not an object.
    // Each element has a "device" field with the device name.
    // Bug #466: code was using .items() which returns array indices as keys.
    json response = json::parse(R"({
        "result": {
            "devices": [
                {
                    "device": "Printer",
                    "status": "on",
                    "type": "homeassistant",
                    "locked_while_printing": true
                },
                {
                    "device": "Automatic Power Off",
                    "status": "off",
                    "type": "klipper_device",
                    "locked_while_printing": false
                },
                {
                    "device": "Cooldown after Print",
                    "status": "on",
                    "type": "klipper_device",
                    "locked_while_printing": false
                },
                {
                    "device": "Unload Filament after Print",
                    "status": "off",
                    "type": "klipper_device",
                    "locked_while_printing": false
                }
            ]
        }
    })");

    auto devices = parse_power_devices(response);

    REQUIRE(devices.size() == 4);

    // Device names must be the actual names, not array indices
    CHECK(devices[0].device == "Printer");
    CHECK(devices[0].type == "homeassistant");
    CHECK(devices[0].status == "on");
    CHECK(devices[0].locked_while_printing == true);

    CHECK(devices[1].device == "Automatic Power Off");
    CHECK(devices[1].type == "klipper_device");
    CHECK(devices[1].status == "off");
    CHECK(devices[1].locked_while_printing == false);

    CHECK(devices[2].device == "Cooldown after Print");
    CHECK(devices[3].device == "Unload Filament after Print");
}

TEST_CASE("Power device parsing handles empty device list", "[power][parsing][moonraker]") {
    json response = json::parse(R"({"result": {"devices": []}})");
    auto devices = parse_power_devices(response);
    REQUIRE(devices.empty());
}

TEST_CASE("Power device parsing skips entries with empty device name",
          "[power][parsing][moonraker]") {
    json response = json::parse(R"({
        "result": {
            "devices": [
                {"device": "valid_device", "status": "on", "type": "gpio"},
                {"status": "off", "type": "gpio"},
                {"device": "", "status": "off", "type": "gpio"}
            ]
        }
    })");

    auto devices = parse_power_devices(response);
    REQUIRE(devices.size() == 1);
    CHECK(devices[0].device == "valid_device");
}

TEST_CASE("Power device parsing handles missing result key", "[power][parsing][moonraker]") {
    json response = json::parse(R"({"error": "not found"})");
    auto devices = parse_power_devices(response);
    REQUIRE(devices.empty());
}
