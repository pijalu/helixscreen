// SPDX-License-Identifier: GPL-3.0-or-later
#include "moonraker_config_manager.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace helix {

// Trim leading and trailing whitespace (spaces, tabs, carriage returns)
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool MoonrakerConfigManager::has_section(
    const std::string& content, const std::string& section_name) {
    const std::string target = "[" + section_name + "]";
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t == target) return true;
    }
    return false;
}

std::string MoonrakerConfigManager::add_section(const std::string& content,
    const std::string& section_name,
    const std::vector<std::pair<std::string, std::string>>& entries,
    const std::string& comment) {
    // Stub — implemented in Task 2
    return content;
}

std::string MoonrakerConfigManager::remove_section(
    const std::string& content, const std::string& section_name) {
    // Stub — implemented in Task 3
    return content;
}

bool MoonrakerConfigManager::has_include_line(const std::string& moonraker_content) {
    // Stub — implemented in Task 4
    return false;
}

std::string MoonrakerConfigManager::add_include_line(const std::string& moonraker_content) {
    // Stub — implemented in Task 4
    return moonraker_content;
}

std::string MoonrakerConfigManager::get_section_value(
    const std::string& content, const std::string& section_name, const std::string& key) {
    // Stub — implemented in Task 4
    return "";
}

} // namespace helix
