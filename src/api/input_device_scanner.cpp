// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_device_scanner.h"

#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

std::string read_sysfs_capability(const std::string& sysfs_base, int event_num,
                                   const std::string& cap_name) {
    std::string path = sysfs_base + "/event" + std::to_string(event_num) +
                       "/device/capabilities/" + cap_name;
    std::ifstream file(path);
    std::string line;
    if (file.good() && std::getline(file, line)) {
        return line;
    }
    return "";
}

std::string read_device_name(const std::string& sysfs_base, int event_num) {
    std::string path =
        sysfs_base + "/event" + std::to_string(event_num) + "/device/name";
    std::ifstream file(path);
    std::string line;
    if (file.good() && std::getline(file, line)) {
        return line;
    }
    return "";
}

}  // namespace

namespace helix::input {

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
    // zeros, so "0" could be 32-bit or 64-bit. For single-word bitmasks, we
    // use the actual digit count. For multi-word bitmasks, we infer the arch
    // word width from the longest word and apply it uniformly.
    int bits_per_word = 0;
    if (words.size() == 1) {
        bits_per_word = static_cast<int>(words[0].size()) * 4;
    } else {
        // Find max hex digit count to infer arch word width
        size_t max_digits = 0;
        for (const auto& w : words) {
            if (w.size() > max_digits) {
                max_digits = w.size();
            }
        }
        // Round up to 32-bit (8 digits) or 64-bit (16 digits) boundary
        if (max_digits <= 8) {
            bits_per_word = 32;
        } else {
            bits_per_word = 64;
        }
    }

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
    DIR* dir = opendir(dev_base.c_str());
    if (!dir) {
        spdlog::debug("[InputScanner] Cannot open {}", dev_base);
        return std::nullopt;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
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

        // Skip touchscreens: devices with ABS_X (bit 0) + ABS_Y (bit 1)
        std::string abs_caps = read_sysfs_capability(sysfs_base, event_num, "abs");
        if (check_capability_bit(abs_caps, 0) && check_capability_bit(abs_caps, 1)) {
            continue;
        }

        // Require REL_X (bit 0) + REL_Y (bit 1)
        std::string rel_caps = read_sysfs_capability(sysfs_base, event_num, "rel");
        if (!check_capability_bit(rel_caps, 0) || !check_capability_bit(rel_caps, 1)) {
            continue;
        }

        // Require BTN_LEFT (bit 272)
        std::string key_caps = read_sysfs_capability(sysfs_base, event_num, "key");
        if (!check_capability_bit(key_caps, 272)) {
            continue;
        }

        std::string name = read_device_name(sysfs_base, event_num);
        spdlog::info("[InputScanner] Found mouse: {} ({})", device_path, name);

        closedir(dir);
        return ScannedDevice{device_path, name, event_num};
    }

    closedir(dir);
    return std::nullopt;
}

std::optional<ScannedDevice> find_mouse_device() {
    return find_mouse_device("/dev/input", "/sys/class/input");
}

std::optional<ScannedDevice> find_keyboard_device(const std::string& dev_base,
                                                   const std::string& sysfs_base) {
    // STUB: will be implemented in Phase 3
    return std::nullopt;
}

std::optional<ScannedDevice> find_keyboard_device() {
    return find_keyboard_device("/dev/input", "/sys/class/input");
}

}  // namespace helix::input
