// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "filament_database.h"

#include <string>
#include <unordered_map>

namespace helix {

/**
 * @brief Manages user overrides for material temperature settings
 *
 * Loads/saves sparse overrides from settings.json under "material_overrides".
 * The filament::get_material_override() bridge function delegates to this manager,
 * so all callers of filament::find_material() transparently get customized values.
 *
 * Thread safety: Single-threaded, main LVGL thread only.
 */
class MaterialSettingsManager {
  public:
    static MaterialSettingsManager& instance();

    // Non-copyable
    MaterialSettingsManager(const MaterialSettingsManager&) = delete;
    MaterialSettingsManager& operator=(const MaterialSettingsManager&) = delete;

    /** @brief Load overrides from config (call at startup before any find_material) */
    void init();

    /** @brief Get override for a material (nullptr if none) */
    const filament::MaterialOverride* get_override(const std::string& name) const;

    /** @brief Set override for a material (saves to config) */
    void set_override(const std::string& name, const filament::MaterialOverride& override);

    /** @brief Remove override for a material (saves to config) */
    void clear_override(const std::string& name);

    /** @brief Check if a material has any overrides */
    bool has_override(const std::string& name) const;

    /** @brief Get all overrides (for UI list display) */
    const std::unordered_map<std::string, filament::MaterialOverride>& get_all_overrides() const {
        return overrides_;
    }

  private:
    MaterialSettingsManager() = default;
    ~MaterialSettingsManager() = default;

    void load_from_config();
    void save_to_config();

    std::unordered_map<std::string, filament::MaterialOverride> overrides_;
    bool initialized_ = false;
};

} // namespace helix
