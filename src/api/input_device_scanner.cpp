// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_device_scanner.h"
#include "settings_manager.h"
#include "touch_calibration.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <unistd.h>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

// Build sysfs path: /sys/class/input/eventN/device/<subpath>
std::string sysfs_device_path(const std::string& sysfs_base, int event_num,
                               const std::string& subpath) {
    return sysfs_base + "/event" + std::to_string(event_num) + "/device/" + subpath;
}

std::string read_sysfs_capability(const std::string& sysfs_base, int event_num,
                                   const std::string& cap_name) {
    return helix::input::read_sysfs_line(
        sysfs_device_path(sysfs_base, event_num, "capabilities/" + cap_name));
}

std::string read_device_name(const std::string& sysfs_base, int event_num) {
    return helix::input::read_sysfs_line(sysfs_device_path(sysfs_base, event_num, "name"));
}

// Read /sys/class/input/eventN/device/id/bustype — returns bus type as integer.
// BUS_USB=3, BUS_BLUETOOTH=5. Returns 0 on failure.
int read_bus_type(const std::string& sysfs_base, int event_num) {
    std::string line = helix::input::read_sysfs_line(
        sysfs_device_path(sysfs_base, event_num, "id/bustype"));
    if (line.empty()) return 0;
    return static_cast<int>(std::strtol(line.c_str(), nullptr, 16));
}

std::string read_vendor_id(const std::string& sysfs_base, int event_num) {
    return helix::input::read_sysfs_line(
        sysfs_device_path(sysfs_base, event_num, "id/vendor"));
}

std::string read_product_id(const std::string& sysfs_base, int event_num) {
    return helix::input::read_sysfs_line(
        sysfs_device_path(sysfs_base, event_num, "id/product"));
}

constexpr int BUS_USB = 0x03;
constexpr int BUS_BLUETOOTH = 0x05;

}  // namespace

namespace helix::input {

using helix::SettingsManager;

bool check_capability_bit(const std::string& hex_bitmask, int bit) {
    if (bit < 0 || hex_bitmask.empty()) {
        return false;
    }

    // Split on spaces into words (rightmost = lowest bits)
    std::vector<std::string> words;
    std::istringstream stream(hex_bitmask);
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }

    if (words.empty()) {
        return false;
    }

    // Each word is an unsigned long printed in hex. The kernel strips leading
    // zeros, so "0" could be 32-bit or 64-bit. Infer arch word width from the
    // longest word's digit count and apply uniformly. This ensures short words
    // like "1" are treated as 32-bit (not 4-bit), making all 32 bit positions
    // addressable.
    size_t max_digits = 0;
    for (const auto& w : words) {
        if (w.size() > max_digits) {
            max_digits = w.size();
        }
    }
    // Round up to 32-bit (8 digits) or 64-bit (16 digits) boundary
    int bits_per_word = (max_digits <= 8) ? 32 : 64;

    // Determine which word contains our bit (right-to-left, 0-indexed)
    int word_index_from_right = bit / bits_per_word;
    int bit_in_word = bit % bits_per_word;

    // Convert to array index (words[0] = leftmost = highest bits)
    int array_index = static_cast<int>(words.size()) - 1 - word_index_from_right;
    if (array_index < 0 || array_index >= static_cast<int>(words.size())) {
        return false;
    }

    unsigned long val = std::strtoul(words[array_index].c_str(), nullptr, 16);
    return (val & (1UL << bit_in_word)) != 0;
}

