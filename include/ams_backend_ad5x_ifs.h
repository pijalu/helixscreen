// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "slot_registry.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>

class Ad5xIfsTestAccess;

/// AMS backend for FlashForge Adventurer 5X IFS (Intelligent Filament Switching).
///
/// IFS is a 4-lane filament switching system controlled by a separate STM32 MCU,
/// driven through ZMOD's zmod_ifs.py Klipper module. Two firmware variants exist:
///
/// lessWaste plugin (has per-port sensors):
/// - save_variables: colors, materials, tool mapping, current tool, external mode
/// - filament_switch_sensor _ifs_port_sensor_{1-4}: per-port filament presence
/// - filament_switch_sensor head_switch_sensor: filament at toolhead
///
/// Native ZMOD IFS (no per-port sensors):
/// - save_variables: same as above
/// - filament_motion_sensor ifs_motion_sensor: toolhead filament presence
/// - filament_switch_sensor head_switch_sensor: filament at toolhead
///
/// Ports are 1-based (1-4), slots are 0-based (0-3).
/// slot_to_port = slot + 1, port_to_slot = port - 1.
class AmsBackendAd5xIfs : public AmsSubscriptionBackend {
  public:
    AmsBackendAd5xIfs(MoonrakerAPI* api, helix::MoonrakerClient* client);
    ~AmsBackendAd5xIfs() override;

    static constexpr int NUM_PORTS = 4;
    static constexpr int TOOL_MAP_SIZE = 16;
    static constexpr int UNMAPPED_PORT = 5;

    // --- AmsBackend interface ---
    [[nodiscard]] AmsType get_type() const override { return AmsType::AD5X_IFS; }
    [[nodiscard]] PathTopology get_topology() const override { return PathTopology::LINEAR; }
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;
    [[nodiscard]] bool is_bypass_active() const override;

    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index = -1) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;

    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;

    [[nodiscard]] bool has_firmware_spool_persistence() const override { return true; }

  protected:
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override { return "[AMS AD5X-IFS]"; }

  private:
    friend class Ad5xIfsTestAccess;

    void parse_save_variables(const nlohmann::json& vars);
    void parse_port_sensor(int port_1based, bool detected);
    void parse_head_sensor(bool detected);
    void update_slot_from_state(int slot_index);

    std::string build_color_list_value() const;
    std::string build_type_list_value() const;
    std::string build_tool_map_value() const;
    AmsError write_save_variable(const std::string& name, const std::string& value);

    int find_first_tool_for_port(int port_1based) const;
    bool validate_slot_index(int slot_index) const;
    void check_action_timeout();
    AmsError ensure_homed_then(std::string gcode);

    // Cached state from save_variables
    std::array<std::string, NUM_PORTS> colors_;    // Hex strings: "FF0000"
    std::array<std::string, NUM_PORTS> materials_; // Material names: "PLA"
    std::array<int, TOOL_MAP_SIZE> tool_map_;      // tool_map_[tool] = port (1-4, 5=unmapped)
    std::array<bool, NUM_PORTS> port_presence_;    // Per-port filament sensor state
    int active_tool_ = -1;                         // Current tool (-1 = none)
    bool external_mode_ = false;                   // Bypass/external spool mode
    bool head_filament_ = false;                   // Head sensor state

    helix::printer::SlotRegistry slots_;

    // Action timeout tracking
    static constexpr int ACTION_TIMEOUT_SECONDS = 90;
    std::chrono::steady_clock::time_point action_start_time_;

    // Async callback safety: shared flag cleared on destruction
    std::shared_ptr<std::atomic<bool>> alive_;
};
