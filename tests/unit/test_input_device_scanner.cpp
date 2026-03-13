// SPDX-License-Identifier: GPL-3.0-or-later

#include "../catch_amalgamated.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

#include "input_device_scanner.h"

using helix::input::check_capability_bit;

namespace fs = std::filesystem;

namespace {

struct MockInputTree {
    std::string base;
    std::string dev_dir;
    std::string sysfs_dir;

    explicit MockInputTree(const std::string& label) {
        base = "/tmp/helix_test_input_" + label + "_" +
               std::to_string(static_cast<unsigned long>(time(nullptr)));
        dev_dir = base + "/dev/input";
        sysfs_dir = base + "/sys/class/input";
        fs::create_directories(dev_dir);
        fs::create_directories(sysfs_dir);
    }

    ~MockInputTree() {
        std::error_code ec;
        fs::remove_all(base, ec);
    }

    void add_device(int event_num, const std::string& name,
                    const std::map<std::string, std::string>& caps) {
        std::string dev_path = dev_dir + "/event" + std::to_string(event_num);
        std::ofstream(dev_path).put('x');

        std::string sysfs_path = sysfs_dir + "/event" + std::to_string(event_num);
        fs::create_directories(sysfs_path + "/device/capabilities");

        std::ofstream(sysfs_path + "/device/name") << name;

        for (const auto& [cap_name, hex_value] : caps) {
            std::ofstream(sysfs_path + "/device/capabilities/" + cap_name) << hex_value;
        }
    }
};

}  // namespace

TEST_CASE("check_capability_bit parses sysfs hex bitmasks", "[input]") {

    SECTION("empty string returns false") {
        REQUIRE_FALSE(check_capability_bit("", 0));
        REQUIRE_FALSE(check_capability_bit("", 30));
    }

    SECTION("bit 0 in single word") {
        REQUIRE(check_capability_bit("1", 0));
        REQUIRE_FALSE(check_capability_bit("0", 0));
    }

    SECTION("bit 1 in single word") {
        REQUIRE(check_capability_bit("3", 1));
        REQUIRE(check_capability_bit("2", 1));
        REQUIRE_FALSE(check_capability_bit("1", 1));
    }

    SECTION("KEY_A (bit 30) in last word") {
        REQUIRE(check_capability_bit("40000000", 30));
        REQUIRE_FALSE(check_capability_bit("20000000", 30));
    }

    SECTION("KEY_A in multi-word bitmask (32-bit words)") {
        REQUIRE(check_capability_bit("10000 40000000", 30));
    }

    SECTION("KEY_A in multi-word bitmask (64-bit words)") {
        REQUIRE(check_capability_bit("10000 0000000040000000", 30));
    }

    SECTION("BTN_LEFT (bit 272) in 32-bit word format") {
        REQUIRE(check_capability_bit("10000 0 0 0 0 0 0 0 0", 272));
    }

    SECTION("BTN_LEFT (bit 272) in 64-bit word format") {
        // At least one word must have >8 hex digits to signal 64-bit
        REQUIRE(check_capability_bit("10000 0000000000000000 0 0 0", 272));
    }

    SECTION("BTN_LEFT not set") {
        REQUIRE_FALSE(check_capability_bit("0 0 0 0 0 0 0 0 0", 272));
    }

    SECTION("KEY_POWER (bit 116) — should not match KEY_A check") {
        REQUIRE(check_capability_bit("0 0 0 0 0 100000 0 0 0", 116));
        REQUIRE_FALSE(check_capability_bit("0 0 0 0 0 100000 0 0 0", 30));
    }

    SECTION("real-world keyboard capability string from Pi 5 (aarch64)") {
        // This keyboard has both KEY_A and BTN_LEFT set (combo keyboard+trackpad)
        const char* real_kb = "3 0 0 0 0 0 403ffff 73ffff206efffd f3cfffff ffffffff fffffffe";
        REQUIRE(check_capability_bit(real_kb, 30));   // KEY_A
        REQUIRE(check_capability_bit(real_kb, 272));   // BTN_LEFT (combo device)
    }

    SECTION("real-world mouse capability string") {
        const char* real_mouse_32 = "1f0000 0 0 0 0 0 0 0 0";
        REQUIRE(check_capability_bit(real_mouse_32, 272));
        REQUIRE(check_capability_bit(real_mouse_32, 273));
        REQUIRE_FALSE(check_capability_bit(real_mouse_32, 30));

        const char* real_mouse_64 = "1f0000 0000000000000000 0 0 0";
        REQUIRE(check_capability_bit(real_mouse_64, 272));
    }

    SECTION("negative bit number returns false") {
        REQUIRE_FALSE(check_capability_bit("ffffffff", -1));
    }
}

