// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix {

/**
 * @brief Resolve the HelixScreen data root directory from a binary path
 *
 * Given the absolute path to the helix-screen binary, strips known
 * binary directory suffixes (/build/bin, /bin) and validates that the
 * resulting directory contains a ui_xml/ subdirectory.
 *
 * @param exe_path Absolute path to the running binary
 * @return Resolved data root path, or empty string if not found
 */
std::string resolve_data_root_from_exe(const std::string& exe_path);

/**
 * @brief Check whether a directory is a valid HelixScreen data root
 *
 * A valid data root must contain a ui_xml/ subdirectory.
 *
 * @param dir Directory path to check
 * @return true if the directory contains ui_xml/
 */
bool is_valid_data_root(const std::string& dir);

/**
 * @brief Resolve the writable user-config directory
 *
 * Returns $HELIX_CONFIG_DIR if set and non-empty; otherwise returns the
 * relative default "config". Does not create the directory — callers are
 * responsible for fs::create_directories before writing.
 *
 * Used for runtime-mutable state (settings.json, tool_spools.json,
 * crash_history.json, user themes, pending_remap.json, etc.). On
 * read-only baselines (Yocto squashfs), HELIX_CONFIG_DIR points at a
 * writable partition (e.g. ~/printer_data/config/helixscreen).
 */
std::string get_user_config_dir();

} // namespace helix