std::optional<ScannedDevice> find_mouse_device(const std::string& dev_base,
                                                const std::string& sysfs_base) {
    auto dir = std::unique_ptr<DIR, decltype(&closedir)>(opendir(dev_base.c_str()), closedir);
    if (!dir) {
        spdlog::debug("[InputScanner] Cannot open {}", dev_base);
        return std::nullopt;
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0) {
            continue;
        }

        std::string device_path = dev_base + "/" + entry->d_name;
        if (access(device_path.c_str(), R_OK) != 0) {
            continue;
        }

        std::string name = read_device_name(sysfs_base, event_num);
        std::string abs_caps = read_sysfs_capability(sysfs_base, event_num, "abs");
        std::string rel_caps = read_sysfs_capability(sysfs_base, event_num, "rel");
        std::string key_caps = read_sysfs_capability(sysfs_base, event_num, "key");

        spdlog::debug("[InputScanner] Scanning {} ({}) abs=[{}] rel=[{}] key=[{}]",
                      device_path, name, abs_caps, rel_caps, key_caps);

        // Skip touchscreens:
        //   - Legacy single-touch: ABS_X (bit 0) + ABS_Y (bit 1)
        //   - MT-only (e.g. Goodix gt9xxnew_ts): ABS_MT_POSITION_X (bit 53) +
        //     ABS_MT_POSITION_Y (bit 54) without legacy ABS_X/ABS_Y
        //   - Any device with BTN_TOUCH (bit 330) — touchscreens, not mice
        bool has_legacy_abs = check_capability_bit(abs_caps, 0) &&
                              check_capability_bit(abs_caps, 1);
        bool has_mt_abs = check_capability_bit(abs_caps, 53) &&
                          check_capability_bit(abs_caps, 54);
        bool has_btn_touch = check_capability_bit(key_caps, 330);

        if (has_legacy_abs || has_mt_abs || has_btn_touch) {
            spdlog::debug("[InputScanner] Skipping {} (touchscreen: legacy_abs={} mt_abs={} "
                          "btn_touch={})", device_path, has_legacy_abs, has_mt_abs, has_btn_touch);
            continue;
        }

        // Require REL_X (bit 0) + REL_Y (bit 1)
        if (!check_capability_bit(rel_caps, 0) || !check_capability_bit(rel_caps, 1)) {
            spdlog::debug("[InputScanner] Skipping {} (no REL_X/REL_Y)", device_path);
            continue;
        }

        // Require BTN_LEFT (bit 272)
        if (!check_capability_bit(key_caps, 272)) {
            spdlog::debug("[InputScanner] Skipping {} (no BTN_LEFT)", device_path);
            continue;
        }

        // Only accept USB or Bluetooth devices — excludes SoC-integrated IR
        // receivers (e.g. MCE IR Keyboard/Mouse on Allwinner), HDMI CEC virtual
        // devices, and other non-physical "mice" that report mouse capabilities.
        int bus = read_bus_type(sysfs_base, event_num);
        if (bus != BUS_USB && bus != BUS_BLUETOOTH) {
            spdlog::debug("[InputScanner] Skipping {} (bus type 0x{:04x}, not USB/BT)",
                          device_path, bus);
            continue;
        }

        spdlog::info("[InputScanner] Found mouse: {} ({})", device_path, name);
        return ScannedDevice{device_path, name, event_num};
    }

    return std::nullopt;
}

std::optional<ScannedDevice> find_mouse_device() {
    return find_mouse_device("/dev/input", "/sys/class/input");
}

std::optional<ScannedDevice> find_keyboard_device(const std::string& dev_base,
                                                   const std::string& sysfs_base,
                                                   const std::string& exclude_vendor_product) {
    // Parse exclusion vendor:product pair if provided
    std::string exclude_vendor;
    std::string exclude_product;
    if (!exclude_vendor_product.empty()) {
        auto colon = exclude_vendor_product.find(':');
        if (colon != std::string::npos) {
            exclude_vendor = exclude_vendor_product.substr(0, colon);
            exclude_product = exclude_vendor_product.substr(colon + 1);
        }
    }

    auto dir = std::unique_ptr<DIR, decltype(&closedir)>(opendir(dev_base.c_str()), closedir);
    if (!dir) {
        spdlog::debug("[InputScanner] Cannot open {}", dev_base);
        return std::nullopt;
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0) {
            continue;
        }

        std::string device_path = dev_base + "/" + entry->d_name;
        if (access(device_path.c_str(), R_OK) != 0) {
            continue;
        }

        // Only accept USB or Bluetooth devices
        int bus = read_bus_type(sysfs_base, event_num);
        if (bus != BUS_USB && bus != BUS_BLUETOOTH) {
            continue;
        }

        // Require KEY_A (bit 30) — distinguishes real keyboards from power buttons etc.
        std::string key_caps = read_sysfs_capability(sysfs_base, event_num, "key");
        if (!check_capability_bit(key_caps, 30)) {
            continue;
        }

        std::string name = read_device_name(sysfs_base, event_num);

        // Skip devices with "barcode" or "scanner" in the name — these are
        // barcode scanners that happen to present as USB HID keyboards.
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_name.find("barcode") != std::string::npos ||
            lower_name.find("scanner") != std::string::npos) {
            spdlog::info("[InputScanner] Skipping scanner device for keyboard: {} ({})",
                         device_path, name);
            continue;
        }

        // Skip device matching the user's configured scanner vendor:product
        if (!exclude_vendor.empty()) {
            std::string vendor = read_vendor_id(sysfs_base, event_num);
            std::string product = read_product_id(sysfs_base, event_num);
            if (vendor == exclude_vendor && product == exclude_product) {
                spdlog::info("[InputScanner] Skipping configured scanner for keyboard: {} ({})",
                             device_path, name);
                continue;
            }
        }

        spdlog::info("[InputScanner] Found keyboard: {} ({})", device_path, name);
        return ScannedDevice{device_path, name, event_num};
    }

    return std::nullopt;
}

