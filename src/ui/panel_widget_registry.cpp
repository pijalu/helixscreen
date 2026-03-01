// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string_view>

namespace helix {

// Forward declarations — widget registration functions (defined in each widget .cpp)
void register_fan_stack_widget();
void register_temperature_widget();
void register_temp_stack_widget();
void register_power_widget();
void register_network_widget();
void register_led_widget();
void register_led_controls_widget();
void register_thermistor_widget();
void register_favorite_macro_widgets();
void register_tips_widget();
void register_humidity_widget();
void register_width_sensor_widget();
void register_printer_image_widget();
void register_print_status_widget();
void register_shutdown_widget();
void register_clock_widget();
void register_job_queue_widget();

// Vector order defines the default display order on the home panel.
// NOTE: Factories are registered at runtime via init_widget_registrations(),
// NOT during static initialization. Do not add file-scope self-registration.
// clang-format off
static std::vector<PanelWidgetDef> s_widget_defs = {
    //                                                                                                                                          en  col row min_c min_r max_c max_r
    {"printer_image",    "Printer Image",    "rotate_3d",        "3D printer visualization",                     "Printer Image",    nullptr,              true,  2, 2, 1, 1, 4, 3},
    {"print_status",     "Print Status",     "printer_3d",       "Print progress and file selection",            "Print Status",     nullptr,              true,  2, 2, 2, 1, 4, 3},
    {"shutdown",         "Shutdown/Reboot",   "power",            "Shutdown or reboot the printer host",          "Shutdown/Reboot",  nullptr,              false, 1, 1, 1, 1, 1, 1},
    {"power",            "Power",            "power_cycle",      "Moonraker power device controls",              "Power",            "power_device_count", false, 1, 1, 1, 1, 1, 1},
    {"network",          "Network",          "wifi_strength_4",  "Wi-Fi and ethernet connection status",         "Network",          nullptr,              false, 1, 1, 1, 1, 2, 1},
    {"firmware_restart", "Firmware Restart",  "refresh",          "Restart Klipper firmware",                     "Firmware Restart", nullptr,              false, 1, 1, 1, 1, 1, 1},
    {"ams",              "AMS Status",        "filament",         "Multi-material spool status and control",      "AMS Status",       "ams_slot_count",     true,  1, 1, 1, 1, 2, 2},
    {"led",              "LED Light",         "lightbulb_outline","Quick toggle, long press for full control",    "LED Light",        "printer_has_led",    true,  1, 1, 1, 1, 2, 1},
    {"led_controls",     "LED Controls",      "led_strip",        "Open LED color and brightness controls",       "LED Controls",     "printer_has_led",    false, 1, 1, 1, 1, 1, 1},
    {"fan_stack",        "Fan Speeds",        "fan",              "Part, hotend, and auxiliary fan speeds",        "Fan Speeds",       nullptr,              true,  1, 1, 1, 1, 3, 2},
    {"temperature",      "Nozzle Temperature","thermometer",      "Monitor and set nozzle temperature",           "Nozzle Temperature", nullptr,            true,  1, 1, 1, 1, 2, 2},
    {"temp_stack",       "Temperatures",      "thermometer",      "Nozzle, bed, and chamber temps stacked",       "Temperatures",     nullptr,              false, 1, 1, 1, 1, 3, 2},
    {"filament",         "Filament Sensor",   "filament_alert",   "Filament runout detection status",             "Filament Sensor",  "filament_sensor_count", true, 1, 1, 1, 1, 2, 1},
    {"humidity",         "Humidity",          "water",            "Enclosure humidity sensor readings",           "Humidity",         "humidity_sensor_count", false, 1, 1, 1, 1, 2, 2},
    {"width_sensor",     "Width Sensor",      "ruler",            "Filament width sensor readings",               "Width Sensor",     "width_sensor_count", false, 1, 1, 1, 1, 2, 2},
    {"thermistor",       "Thermistor",        "thermometer",      "Monitor a custom temperature sensor",          "Thermistor",       "temp_sensor_count",  false, 1, 1, 1, 1, 2, 1},
    {"favorite_macro_1", "Macro Button 1",    "play",             "Run a configured macro with one tap",          "Macro Button 1",   nullptr,              false, 1, 1, 1, 1, 2, 1},
    {"favorite_macro_2", "Macro Button 2",    "play",             "Run a configured macro with one tap",          "Macro Button 2",   nullptr,              false, 1, 1, 1, 1, 2, 1},
    {"clock",            "Digital Clock",     "clock",            "Current time and date",                       "Digital Clock",    nullptr,              false, 2, 1, 1, 1, 3, 3},
    {"job_queue",        "Job Queue",         "progress_clock",   "Queued print jobs",                           "Job Queue",        nullptr,              false, 2, 2, 2, 1, 4, 3},
    //                                                                                                                                          en  col row min_c min_r max_c max_r
    {"tips",             "Tips",              "help_circle",      "Rotating tips and helpful information",        "Tips",             nullptr,              true,  4, 2, 2, 1, 6, 2},
    {"notifications",    "Notifications",     "notifications",    "Pending alerts and system messages",           "Notifications",    nullptr,              true,  1, 1, 1, 1, 2, 1},
};
// clang-format on

const std::vector<PanelWidgetDef>& get_all_widget_defs() {
    return s_widget_defs;
}

const PanelWidgetDef* find_widget_def(std::string_view id) {
    auto it = std::find_if(s_widget_defs.begin(), s_widget_defs.end(),
                           [&id](const PanelWidgetDef& def) { return id == def.id; });
    return it != s_widget_defs.end() ? &*it : nullptr;
}

size_t widget_def_count() {
    return s_widget_defs.size();
}

void register_widget_factory(std::string_view id, WidgetFactory factory) {
    for (auto& def : s_widget_defs) {
        if (id == def.id) {
            def.factory = std::move(factory);
            return;
        }
    }
    spdlog::warn("[PanelWidgetRegistry] Factory registration failed: '{}' not found", id);
}

void register_widget_subjects(std::string_view id, SubjectInitFn init_fn) {
    for (auto& def : s_widget_defs) {
        if (id == def.id) {
            def.init_subjects = std::move(init_fn);
            return;
        }
    }
    spdlog::warn("[PanelWidgetRegistry] Subject init registration failed: '{}' not found", id);
}

void init_widget_registrations() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    register_printer_image_widget();
    register_print_status_widget();
    register_power_widget();
    register_network_widget();
    register_temperature_widget();
    register_temp_stack_widget();
    register_led_widget();
    register_led_controls_widget();
    register_fan_stack_widget();
    register_thermistor_widget();
    register_favorite_macro_widgets();
    register_clock_widget();
    register_job_queue_widget();
    register_tips_widget();
    register_humidity_widget();
    register_width_sensor_widget();
    register_shutdown_widget();

    spdlog::debug("[PanelWidgetRegistry] All widget factories registered");
}

} // namespace helix
