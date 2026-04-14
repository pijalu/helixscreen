// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#if HELIX_HAS_IFS

#include "ams_subscription_backend.h"
#include "slot_registry.h"

#include <array>
#include <atomic>
#include <chrono>
#include <string>

class Ad5xIfsTestAccess;

/// AMS backend for FlashForge Adventurer 5X IFS (Intelligent Filament Switching).
///
/// IFS is a 4-lane filament switching system controlled by a separate STM32 MCU,
/// driven through ZMOD's zmod_ifs.py Klipper module.
///
/// === Stock zMod vs plugin variants (IMPORTANT for Moonraker visibility) ===
///
/// Stock zMod owns two Klipper objects, `zmod_ifs` and `zmod_color`, that hold
/// the authoritative per-channel state:
///   - zmod_ifs.ifs_data.get_port(port)         -> per-channel HUB presence switch
///   - zmod_ifs.get_ifs_sensor(port)            -> per-channel motion/stall sensor
///                                                 (located INSIDE the IFS, just
///                                                 after the hub — NOT at the toolhead)
///   - zmod_ifs.get_extruder_sensor()           -> toolhead filament switch
///   - zmod_ifs.get_prutok_type_from_config(p)  -> per-channel material string
///   - zmod_color.get_current_channel()         -> active channel (1-based)
///   - zmod_color.get_printer_data_detail()     -> hasMatlStation, indepMatlInfo, ...
///
/// These are `printer.lookup_object()`-only Python APIs. They are NOT exposed via
/// `get_status()`, so Moonraker (and therefore HelixScreen) cannot subscribe to
/// them directly. Stock zMod only gives Moonraker:
///   - filament_motion_sensor ifs_motion_sensor   (single boolean, post-hub)
///   - filament_switch_sensor head_switch_sensor  (toolhead)
///   - Adventurer5M.json                          (polled via Moonraker file API)
///
/// The lessWaste / bambufy plugins close this gap. They are effectively a
/// Moonraker exporter for zmod_ifs/zmod_color, publishing:
///   - filament_switch_sensor _ifs_port_sensor_{1-4}  per-port HUB presence
///     (wraps zmod_ifs.ifs_data.get_port)
///   - save_variables with <prefix>_colors, _types, _tools, _current_tool,
///     _external   (prefix = "less_waste" or "bambufy"; schema identical)
///   - _IFS_VARS gcode macro for atomic writes of the above
///
/// Plugin delta over stock zMod (via Moonraker):
///   (1) per-channel HUB presence as 4 separate booleans
///   (2) live tool->port mapping (16 slots)
///   (3) active tool index with push notifications
///   (4) bypass/external flag
///   (5) atomic, subscribable color+material updates
/// Everything else — including the toolhead switch — is shared with stock zMod.
///
/// === Sensor -> PathSegment mapping ===
///
///   head_switch_sensor        -> TOOLHEAD / NOZZLE (at toolhead)
///   _ifs_port_sensor_{1..4}   -> HUB               (per-channel, plugin only)
///   ifs_motion_sensor         -> OUTPUT            (post-hub, NOT toolhead;
///                                                   single boolean on stock zMod)
///
/// NOTE: `parse_head_sensor()` currently conflates `ifs_motion_sensor` with the
/// toolhead switch. That is a known simplification — motion at the hub does not
/// mean filament has reached the nozzle. Fixing this requires splitting a
/// hub_output presence from head_filament and updating
/// `system_info_.filament_loaded` + `detect_load_unload_completion()` accordingly.
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
    [[nodiscard]] AmsType get_type() const override {
        return AmsType::AD5X_IFS;
    }
    [[nodiscard]] PathTopology get_topology() const override {
        return PathTopology::LINEAR;
    }
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

    // IFS firmware persists color + material type but NOT spoolman_id,
    // so ToolState must handle spool assignment persistence via Moonraker DB.
    [[nodiscard]] bool has_firmware_spool_persistence() const override {
        return false;
    }

  protected:
    void on_started() override;
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override {
        return "[AMS AD5X-IFS]";
    }

  private:
    friend class Ad5xIfsTestAccess;

    void parse_save_variables(const nlohmann::json& vars);
    void parse_port_sensor(int port_1based, bool detected);
    void parse_head_sensor(bool detected);
    void update_slot_from_state(int slot_index);
    void parse_adventurer_json(const std::string& content);
    void read_adventurer_json();
    void register_zcolor_listener();
    void schedule_json_reread();

    std::string build_color_list_value() const;
    std::string build_type_list_value() const;
    std::string build_tool_map_value() const;
    AmsError write_ifs_var(const std::string& key, const std::string& value);
    AmsError write_adventurer_json(int slot_index);
    void detect_load_unload_completion(bool head_detected);

    int find_first_tool_for_port(int port_1based) const;
    bool validate_slot_index(int slot_index) const;
    void check_action_timeout();

    // Cached state from save_variables
    // Variable prefix: "less_waste" (lessWaste/zmod) or "bambufy" — auto-detected from
    // whichever save_variables are present on the printer.
    std::string var_prefix_ = "less_waste";
    std::array<std::string, NUM_PORTS> colors_;    // Hex strings: "FF0000"
    std::array<std::string, NUM_PORTS> materials_; // Material names: "PLA"
    std::array<int, TOOL_MAP_SIZE> tool_map_;      // tool_map_[tool] = port (1-4, 5=unmapped)
    std::array<bool, NUM_PORTS> port_presence_;    // Per-port filament sensor state
    int active_tool_ = -1;                         // Current tool (-1 = none)
    bool external_mode_ = false;                   // Bypass/external spool mode
    bool head_filament_ = false;                   // Head sensor state
    std::array<bool, NUM_PORTS> dirty_{}; // Per-slot dirty flag to prevent stale overwrites

    helix::printer::SlotRegistry slots_;

    // Native ZMOD IFS has no per-port sensors — infer port presence from active
    // tool + head sensor state so the UI doesn't show all slots as EMPTY.
    bool has_per_port_sensors_ = false;

    // True if _IFS_VARS macro is available (lessWaste or bambufy plugin).
    // False for native ZMOD, which stores color/type in Adventurer5M.json
    // (read/written via Moonraker HTTP file API).
    bool has_ifs_vars_ = false;
    std::atomic<bool> reread_pending_{false};

    // Action timeout tracking
    static constexpr int ACTION_TIMEOUT_SECONDS = 90;
    std::chrono::steady_clock::time_point action_start_time_;

    // Note: uses inherited lifetime_ from AmsSubscriptionBackend (not shadowed).
};

#endif // HELIX_HAS_IFS
