// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "data_root_resolver.h"

#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace helix {

bool is_valid_data_root(const std::string& dir) {
    if (dir.empty()) {
        return false;
    }
    std::string path = dir + "/ui_xml";
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}

std::string resolve_data_root_from_exe(const std::string& exe_path) {
    if (exe_path.empty()) {
        return {};
    }

    // Strip the binary filename to get the directory
    auto last_slash = exe_path.rfind('/');
    if (last_slash == std::string::npos) {
        return {};
    }
    std::string bin_dir = exe_path.substr(0, last_slash);

    // Try stripping known binary directory suffixes to find the project root.
    // Order matters: /build/bin is more specific than /bin, so try it first.
    //   Dev builds:    /path/to/project/build/bin/helix-screen  → /path/to/project
    //   Deployed:      /home/pi/helixscreen/bin/helix-screen    → /home/pi/helixscreen
    const char* suffixes[] = {"/build/bin", "/bin"};
    for (const char* suffix : suffixes) {
        size_t suffix_len = strlen(suffix);
        if (bin_dir.size() >= suffix_len &&
            bin_dir.compare(bin_dir.size() - suffix_len, suffix_len, suffix) == 0) {
            std::string candidate = bin_dir.substr(0, bin_dir.size() - suffix_len);
            if (is_valid_data_root(candidate)) {
                return candidate;
            }
        }
    }

    return {};
}

std::string get_user_config_dir() {
    const char* env_dir = std::getenv("HELIX_CONFIG_DIR");
    if (env_dir != nullptr && env_dir[0] != '\0') {
        return env_dir;
    }
    return "config";
}

} // namespace helix
