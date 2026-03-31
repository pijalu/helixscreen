// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"

#include <array>
#include <string>
#include <vector>

/**
 * @file ams_backend_snapmaker.h
 * @brief Snapmaker U1 SnapSwap toolchanger backend
 *
 * The Snapmaker U1 is a 4-toolhead printer with custom Klipper extensions.
 * Each extruder has state fields (park_pin, active_pin, activating_move)
 * and RFID tags provide filament info per channel.
 *
 * Klipper Objects:
 * - extruder0..3 with custom fields: state, park_pin, active_pin,
 *   activating_move, extruder_offset, switch_count, retry_count, error_count
 * - filament_detect with info/state per channel and filament_feed left/right
 * - toolchanger, toolhead, print_task_config
 *
 * Path topology is PARALLEL (each tool has its own independent path).
 */

/// Per-extruder tool state from Snapmaker custom Klipper fields
struct ExtruderToolState {
    std::string state;                  ///< e.g., "PARKED", "ACTIVE", "ACTIVATING"
    bool park_pin = false;              ///< Tool is in park position
    bool active_pin = false;            ///< Tool is in active position
    bool activating_move = false;       ///< Tool change move in progress
    std::array<float, 3> extruder_offset = {0, 0, 0}; ///< XYZ offset
    int switch_count = 0;               ///< Total tool changes for this extruder
    int retry_count = 0;                ///< Tool change retries
    int error_count = 0;                ///< Tool change errors
};

/// RFID tag data parsed from filament_detect info
struct SnapmakerRfidInfo {
    std::string main_type;       ///< e.g., "PLA", "PETG"
    std::string sub_type;        ///< e.g., "SnapSpeed", "Basic"
    std::string manufacturer;    ///< e.g., "Polymaker"
    std::string vendor;          ///< e.g., "Snapmaker"
    uint32_t color_rgb = 0x808080; ///< RGB color (ARGB masked to 0x00FFFFFF)
    int hotend_min_temp = 0;
    int hotend_max_temp = 0;
    int bed_temp = 0;
    int weight_g = 0;            ///< Spool weight in grams
};

class AmsBackendSnapmaker : public AmsSubscriptionBackend {
  public:
    AmsBackendSnapmaker(MoonrakerAPI* api, helix::MoonrakerClient* client);

    [[nodiscard]] AmsType get_type() const override { return AmsType::SNAPMAKER; }

    // State queries
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // Path visualization (PARALLEL topology — each tool is independent)
    [[nodiscard]] PathTopology get_topology() const override { return PathTopology::PARALLEL; }
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    // Operations
    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index = -1) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;

    // Recovery (not supported)
    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    // Configuration
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Bypass (not applicable for tool changers)
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override { return false; }

    // Static parsers (public for testing)
    static ExtruderToolState parse_extruder_state(const nlohmann::json& json);
    static SnapmakerRfidInfo parse_rfid_info(const nlohmann::json& json);

  protected:
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override { return "[AMS Snapmaker]"; }

  private:
    static constexpr int NUM_TOOLS = 4;

    /// Per-extruder cached state
    std::array<ExtruderToolState, NUM_TOOLS> extruder_states_;

    /// Validate slot index is within range
    AmsError validate_slot_index(int slot_index) const;
};
