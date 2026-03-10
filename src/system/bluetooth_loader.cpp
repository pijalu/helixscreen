// SPDX-License-Identifier: GPL-3.0-or-later

#include "bluetooth_loader.h"

#include <spdlog/spdlog.h>

#include <dlfcn.h>
#include <unistd.h>

#include <climits>
#include <cstring>
#include <string>

namespace helix::bluetooth {

BluetoothLoader& BluetoothLoader::instance() {
    static BluetoothLoader instance;
    return instance;
}

BluetoothLoader::BluetoothLoader() {
    if (!has_bt_hardware()) {
        spdlog::info("[BluetoothLoader] No Bluetooth hardware detected (no /sys/class/bluetooth/hci0)");
        return;
    }
    spdlog::info("[BluetoothLoader] Bluetooth hardware detected");

    if (try_load()) {
        available_ = true;
        spdlog::info("[BluetoothLoader] Plugin loaded successfully");
    } else {
        spdlog::info("[BluetoothLoader] Plugin not available (expected on dev machines)");
    }
}

BluetoothLoader::~BluetoothLoader() {
    if (dl_handle_) {
        dlclose(dl_handle_);
        dl_handle_ = nullptr;
        spdlog::trace("[BluetoothLoader] Plugin unloaded");
    }
}

bool BluetoothLoader::is_available() const {
    return available_;
}

bool BluetoothLoader::has_bt_hardware() {
    return access("/sys/class/bluetooth/hci0", F_OK) == 0;
}

bool BluetoothLoader::try_load() {
    // Determine path to plugin .so next to the executable
    char exe_path[PATH_MAX] = {};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) {
        spdlog::warn("[BluetoothLoader] Cannot determine executable path");
        return false;
    }
    exe_path[len] = '\0';

    // Find directory containing the executable
    std::string dir(exe_path);
    auto slash = dir.rfind('/');
    if (slash != std::string::npos) {
        dir = dir.substr(0, slash + 1);
    } else {
        dir = "./";
    }

    std::string plugin_path = dir + "libhelix-bluetooth.so";
    spdlog::debug("[BluetoothLoader] Trying to load: {}", plugin_path);

    dl_handle_ = dlopen(plugin_path.c_str(), RTLD_NOW);
    if (!dl_handle_) {
        spdlog::debug("[BluetoothLoader] dlopen failed: {}", dlerror());
        return false;
    }

    // Resolve get_info first to verify API version
    auto get_info = reinterpret_cast<helix_bt_get_info_fn>(
        dlsym(dl_handle_, HELIX_BT_SYM_GET_INFO));
    if (!get_info) {
        spdlog::warn("[BluetoothLoader] Missing symbol: {}", HELIX_BT_SYM_GET_INFO);
        dlclose(dl_handle_);
        dl_handle_ = nullptr;
        return false;
    }

    auto* info = get_info();
    if (!info || info->api_version != HELIX_BT_API_VERSION) {
        spdlog::warn("[BluetoothLoader] API version mismatch: expected {}, got {}",
                      HELIX_BT_API_VERSION, info ? info->api_version : -1);
        dlclose(dl_handle_);
        dl_handle_ = nullptr;
        return false;
    }

    spdlog::debug("[BluetoothLoader] Plugin '{}' v{}, classic={}, ble={}",
                  info->name ? info->name : "unknown",
                  info->api_version, info->has_classic, info->has_ble);

    // Resolve all function pointers
    auto resolve = [this](const char* name) -> void* {
        void* sym = dlsym(dl_handle_, name);
        if (!sym) {
            spdlog::warn("[BluetoothLoader] Missing symbol: {}", name);
        }
        return sym;
    };

    init           = reinterpret_cast<helix_bt_init_fn>(resolve(HELIX_BT_SYM_INIT));
    deinit         = reinterpret_cast<helix_bt_deinit_fn>(resolve(HELIX_BT_SYM_DEINIT));
    discover       = reinterpret_cast<helix_bt_discover_fn>(resolve(HELIX_BT_SYM_DISCOVER));
    stop_discovery = reinterpret_cast<helix_bt_stop_discovery_fn>(resolve(HELIX_BT_SYM_STOP_DISCOVERY));
    pair           = reinterpret_cast<helix_bt_pair_fn>(resolve(HELIX_BT_SYM_PAIR));
    is_paired      = reinterpret_cast<helix_bt_is_paired_fn>(resolve(HELIX_BT_SYM_IS_PAIRED));
    connect_rfcomm = reinterpret_cast<helix_bt_connect_rfcomm_fn>(resolve(HELIX_BT_SYM_CONNECT_RFCOMM));
    connect_ble    = reinterpret_cast<helix_bt_connect_ble_fn>(resolve(HELIX_BT_SYM_CONNECT_BLE));
    ble_write      = reinterpret_cast<helix_bt_ble_write_fn>(resolve(HELIX_BT_SYM_BLE_WRITE));
    disconnect     = reinterpret_cast<helix_bt_disconnect_fn>(resolve(HELIX_BT_SYM_DISCONNECT));
    last_error     = reinterpret_cast<helix_bt_last_error_fn>(resolve(HELIX_BT_SYM_LAST_ERROR));

    // Verify required symbols loaded
    if (!init || !deinit) {
        spdlog::warn("[BluetoothLoader] Required symbols (init/deinit) not resolved");
        dlclose(dl_handle_);
        dl_handle_ = nullptr;
        return false;
    }

    return true;
}

}  // namespace helix::bluetooth
