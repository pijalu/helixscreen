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

    // Each word is an unsigned long printed in hex. Word width varies by arch
    // (8 hex digits on 32-bit, 16 on 64-bit). We detect from the actual content.
    // Approach: iterate words right-to-left, tracking cumulative bit offset.
    // This is correct even when the kernel omits leading zeros, because a bit
    // can only appear set in a word whose hex representation includes that
    // bit position.
    int cumulative_bit = 0;
    for (int i = static_cast<int>(words.size()) - 1; i >= 0; --i) {
        unsigned long val = std::strtoul(words[i].c_str(), nullptr, 16);
        int bits_in_word = static_cast<int>(words[i].size()) * 4;  // 4 bits per hex digit

        if (bit >= cumulative_bit && bit < cumulative_bit + bits_in_word) {
            int bit_in_word = bit - cumulative_bit;
            return (val & (1UL << bit_in_word)) != 0;
        }

        cumulative_bit += bits_in_word;
    }

    return false;
}

std::optional<ScannedDevice> find_mouse_device(const std::string& dev_base,
                                                const std::string& sysfs_base) {
    // STUB: will be implemented in Phase 2
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
