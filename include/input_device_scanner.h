// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>

namespace helix {
struct AbsCapabilities;
}  // namespace helix

namespace helix::input {

struct ScannedDevice {
    std::string path;
    std::string name;
    int event_num = -1;
};

/// Check if a specific bit is set in a sysfs capability hex bitmask.
/// The kernel prints space-separated hex words (unsigned long), rightmost = lowest bits.
/// Handles both 32-bit and 64-bit word widths automatically by inferring
/// bits-per-word from the longest hex word in the bitmask (<=8 digits = 32-bit,
/// >8 digits = 64-bit). Single-word bitmasks use actual digit count.
bool check_capability_bit(const std::string& hex_bitmask, int bit);

/// Scan /dev/input/event* for mouse devices (REL_X + REL_Y + BTN_LEFT, no ABS_X/ABS_Y).
std::optional<ScannedDevice> find_mouse_device();
std::optional<ScannedDevice> find_mouse_device(const std::string& dev_base,
                                                const std::string& sysfs_base);

/// Scan /dev/input/event* for keyboard devices (KEY_A set).
std::optional<ScannedDevice> find_keyboard_device();
std::optional<ScannedDevice> find_keyboard_device(const std::string& dev_base,
                                                   const std::string& sysfs_base);

/// Read a single line from a sysfs file (returns empty string on failure)
std::string read_sysfs_line(const std::string& path);

/// Get device name from /sys/class/input/eventN/device/name
std::string get_input_device_name(int event_num);

/// Get device phys from /sys/class/input/eventN/device/phys
std::string get_input_device_phys(int event_num);

/// Check if input device has touch ABS capabilities, optionally fill AbsCapabilities
bool get_input_touch_capabilities(int event_num, helix::AbsCapabilities* caps_out = nullptr);

}  // namespace helix::input
