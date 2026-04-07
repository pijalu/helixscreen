// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_controller.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("NativeBackend: set_color with null API calls error callback", "[led][native]") {
    helix::led::NativeBackend backend;
    // No API set

    bool error_called = false;
    std::string error_msg;
    backend.set_color("neopixel test", 1.0, 0.0, 0.0, 0.0, nullptr, [&](const std::string& err) {
        error_called = true;
        error_msg = err;
    });

    REQUIRE(error_called);
    REQUIRE(!error_msg.empty());
}

TEST_CASE("NativeBackend: turn_on with null API calls error callback", "[led][native]") {
    helix::led::NativeBackend backend;

    bool error_called = false;
    backend.turn_on("neopixel test", nullptr, [&](const std::string& err) { error_called = true; });

    REQUIRE(error_called);
}

TEST_CASE("NativeBackend: turn_off with null API calls error callback", "[led][native]") {
    helix::led::NativeBackend backend;

    bool error_called = false;
    backend.turn_off("neopixel test", nullptr,
                     [&](const std::string& err) { error_called = true; });

    REQUIRE(error_called);
}

TEST_CASE("NativeBackend: set_brightness with null API calls error callback", "[led][native]") {
    helix::led::NativeBackend backend;

    bool error_called = false;
    backend.set_brightness("neopixel test", 50, 1.0, 1.0, 1.0, 0.0, nullptr,
                           [&](const std::string& err) { error_called = true; });

    REQUIRE(error_called);
}

TEST_CASE("NativeBackend: null error callback with null API doesn't crash", "[led][native]") {
    helix::led::NativeBackend backend;

    // Should not crash even without callbacks
    backend.set_color("neopixel test", 1.0, 0.0, 0.0, 0.0, nullptr, nullptr);
    backend.turn_on("neopixel test", nullptr, nullptr);
    backend.turn_off("neopixel test", nullptr, nullptr);
    backend.set_brightness("neopixel test", 50, 1.0, 1.0, 1.0, 0.0, nullptr, nullptr);
}

TEST_CASE("NativeBackend: strip type detection", "[led][native]") {
    helix::led::NativeBackend backend;

    REQUIRE(backend.type() == helix::led::LedBackendType::NATIVE);
}

TEST_CASE("NativeBackend: update_from_status detects RGBW from 4-element color_data",
          "[led][native][rgbw]") {
    helix::led::NativeBackend backend;

    helix::led::LedStripInfo strip;
    strip.name = "Chamber";
    strip.id = "neopixel chamber";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = true;
    strip.supports_white = true; // prefix-based default
    backend.add_strip(strip);

    // 4-element color_data confirms RGBW
    nlohmann::json status = {{"neopixel chamber", {{"color_data", {{0.0, 0.0, 0.0, 1.0}}}}}};
    backend.update_from_status(status);
    REQUIRE(backend.strips()[0].supports_white == true);

    // 3-element color_data overrides to RGB-only
    nlohmann::json status_rgb = {{"neopixel chamber", {{"color_data", {{1.0, 0.0, 0.0}}}}}};
    backend.update_from_status(status_rgb);
    REQUIRE(backend.strips()[0].supports_white == false);
}

TEST_CASE("NativeBackend: update_from_status corrects wrong RGBW guess", "[led][native][rgbw]") {
    helix::led::NativeBackend backend;

    // Neopixel guessed as RGBW but is actually RGB
    helix::led::LedStripInfo strip;
    strip.name = "Status";
    strip.id = "neopixel status_led";
    strip.backend = helix::led::LedBackendType::NATIVE;
    strip.supports_color = true;
    strip.supports_white = true; // wrong guess
    backend.add_strip(strip);

    nlohmann::json status = {{"neopixel status_led", {{"color_data", {{0.5, 0.5, 0.5}}}}}};
    backend.update_from_status(status);
    REQUIRE(backend.strips()[0].supports_white == false);
}
