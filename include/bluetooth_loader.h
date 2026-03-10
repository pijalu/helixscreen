// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bluetooth_plugin.h"

#include <string>

namespace helix::bluetooth {

/// Runtime loader for libhelix-bluetooth.so.
/// Checks for BT hardware, loads plugin via dlopen, resolves function pointers.
/// Zero overhead when BT unavailable — is_available() returns false, all ops are no-ops.
class BluetoothLoader {
  public:
    static BluetoothLoader& instance();

    /// Check if BT hardware exists AND plugin loaded successfully.
    [[nodiscard]] bool is_available() const;

    /// Plugin function pointers — only valid when is_available() == true.
    /// Caller must check is_available() before using these.
    helix_bt_init_fn init = nullptr;
    helix_bt_deinit_fn deinit = nullptr;
    helix_bt_discover_fn discover = nullptr;
    helix_bt_stop_discovery_fn stop_discovery = nullptr;
    helix_bt_pair_fn pair = nullptr;
    helix_bt_is_paired_fn is_paired = nullptr;
    helix_bt_connect_rfcomm_fn connect_rfcomm = nullptr;
    helix_bt_connect_ble_fn connect_ble = nullptr;
    helix_bt_ble_write_fn ble_write = nullptr;
    helix_bt_disconnect_fn disconnect = nullptr;
    helix_bt_last_error_fn last_error = nullptr;

    // Non-copyable
    BluetoothLoader(const BluetoothLoader&) = delete;
    BluetoothLoader& operator=(const BluetoothLoader&) = delete;

  private:
    BluetoothLoader();
    ~BluetoothLoader();

    bool try_load();
    static bool has_bt_hardware();

    void* dl_handle_ = nullptr;
    bool available_ = false;
};

}  // namespace helix::bluetooth
