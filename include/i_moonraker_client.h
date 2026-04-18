// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "json_fwd.h"
#include "moonraker_error.h"
#include "moonraker_request_tracker.h" // for helix::RequestId
#include "moonraker_types.h"           // for ::GcodeStoreEntry

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace helix {

using ::json; // Make global json alias visible in this namespace

/**
 * @brief Abstract interface for the Moonraker WebSocket + JSON-RPC transport layer.
 *
 * Production and test consumers that only need polymorphic access to the Moonraker
 * transport should depend on this interface rather than the concrete MoonrakerClient.
 * The concrete class inherits this interface (alongside hv::WebSocketClient).
 *
 * The interface is intentionally narrow — it mirrors only the 10 methods currently
 * marked `virtual` on MoonrakerClient. Non-virtual helpers (hardware queries,
 * timeout configuration, event registration, identification state,
 * connection-generation accessors, etc.) and hv::WebSocketClient itself remain on
 * the concrete class.
 */
class IMoonrakerClient {
  public:
    virtual ~IMoonrakerClient() = default;

    // ========================================================================
    // Connection Lifecycle
    // ========================================================================

    /// @brief Connect to Moonraker WebSocket server
    virtual int connect(const char* url, std::function<void()> on_connected,
                        std::function<void()> on_disconnected) = 0;

    /// @brief Disconnect from Moonraker WebSocket server
    virtual void disconnect() = 0;

    // ========================================================================
    // JSON-RPC Protocol
    // ========================================================================

    /// @brief Send JSON-RPC request without parameters
    virtual int send_jsonrpc(const std::string& method) = 0;

    /// @brief Send JSON-RPC request with parameters
    virtual int send_jsonrpc(const std::string& method, const json& params) = 0;

    /// @brief Send JSON-RPC request with one-time response callback
    virtual RequestId send_jsonrpc(const std::string& method, const json& params,
                                   std::function<void(const json&)> cb) = 0;

    /// @brief Send JSON-RPC request with success and error callbacks
    virtual RequestId send_jsonrpc(const std::string& method, const json& params,
                                   std::function<void(const json&)> success_cb,
                                   std::function<void(const MoonrakerError&)> error_cb,
                                   uint32_t timeout_ms = 0, bool silent = false) = 0;

    /// @brief Send G-code script command
    virtual int gcode_script(const std::string& gcode) = 0;

    /// @brief Fetch G-code command history from Moonraker
    virtual void
    get_gcode_store(int count, std::function<void(const std::vector<GcodeStoreEntry>&)> on_success,
                    std::function<void(const MoonrakerError&)> on_error) = 0;

    // ========================================================================
    // Discovery
    // ========================================================================

    /// @brief Perform printer auto-discovery sequence
    virtual void
    discover_printer(std::function<void()> on_complete,
                     std::function<void(const std::string& reason)> on_error = nullptr) = 0;

    // ========================================================================
    // Simulation Hooks (for testing)
    // ========================================================================

    /// @brief Toggle filament runout simulation (no-op in production)
    virtual void toggle_filament_runout_simulation() = 0;
};

} // namespace helix