std::optional<ScannedDevice> find_keyboard_device() {
    std::string exclude_id;
    try {
        exclude_id = SettingsManager::instance().get_scanner_device_id();
    } catch (...) {
        // SettingsManager may not be initialized yet during early startup
    }
    return find_keyboard_device("/dev/input", "/sys/class/input", exclude_id);
}

std::vector<ScannedDevice> find_hid_keyboard_devices(const std::string& dev_base,
                                                      const std::string& sysfs_base) {
    std::vector<ScannedDevice> named_scanners;   // "barcode"/"scanner" in name — high priority
    std::vector<ScannedDevice> generic_keyboards; // any other USB HID keyboard

    auto dir = std::unique_ptr<DIR, decltype(&closedir)>(opendir(dev_base.c_str()), closedir);
    if (!dir) {
        spdlog::debug("[InputScanner] Cannot open {}", dev_base);
        return {};
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0) {
            continue;
        }

        std::string device_path = dev_base + "/" + entry->d_name;
        if (access(device_path.c_str(), R_OK) != 0) {
            continue;
        }

        // Only accept USB or Bluetooth devices
        int bus = read_bus_type(sysfs_base, event_num);
        if (bus != BUS_USB && bus != BUS_BLUETOOTH) {
            continue;
        }

        // Require KEY_A (bit 30) — real keyboard-like device
        std::string key_caps = read_sysfs_capability(sysfs_base, event_num, "key");
        if (!check_capability_bit(key_caps, 30)) {
            continue;
        }

        // Skip touchscreens (have ABS_X/ABS_Y or ABS_MT_POSITION_X/Y)
        std::string abs_caps = read_sysfs_capability(sysfs_base, event_num, "abs");
        bool has_legacy_abs = check_capability_bit(abs_caps, 0) &&
                              check_capability_bit(abs_caps, 1);
        bool has_mt_abs = check_capability_bit(abs_caps, 53) &&
                          check_capability_bit(abs_caps, 54);
        if (has_legacy_abs || has_mt_abs) {
            continue;
        }

        std::string name = read_device_name(sysfs_base, event_num);
        ScannedDevice dev{device_path, name, event_num};

        // Prioritize devices with "barcode" or "scanner" in the name
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_name.find("barcode") != std::string::npos ||
            lower_name.find("scanner") != std::string::npos) {
            spdlog::info("[InputScanner] Found named scanner device: {} ({})", device_path, name);
            named_scanners.push_back(std::move(dev));
        } else {
            spdlog::info("[InputScanner] Found USB HID keyboard device: {} ({})",
                         device_path, name);
            generic_keyboards.push_back(std::move(dev));
        }
    }

    // Return named scanners first, then generic keyboards
    std::vector<ScannedDevice> result;
    result.reserve(named_scanners.size() + generic_keyboards.size());
    for (auto& d : named_scanners) result.push_back(std::move(d));
    for (auto& d : generic_keyboards) result.push_back(std::move(d));
    return result;
}

std::vector<ScannedDevice> find_hid_keyboard_devices() {
    return find_hid_keyboard_devices("/dev/input", "/sys/class/input");
}

