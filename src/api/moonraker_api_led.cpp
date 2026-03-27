// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_error_reporting.h"

#include "moonraker_api.h"
#include "moonraker_api_internal.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>
#include <sstream>

using namespace moonraker_internal;

// ============================================================================
// LED Control Operations
// ============================================================================

void MoonrakerAPI::set_led(const std::string& led, double red, double green, double blue,
                           double white, SuccessCallback on_success, ErrorCallback on_error) {
    // Reject NaN/Inf before any G-code generation
    if (reject_non_finite({red, green, blue, white}, "set_led", on_error)) {
        return;
    }

    // Validate LED name
    if (!is_safe_identifier(led)) {
        NOTIFY_ERROR("Invalid LED name '{}'. Contains unsafe characters.", led);
        if (on_error) {
            MoonrakerError err;
            err.type = MoonrakerErrorType::VALIDATION_ERROR;
            err.message = "Invalid LED name contains illegal characters";
            err.method = "set_led";
            on_error(err);
        }
        return;
    }

    // Clamp color values to 0.0-1.0 range
    red = std::clamp(red, 0.0, 1.0);
    green = std::clamp(green, 0.0, 1.0);
    blue = std::clamp(blue, 0.0, 1.0);
    white = std::clamp(white, 0.0, 1.0);

    // Extract just the LED name without the type prefix (e.g., "neopixel " or "led ")
    std::string led_name = led;
    size_t space_pos = led.find(' ');
    if (space_pos != std::string::npos) {
        led_name = led.substr(space_pos + 1);
    }

    // Build SET_LED G-code command
    // Quote LED name (may contain spaces), add SYNC=0 TRANSMIT=1 for immediate effect
    std::ostringstream gcode;
    gcode << "SET_LED LED=\"" << led_name << "\" RED=" << red << " GREEN=" << green
          << " BLUE=" << blue;

    // Only add WHITE parameter if non-zero (for RGBW LEDs)
    if (white > 0.0) {
        gcode << " WHITE=" << white;
    }

    gcode << " SYNC=0 TRANSMIT=1";

    spdlog::info("[Moonraker API] Setting LED {}: R={:.2f} G={:.2f} B={:.2f} W={:.2f}", led_name,
                 red, green, blue, white);

    execute_gcode(gcode.str(), on_success, on_error);
}

void MoonrakerAPI::set_led_on(const std::string& led, SuccessCallback on_success,
                              ErrorCallback on_error) {
    set_led(led, 1.0, 1.0, 1.0, 1.0, on_success, on_error);
}

void MoonrakerAPI::set_led_off(const std::string& led, SuccessCallback on_success,
                               ErrorCallback on_error) {
    set_led(led, 0.0, 0.0, 0.0, 0.0, on_success, on_error);
}
