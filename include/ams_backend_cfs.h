// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix::printer {

/// Material info from CFS RFID database
struct CfsMaterialInfo {
    std::string id;
    std::string brand;
    std::string name;
    std::string material_type;
    int min_temp = 0;
    int max_temp = 0;
};

/// Static material database + CFS utility functions
class CfsMaterialDb {
  public:
    static const CfsMaterialDb& instance();

    /// Lookup material by 5-digit ID (e.g., "01001")
    const CfsMaterialInfo* lookup(const std::string& id) const;

    /// Strip CFS material_type code prefix: "101001" -> "01001", "-1" -> ""
    static std::string strip_code(const std::string& code);

    /// Parse CFS color: "0RRGGBB" -> 0xRRGGBB, sentinels -> 0x808080
    static uint32_t parse_color(const std::string& color_str);

    /// Global slot index -> TNN name: 0 -> "T1A", 4 -> "T2A"
    static std::string slot_to_tnn(int global_index);

    /// TNN name -> global slot index: "T1A" -> 0, "T2A" -> 4, invalid -> -1
    static int tnn_to_slot(const std::string& tnn);

    /// Default color for unknown/sentinel slots
    static constexpr uint32_t DEFAULT_COLOR = 0x808080;

  private:
    CfsMaterialDb();
    void load_database();
    std::unordered_map<std::string, CfsMaterialInfo> materials_;
};

/// Decode CFS key8xx error codes into human-readable AmsAlerts
class CfsErrorDecoder {
  public:
    /// Decode a CFS error code. Returns nullopt for unknown codes.
    static std::optional<AmsAlert> decode(const std::string& key_code,
                                          int unit_index, int slot_index);
};

/// CFS (Creality Filament System) backend — K2 series printers with RS-485 CFS units
class AmsBackendCfs : public AmsSubscriptionBackend {
  public:
    AmsBackendCfs(MoonrakerAPI* api, helix::MoonrakerClient* client);

    [[nodiscard]] AmsType get_type() const override { return AmsType::CFS; }

    // State queries
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // Path visualization
    [[nodiscard]] PathTopology get_topology() const override { return PathTopology::HUB; }
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    // Operations
    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index = -1) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;
    AmsError reset() override;
    AmsError recover() override;
    AmsError cancel() override;

    // Slot management (not supported — RFID manages slot info)
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Bypass (not supported)
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override { return false; }

    // Capabilities
    [[nodiscard]] helix::printer::EndlessSpoolCapabilities
    get_endless_spool_capabilities() const override;
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;
    [[nodiscard]] bool supports_auto_heat_on_load() const override { return true; }
    [[nodiscard]] bool tracks_weight_locally() const override { return false; }
    [[nodiscard]] bool manages_active_spool() const override { return false; }
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;

    // Static parser (public for testing)
    static AmsSystemInfo parse_box_status(const nlohmann::json& box_json);

    // GCode helpers (public for testing)
    static std::string load_gcode(int global_slot_index);
    static std::string unload_gcode();
    static std::string reset_gcode();
    static std::string recover_gcode();

  protected:
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override { return "[AMS CFS]"; }
    void on_started() override;

  private:
    std::string current_tnn_;
    bool motor_ready_ = true;
};

} // namespace helix::printer
