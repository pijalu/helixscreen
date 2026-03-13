// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>

namespace helix::input {

struct ScannedDevice {
    std::string path;
    std::string name;
    int event_num = -1;
};

/// Check if a specific bit is set in a sysfs capability hex bitmask.
/// The kernel prints space-separated hex words (unsigned long), rightmost = lowest bits.
/// Handles both 32-bit and 64-bit word widths automatically by inferring
/// bits-per-word from the hex digit count of each word. This is correct even
/// when the kernel omits leading zeros, because a bit can only appear set in a
/// word whose hex representation includes that bit position.
bool check_capability_bit(const std::string& hex_bitmask, int bit);

/// Scan /dev/input/event* for mouse devices (REL_X + REL_Y + BTN_LEFT, no ABS_X/ABS_Y).
std::optional<ScannedDevice> find_mouse_device();
std::optional<ScannedDevice> find_mouse_device(const std::string& dev_base,
                                                const std::string& sysfs_base);

/// Scan /dev/input/event* for keyboard devices (KEY_A set).
std::optional<ScannedDevice> find_keyboard_device();
std::optional<ScannedDevice> find_keyboard_device(const std::string& dev_base,
                                                   const std::string& sysfs_base);

}  // namespace helix::input
