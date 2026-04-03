// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"

#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

namespace fs = std::filesystem;
using namespace helix;

namespace helix {
class PresetConfigFixture {
  protected:
    Config config;
    std::string temp_dir;

    void SetUp() {
        // Create temp directory for test config and presets
        temp_dir = (fs::temp_directory_path() / "helix_preset_test").string();
        fs::create_directories(temp_dir + "/presets");

        // Set config path so apply_preset_file can find presets/ relative to it
        config.path = temp_dir + "/settings.json";

        // Set active printer ID so df() returns the correct prefix
        config.active_printer_id_ = "default";

        // Initialize with a v4 multi-printer structure
        config.data = {
            {"preset", "ad5m"},
            {"language", "de"},
            {"active_printer_id", "default"},
            {"display", {{"animations_enabled", false}}},
            {"printers",
             {{"default",
               {{"moonraker_host", "127.0.0.1"},
                {"moonraker_port", 7125},
                {"printer_name", "My Printer"},
                {"wizard_completed", false},
                {"fans", {{"hotend", "heater_fan heat_fan"}, {"part", "fan_generic fanM106"}}},
                {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
                {"temp_sensors", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
                {"hardware", {{"expected", {"heater_bed", "extruder"}}}}}}}}};
    }

    void TearDown() {
        fs::remove_all(temp_dir);
    }

    void write_preset(const std::string& name, const nlohmann::json& preset_json) {
        std::string path = temp_dir + "/presets/" + name + ".json";
        std::ofstream f(path);
        f << preset_json.dump(2);
    }

    json& printer_data() {
        return config.data["printers"]["default"];
    }

    json& data() {
        return config.data;
    }
};
} // namespace helix

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file merges hardware keys into active printer",
                 "[config][preset]") {
    SetUp();

    json preset = {{"printer",
                    {{"fans",
                      {{"hotend", "heater_fan heat_fan"},
                       {"part", "fan_generic fanM106"},
                       {"chamber", "fan_generic chamber_fan"},
                       {"exhaust", "fan_generic exhaust_fan"}}},
                     {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
                     {"temp_sensors", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
                     {"leds", {{"strip", "neopixel led_strip"}}},
                     {"hardware",
                      {{"expected",
                        {"heater_bed", "extruder", "neopixel led_strip", "fan_generic chamber_fan",
                         "fan_generic exhaust_fan"}}}},
                     {"filament_sensors", {{"runout", "filament_switch_sensor runout"}}},
                     {"default_macros", {"START_PRINT", "END_PRINT"}}}}};
    write_preset("ad5m_pro", preset);

    REQUIRE(config.apply_preset_file("ad5m_pro") == true);

    auto& pd = printer_data();
    REQUIRE(pd["fans"].contains("chamber"));
    REQUIRE(pd["fans"].contains("exhaust"));
    REQUIRE(pd["leds"].contains("strip"));
    REQUIRE(pd["hardware"]["expected"].size() == 5);
    REQUIRE(pd.contains("filament_sensors"));
    REQUIRE(pd.contains("default_macros"));

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture, "Config::apply_preset_file preserves non-hardware settings",
                 "[config][preset]") {
    SetUp();

    json preset = {{"printer",
                    {{"fans", {{"hotend", "heater_fan heat_fan"}}},
                     {"heaters", {{"bed", "heater_bed"}}},
                     {"temp_sensors", {{"bed", "heater_bed"}}}}}};
    write_preset("minimal", preset);

    config.apply_preset_file("minimal");

    REQUIRE(data()["language"] == "de");
    auto& pd = printer_data();
    REQUIRE(pd["moonraker_host"] == "127.0.0.1");
    REQUIRE(pd["moonraker_port"] == 7125);
    REQUIRE(pd["printer_name"] == "My Printer");
    REQUIRE(pd["wizard_completed"] == false);

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture, "Config::apply_preset_file skips merge when wizard completed",
                 "[config][preset]") {
    SetUp();

    printer_data()["wizard_completed"] = true;

    json preset = {{"printer",
                    {{"fans",
                      {{"hotend", "heater_fan heat_fan"},
                       {"part", "fan_generic fanM106"},
                       {"chamber", "fan_generic chamber_fan"}}}}}};
    write_preset("ad5m_pro", preset);

    REQUIRE(config.apply_preset_file("ad5m_pro") == false);
    REQUIRE_FALSE(printer_data()["fans"].contains("chamber"));

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture, "Config::apply_preset_file returns false for missing file",
                 "[config][preset]") {
    SetUp();

    REQUIRE(config.apply_preset_file("nonexistent_preset") == false);
    REQUIRE(printer_data()["fans"].size() == 2);

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture, "Config::apply_preset_file erases keys not present in preset",
                 "[config][preset]") {
    SetUp();

    // Add leds to initial config
    printer_data()["leds"] = {{"strip", "neopixel led_strip"}};

    // Write preset WITHOUT leds
    json preset = {{"printer",
                    {{"fans", {{"hotend", "heater_fan heat_fan"}}},
                     {"heaters", {{"bed", "heater_bed"}}},
                     {"temp_sensors", {{"bed", "heater_bed"}}}}}};
    write_preset("no_leds", preset);

    config.apply_preset_file("no_leds");

    REQUIRE_FALSE(printer_data().contains("leds"));

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture,
                 "Config::apply_preset_file merges display settings from preset",
                 "[config][preset]") {
    SetUp();

    json preset = {{"printer",
                    {{"fans", {{"hotend", "heater_fan heat_fan"}}},
                     {"heaters", {{"bed", "heater_bed"}}},
                     {"temp_sensors", {{"bed", "heater_bed"}}}}},
                   {"display",
                    {{"backlight_enable_ioctl", true},
                     {"hardware_blank", true},
                     {"sleep_backlight_off", true}}}};
    write_preset("display_preset", preset);

    config.apply_preset_file("display_preset");

    REQUIRE(data()["display"]["backlight_enable_ioctl"] == true);
    REQUIRE(data()["display"]["hardware_blank"] == true);
    REQUIRE(data()["display"]["sleep_backlight_off"] == true);
    // Existing display setting should be preserved
    REQUIRE(data()["display"]["animations_enabled"] == false);

    TearDown();
}

TEST_CASE_METHOD(PresetConfigFixture, "Config::apply_preset_file integrates with detection flow",
                 "[config][preset][integration]") {
    SetUp();

    // Simulate what auto_detect_and_save does: set preset then apply
    write_preset(
        "ad5m_pro",
        {{"preset", "ad5m_pro"},
         {"printer",
          {{"fans",
            {{"chamber", "fan_generic chamber_fan"},
             {"exhaust", "fan_generic external_fan"},
             {"hotend", "heater_fan heat_fan"},
             {"part", "fan_generic fanM106"},
             {"aux", "fan_generic internal_fan"}}},
           {"heaters", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
           {"temp_sensors", {{"bed", "heater_bed"}, {"hotend", "extruder"}}},
           {"leds", {{"strip", "led chamber_light"}}},
           {"hardware",
            {{"expected",
              {"heater_bed", "extruder", "fan_generic chamber_fan", "led chamber_light"}}}}}}});

    // Simulate detection flow
    config.set_preset("ad5m_pro");
    bool applied = config.apply_preset_file("ad5m_pro");
    REQUIRE(applied == true);

    // Verify preset name was updated
    REQUIRE(config.get_preset() == "ad5m_pro");

    // Verify hardware was merged
    REQUIRE(printer_data()["fans"].contains("chamber"));
    REQUIRE(printer_data().contains("leds"));

    // Verify non-hardware preserved
    REQUIRE(printer_data()["moonraker_host"] == "127.0.0.1");

    TearDown();
}
