// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "async_lifetime_guard.h"
#include "moonraker_types.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

/**
 * @file ams_backend_ace.h
 * @brief ACE (Anycubic ACE Pro) backend implementation
 *
 * Implements the AmsBackend interface for AnyCubic ACE Pro systems
 * using the ValgACE/BunnyACE/DuckACE Klipper drivers.
 *
 * Primary path (ValgACE): Subscribes to the `ace` Klipper object via
 * standard Moonraker WebSocket. ValgACE implements get_status() which
 * returns combined state in a single object.
 *
 * Fallback path (BunnyACE/DuckACE): If the initial query returns empty
 * data (driver lacks get_status()), falls back to REST polling via the
 * ace_status.py Moonraker bridge at /server/ace/ endpoints.
 *
 * G-code Commands:
 * - ACE_CHANGE_TOOL TOOL={n}  - Load filament from slot n (-1 to unload)
 * - ACE_START_DRYING TEMP={t} DURATION={m}  - Start drying
 * - ACE_STOP_DRYING           - Stop drying
 *
 * Thread Model:
 * - Primary: WebSocket subscription callbacks on background thread
 * - Fallback: REST polling thread at ~500ms interval
 * - State is cached under mutex protection (inherited from AmsSubscriptionBackend)
 */
class AmsBackendAce : public AmsSubscriptionBackend {
  public:
    AmsBackendAce(MoonrakerAPI* api, helix::MoonrakerClient* client);

    ~AmsBackendAce() override;

    // ========================================================================
    // Type
    // ========================================================================

    [[nodiscard]] AmsType get_type() const override { return AmsType::ACE; }

    // ========================================================================
    // State Queries
    // ========================================================================

    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // ========================================================================
    // Path Visualization
    // ========================================================================

    [[nodiscard]] PathTopology get_topology() const override;
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    // ========================================================================
    // Filament Operations
    // ========================================================================

    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index = -1) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;

    // ========================================================================
    // Recovery Operations
    // ========================================================================

    AmsError recover() override;
    AmsError reset() override;
    AmsError cancel() override;

    // ========================================================================
    // Configuration
    // ========================================================================

    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // ACE has fixed 1:1 mapping (tools ARE slots), not configurable
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;
    [[nodiscard]] std::vector<int> get_tool_mapping() const override;

    // ========================================================================
    // Bypass Mode (not supported on ACE Pro)
    // ========================================================================

    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override;

    // ========================================================================
    // Environment Sensors & Dryer Control (ACE Pro has built-in dryer + temp)
    // ========================================================================

    [[nodiscard]] bool has_environment_sensors() const override { return true; }
    [[nodiscard]] DryerInfo get_dryer_info() const override;
    AmsError start_drying(float temp_c, int duration_min, int fan_pct = -1,
                           int unit = 0) override;
    AmsError stop_drying(int unit = 0) override;
    AmsError update_drying(float temp_c = -1, int duration_min = -1, int fan_pct = -1) override;
    [[nodiscard]] std::vector<DryingPreset> get_drying_presets() const override;

    // ========================================================================
    // Device Actions
    // ========================================================================

    [[nodiscard]] std::vector<helix::printer::DeviceSection> get_device_sections() const override;
    [[nodiscard]] std::vector<helix::printer::DeviceAction> get_device_actions() const override;
    AmsError execute_device_action(const std::string& action_id,
                                   const std::any& value = {}) override;

  protected:
    // ========================================================================
    // AmsSubscriptionBackend hooks
    // ========================================================================

    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override { return "[ACE]"; }
    void on_started() override;
    void on_stopping() override;

    // ========================================================================
    // Response Parsing (protected for unit testing)
    // ========================================================================

    /**
     * @brief Parse /server/ace/info response (REST fallback path)
     * @param data JSON response data
     */
    void parse_info_response(const nlohmann::json& data);

    /**
     * @brief Parse /server/ace/status response (REST fallback path)
     * @param data JSON response data
     * @return true if state changed (emit event)
     */
    bool parse_status_response(const nlohmann::json& data);

    /**
     * @brief Parse /server/ace/slots response (REST fallback path)
     * @param data JSON response data
     * @return true if state changed (emit event)
     */
    bool parse_slots_response(const nlohmann::json& data);

    /**
     * @brief Parse combined ace Klipper object data (WebSocket subscription path)
     *
     * ValgACE's get_status() returns all state in one object: model, firmware,
     * status, slots, dryer, etc. This handles that combined format.
     *
     * @param data JSON data from the ace Klipper object
     */
    void parse_ace_object(const nlohmann::json& data);

  private:
    // ========================================================================
    // REST Fallback (for BunnyACE/DuckACE without get_status())
    // ========================================================================

    void start_rest_fallback();
    void stop_rest_fallback();
    void rest_polling_loop();

    void poll_info();
    void poll_status();
    void poll_slots();

    // ========================================================================
    // Helpers
    // ========================================================================

    AmsError validate_slot_index(int slot_index) const;

    /**
     * @brief Parse slot color from either RGB array [r,g,b] or hex string "#RRGGBB"
     * @param color_val JSON value (array or string)
     * @return Parsed RGB color value
     */
    static uint32_t parse_slot_color(const nlohmann::json& color_val);

    // ========================================================================
    // Members
    // ========================================================================

    // Dryer state (ACE-specific, not in base class)
    DryerInfo dryer_info_;

    // Info tracking
    std::atomic<bool> info_fetched_{false};
    std::atomic<int> info_fetch_failures_{0};

    // Callback lifetime management
    helix::AsyncLifetimeGuard lifetime_;

    // REST fallback state
    bool use_rest_fallback_{false};
    std::thread rest_polling_thread_;
    std::atomic<bool> rest_stop_requested_{false};
    std::condition_variable rest_stop_cv_;
    std::mutex rest_stop_mutex_;

    // Configuration
    static constexpr int POLL_INTERVAL_MS = 500;

    // User slot overrides: ACE hardware doesn't support set_slot_info via gcode,
    // so user edits (color, material, weight) are stored in-memory and merged
    // over hardware-reported values during parse. Cleared when slot status changes
    // (e.g., spool physically swapped).
    struct SlotOverride {
        uint32_t color_rgb = 0;
        std::string material;
        std::string color_name;
        std::string brand;
        std::string spool_name;
        int spoolman_id = 0;
        float remaining_weight_g = -1;
        float total_weight_g = -1;
    };
    std::unordered_map<int, SlotOverride> slot_overrides_;
    std::unordered_map<int, SlotStatus> prev_slot_status_;

    // Slot override persistence (Moonraker DB + local JSON fallback)
    void save_slot_overrides();
    void load_slot_overrides();
    void save_slot_overrides_json() const;
    bool load_slot_overrides_json();
    nlohmann::json slot_overrides_to_json() const;
    void apply_slot_overrides_json(const nlohmann::json& data);
};