TEST_CASE("find_mouse_device detects USB HID mice via sysfs", "[input]") {
    using helix::input::find_mouse_device;

    SECTION("detects mouse with REL_X + REL_Y + BTN_LEFT") {
        MockInputTree tree("mouse_basic");
        tree.add_device(3, "Logitech USB Mouse", {
            {"rel", "3"},
            {"key", "1f0000 0 0 0 0 0 0 0 0"},
            {"abs", "0"}
        });

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "Logitech USB Mouse");
        REQUIRE(result->event_num == 3);
    }

    SECTION("skips touchscreen with ABS_X + ABS_Y") {
        MockInputTree tree("mouse_skip_touch");
        tree.add_device(0, "Goodix Touchscreen", {
            {"abs", "3"},
            {"rel", "0"},
            {"key", "0"}
        });

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("skips device without BTN_LEFT") {
        MockInputTree tree("mouse_no_btn");
        tree.add_device(1, "Some Sensor", {
            {"rel", "3"},
            {"key", "0"},
            {"abs", "0"}
        });

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("empty directory returns nullopt") {
        MockInputTree tree("mouse_empty");
        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("picks mouse when touchscreen also present") {
        MockInputTree tree("mouse_with_touch");
        tree.add_device(0, "Goodix Touchscreen", {
            {"abs", "3"},
            {"rel", "0"},
            {"key", "0"}
        });
        tree.add_device(2, "USB Mouse", {
            {"rel", "3"},
            {"key", "1f0000 0 0 0 0 0 0 0 0"},
            {"abs", "0"}
        });

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Mouse");
        REQUIRE(result->event_num == 2);
    }

    SECTION("skips device with both ABS and REL (touchscreen with mouse emulation)") {
        MockInputTree tree("mouse_abs_rel");
        tree.add_device(0, "TouchMouseCombo", {
            {"abs", "3"},
            {"rel", "3"},
            {"key", "1f0000 0 0 0 0 0 0 0 0"}
        });

        auto result = find_mouse_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("find_keyboard_device detects USB HID keyboards via sysfs", "[input]") {
    using helix::input::find_keyboard_device;

    SECTION("detects keyboard with KEY_A") {
        MockInputTree tree("kb_basic");
        tree.add_device(1, "USB Keyboard", {
            {"key", "40000000"},
            {"rel", "0"},
            {"abs", "0"}
        });

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Keyboard");
    }

    SECTION("skips power button (KEY_POWER=116 but no KEY_A)") {
        MockInputTree tree("kb_power");
        tree.add_device(0, "Power Button", {
            {"key", "0 0 0 0 0 100000 0 0 0"},
            {"rel", "0"},
            {"abs", "0"}
        });

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("finds keyboard among other devices") {
        MockInputTree tree("kb_mixed");
        tree.add_device(0, "Goodix Touchscreen", {
            {"key", "0"},
            {"abs", "3"},
            {"rel", "0"}
        });
        tree.add_device(1, "Power Button", {
            {"key", "100000"},
            {"abs", "0"},
            {"rel", "0"}
        });
        tree.add_device(2, "USB Keyboard", {
            {"key", "10000 40000000"},
            {"abs", "0"},
            {"rel", "0"}
        });

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "USB Keyboard");
    }

    SECTION("empty directory returns nullopt") {
        MockInputTree tree("kb_empty");
        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("combo keyboard+mouse device detected as keyboard") {
        MockInputTree tree("kb_combo");
        tree.add_device(0, "Logitech K400", {
            {"key", "1f0000 0 0 0 0 0 0 0 40000000"},
            {"rel", "3"},
            {"abs", "0"}
        });

        auto result = find_keyboard_device(tree.dev_dir, tree.sysfs_dir);
        REQUIRE(result.has_value());
        REQUIRE(result->name == "Logitech K400");
    }
}
