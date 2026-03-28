// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"
#include "lvgl.h"
#include "printer_capabilities_state.h"
#include "printer_discovery.h"
#include "printer_temperature_state.h"
#include "settings_manager.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::PrinterCapabilitiesState;
using helix::PrinterDiscovery;
using helix::PrinterTemperatureState;

// 1. PrinterDiscovery stores chamber sensor name
TEST_CASE("PrinterDiscovery stores chamber sensor name", "[discovery][chamber]") {
    PrinterDiscovery discovery;
    nlohmann::json objects = {"temperature_sensor chamber", "extruder", "heater_bed"};
    discovery.parse_objects(objects);

    REQUIRE(discovery.has_chamber_sensor());
    REQUIRE(discovery.chamber_sensor_name() == "temperature_sensor chamber");
}

// 2. PrinterTemperatureState updates chamber temp from status
TEST_CASE("PrinterTemperatureState updates chamber temp from status", "[temperature][chamber]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState temp_state;
    temp_state.init_subjects(false); // No XML registration in tests
    temp_state.set_chamber_sensor_name("temperature_sensor chamber");

    nlohmann::json status = {{"temperature_sensor chamber", {{"temperature", 45.3}}}};
    temp_state.update_from_status(status);

    REQUIRE(lv_subject_get_int(temp_state.get_chamber_temp_subject()) == 453); // centidegrees
}

// 3. PrinterCapabilitiesState sets chamber sensor capability
TEST_CASE("PrinterCapabilitiesState sets chamber sensor capability", "[capabilities][chamber]") {
    LVGLTestFixture fixture;

    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    // Verify initial state before set_hardware()
    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_sensor_subject()) == 0);

    PrinterDiscovery hardware;
    nlohmann::json objects = {"temperature_sensor chamber"};
    hardware.parse_objects(objects);

    CapabilityOverrides overrides;
    caps.set_hardware(hardware, overrides);

    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_sensor_subject()) == 1);
}

// 4. No chamber sensor - capability is 0
TEST_CASE("PrinterCapabilitiesState reports no chamber sensor when absent",
          "[capabilities][chamber]") {
    LVGLTestFixture fixture;

    PrinterCapabilitiesState caps;
    caps.init_subjects(false);

    // Verify initial state before set_hardware()
    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_sensor_subject()) == 0);

    PrinterDiscovery hardware;
    nlohmann::json objects = {"extruder", "heater_bed"}; // No chamber
    hardware.parse_objects(objects);

    CapabilityOverrides overrides;
    caps.set_hardware(hardware, overrides);

    REQUIRE(lv_subject_get_int(caps.get_printer_has_chamber_sensor_subject()) == 0);
}

// 5. PrinterTemperatureState ignores chamber when sensor not configured
TEST_CASE("PrinterTemperatureState ignores chamber when sensor not configured",
          "[temperature][chamber]") {
    LVGLTestFixture fixture;

    PrinterTemperatureState temp_state;
    temp_state.init_subjects(false);
    // Note: set_chamber_sensor_name() NOT called

    nlohmann::json status = {{"temperature_sensor chamber", {{"temperature", 45.3}}}};
    temp_state.update_from_status(status);

    // Should remain at initial value (0)
    REQUIRE(lv_subject_get_int(temp_state.get_chamber_temp_subject()) == 0);
}

// 6. Chamber assignment settings default to "auto"
TEST_CASE("Chamber assignment settings default to auto", "[settings][chamber]") {
    LVGLTestFixture fixture;

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    REQUIRE(settings.get_chamber_heater_assignment() == "auto");
    REQUIRE(settings.get_chamber_sensor_assignment() == "auto");
}

// 7. Chamber assignment settings persist values
TEST_CASE("Chamber assignment settings persist values", "[settings][chamber]") {
    LVGLTestFixture fixture;

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    settings.set_chamber_heater_assignment("heater_generic my_chamber");
    REQUIRE(settings.get_chamber_heater_assignment() == "heater_generic my_chamber");

    settings.set_chamber_sensor_assignment("temperature_sensor enclosure_bme");
    REQUIRE(settings.get_chamber_sensor_assignment() == "temperature_sensor enclosure_bme");

    settings.set_chamber_heater_assignment("none");
    REQUIRE(settings.get_chamber_heater_assignment() == "none");

    settings.set_chamber_sensor_assignment("auto");
    REQUIRE(settings.get_chamber_sensor_assignment() == "auto");
}

// 8. Manual chamber sensor override takes precedence over auto-detection
TEST_CASE("Manual chamber sensor override", "[chamber][override]") {
    LVGLTestFixture fixture;

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    PrinterTemperatureState temp_state;
    temp_state.init_subjects(false);

    PrinterDiscovery discovery;
    nlohmann::json objects = {
        "temperature_sensor chamber",
        "temperature_sensor enclosure_bme",
        "extruder",
        "heater_bed"};
    discovery.parse_objects(objects);

    settings.set_chamber_sensor_assignment("temperature_sensor enclosure_bme");

    std::string resolved_sensor = settings.get_chamber_sensor_assignment();
    if (resolved_sensor == "auto") {
        resolved_sensor = discovery.chamber_sensor_name();
    } else if (resolved_sensor == "none") {
        resolved_sensor = "";
    }
    temp_state.set_chamber_sensor_name(resolved_sensor);

    nlohmann::json status = {{"temperature_sensor enclosure_bme", {{"temperature", 33.7}}}};
    temp_state.update_from_status(status);

    REQUIRE(lv_subject_get_int(temp_state.get_chamber_temp_subject()) == 337);

    settings.set_chamber_sensor_assignment("auto");
}

// 9. "none" disables chamber sensor even when auto would detect
TEST_CASE("Chamber sensor 'none' disables detection", "[chamber][override]") {
    LVGLTestFixture fixture;

    auto& settings = helix::SettingsManager::instance();
    settings.init_subjects();

    PrinterTemperatureState temp_state;
    temp_state.init_subjects(false);

    PrinterDiscovery discovery;
    nlohmann::json objects = {"temperature_sensor chamber", "extruder", "heater_bed"};
    discovery.parse_objects(objects);

    settings.set_chamber_sensor_assignment("none");

    std::string resolved_sensor = settings.get_chamber_sensor_assignment();
    if (resolved_sensor == "auto") {
        resolved_sensor = discovery.chamber_sensor_name();
    } else if (resolved_sensor == "none") {
        resolved_sensor = "";
    }
    temp_state.set_chamber_sensor_name(resolved_sensor);

    nlohmann::json status = {{"temperature_sensor chamber", {{"temperature", 45.3}}}};
    temp_state.update_from_status(status);

    REQUIRE(lv_subject_get_int(temp_state.get_chamber_temp_subject()) == 0);

    settings.set_chamber_sensor_assignment("auto");
}
