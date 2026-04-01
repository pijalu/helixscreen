// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace helix {
struct AbsCapabilities;
}  // namespace helix

namespace helix::input {

struct ScannedDevice {
    std::string path;
    std::string name;
    int event_num = -1;
};

/// USB HID device with vendor/product identification for manual scanner selection.
struct UsbHidDevice {
    std::string name;         // e.g., "TMS HIDKeyBoard"
    std::string vendor_id;    // e.g., "1a2c" (hex from sysfs)
    std::string product_id;   // e.g., "4c5e" (hex from sysfs)
    std::string event_path;   // e.g., "/dev/input/event5"
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

/// Set a callback that returns the configured scanner device ID (vendor:product).
/// Called by the no-arg find_keyboard_device() to exclude the scanner from keyboard detection.
/// Must be set before find_keyboard_device() is called (typically during display backend init).
void set_scanner_device_id_provider(std::function<std::string()> provider);

/// Scan /dev/input/event* for keyboard devices (KEY_A set).
/// Skips devices that look like barcode scanners ("barcode"/"scanner" in name).
/// If exclude_vendor_product is non-empty (format "vendor:product"), also skips
/// that specific device. This prevents LVGL from claiming a barcode scanner as
/// its keyboard input.
std::optional<ScannedDevice> find_keyboard_device();
std::optional<ScannedDevice> find_keyboard_device(const std::string& dev_base,
                                                   const std::string& sysfs_base,
                                                   const std::string& exclude_vendor_product = "");

/// Scan /dev/input/event* for USB HID keyboard-like devices suitable for barcode scanning.
/// Returns ALL matching devices (USB/BT bus, has KEY_A, not a touchscreen).
/// Prioritizes devices with "barcode"/"scanner" in name, then any other HID keyboard.
std::vector<ScannedDevice> find_hid_keyboard_devices();
std::vector<ScannedDevice> find_hid_keyboard_devices(const std::string& dev_base,
                                                      const std::string& sysfs_base);

/// Like find_hid_keyboard_devices(), but if configured_vendor_product is non-empty
/// (format "vendor:product", e.g. "1a2c:4c5e"), the matching device is returned
/// as the sole result with highest priority. Falls back to name-based priority
/// if the configured device is not found.
std::vector<ScannedDevice> find_hid_keyboard_devices(const std::string& dev_base,
                                                      const std::string& sysfs_base,
                                                      const std::string& configured_vendor_product);

/// Enumerate all USB HID keyboard-capable devices with vendor/product IDs.
/// Used by the scanner picker UI. Returns all matching devices (no prioritization).
std::vector<UsbHidDevice> enumerate_usb_hid_devices();
std::vector<UsbHidDevice> enumerate_usb_hid_devices(const std::string& dev_base,
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