std::vector<ScannedDevice> find_hid_keyboard_devices(const std::string& dev_base,
                                                      const std::string& sysfs_base,
                                                      const std::string& configured_vendor_product) {
    if (configured_vendor_product.empty()) {
        return find_hid_keyboard_devices(dev_base, sysfs_base);
    }

    // Parse "vendor:product" string
    auto colon = configured_vendor_product.find(':');
    if (colon == std::string::npos) {
        spdlog::warn("[InputScanner] Invalid configured_vendor_product format: {}",
                     configured_vendor_product);
        return find_hid_keyboard_devices(dev_base, sysfs_base);
    }
    std::string target_vendor = configured_vendor_product.substr(0, colon);
    std::string target_product = configured_vendor_product.substr(colon + 1);

    // Scan for the configured device
    auto dir = std::unique_ptr<DIR, decltype(&closedir)>(opendir(dev_base.c_str()), closedir);
    if (!dir) return find_hid_keyboard_devices(dev_base, sysfs_base);

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0) continue;

        std::string device_path = dev_base + "/" + entry->d_name;
        if (access(device_path.c_str(), R_OK) != 0) continue;

        int bus = read_bus_type(sysfs_base, event_num);
        if (bus != BUS_USB && bus != BUS_BLUETOOTH) continue;

        std::string vendor = read_vendor_id(sysfs_base, event_num);
        std::string product = read_product_id(sysfs_base, event_num);

        if (vendor == target_vendor && product == target_product) {
            std::string name = read_device_name(sysfs_base, event_num);
            spdlog::info("[InputScanner] Found configured scanner device: {} ({}) "
                         "vendor={} product={}", device_path, name, vendor, product);
            return {{device_path, name, event_num}};
        }
    }

    spdlog::info("[InputScanner] Configured device {}:{} not found, falling back to auto-detect",
                 target_vendor, target_product);
    return find_hid_keyboard_devices(dev_base, sysfs_base);
}

std::string read_sysfs_line(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return "";
    std::string line;
    std::getline(file, line);
    return line;
}

std::string get_input_device_name(int event_num) {
    std::string path = "/sys/class/input/event" + std::to_string(event_num) + "/device/name";
    return read_sysfs_line(path);
}

std::string get_input_device_phys(int event_num) {
    std::string path = "/sys/class/input/event" + std::to_string(event_num) + "/device/phys";
    return read_sysfs_line(path);
}

std::vector<UsbHidDevice> enumerate_usb_hid_devices(const std::string& dev_base,
                                                     const std::string& sysfs_base) {
    std::vector<UsbHidDevice> devices;

    auto dir = std::unique_ptr<DIR, decltype(&closedir)>(opendir(dev_base.c_str()), closedir);
    if (!dir) {
        spdlog::debug("[InputScanner] Cannot open {}", dev_base);
        return devices;
    }

    struct dirent* entry;
    while ((entry = readdir(dir.get())) != nullptr) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        int event_num = -1;
        if (sscanf(entry->d_name, "event%d", &event_num) != 1 || event_num < 0) continue;

        std::string device_path = dev_base + "/" + entry->d_name;
        if (access(device_path.c_str(), R_OK) != 0) continue;

        int bus = read_bus_type(sysfs_base, event_num);
        if (bus != BUS_USB && bus != BUS_BLUETOOTH) continue;

        std::string key_caps = read_sysfs_capability(sysfs_base, event_num, "key");
        if (!check_capability_bit(key_caps, 30)) continue;

        std::string abs_caps = read_sysfs_capability(sysfs_base, event_num, "abs");
        bool has_legacy_abs = check_capability_bit(abs_caps, 0) && check_capability_bit(abs_caps, 1);
        bool has_mt_abs = check_capability_bit(abs_caps, 53) && check_capability_bit(abs_caps, 54);
        if (has_legacy_abs || has_mt_abs) continue;

        std::string name = read_device_name(sysfs_base, event_num);
        std::string vendor = read_vendor_id(sysfs_base, event_num);
        std::string product = read_product_id(sysfs_base, event_num);

        spdlog::info("[InputScanner] Enumerated USB HID device: {} ({}) vendor={} product={}",
                     device_path, name, vendor, product);
        devices.push_back({std::move(name), std::move(vendor), std::move(product),
                           std::move(device_path)});
    }

    return devices;
}

std::vector<UsbHidDevice> enumerate_usb_hid_devices() {
    return enumerate_usb_hid_devices("/dev/input", "/sys/class/input");
}

bool get_input_touch_capabilities(int event_num, helix::AbsCapabilities* caps_out) {
    std::string path =
        "/sys/class/input/event" + std::to_string(event_num) + "/device/capabilities/abs";
    std::string caps = read_sysfs_line(path);
    if (caps.empty()) return false;

    auto result = helix::parse_abs_capabilities(caps);
    if (caps_out) *caps_out = result;

    if (result.has_multitouch && !result.has_single_touch) {
        spdlog::debug("[InputScanner] event{}: MT-only touchscreen detected "
                      "(no legacy ABS_X/ABS_Y)",
                      event_num);
    }

    return result.has_single_touch || result.has_multitouch;
}

}  // namespace helix::input
