// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"

#include "async_lifetime_guard.h"
#include "input_device_scanner.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct helix_bt_context;

namespace helix::ui {

/// Modal that lists connected USB and Bluetooth HID devices for scanner selection.
/// USB devices are enumerated from sysfs. BT devices are discovered via BluetoothLoader.
/// Shows auto-detect option at top, then each discovered device with transport badge.
class ScannerPickerModal : public Modal {
  public:
    using SelectionCallback =
        std::function<void(const std::string& vendor_product, const std::string& device_name)>;

    explicit ScannerPickerModal(SelectionCallback on_select);
    ~ScannerPickerModal() override;

    const char* get_name() const override {
        return "Scanner Picker";
    }
    const char* component_name() const override {
        return "scanner_picker_modal";
    }

  protected:
    void on_show() override;

  private:
    // Device list
    void populate_device_list();
    void add_device_row(lv_obj_t* list, const std::string& label, const std::string& sublabel,
                        const std::string& vendor_product, bool is_bluetooth = false,
                        const std::string& bt_mac = "");
    void handle_device_selected(const std::string& vendor_product, const std::string& device_name,
                                const std::string& bt_mac);

    // Bluetooth discovery
    void start_bt_discovery();
    void stop_bt_discovery();

    // Bluetooth pairing
    void pair_bt_device(const std::string& mac, const std::string& name);

    // Pairing modal callbacks
    static void on_pair_confirm(lv_event_t* e);
    static void on_pair_cancel(lv_event_t* e);

    SelectionCallback on_select_;
    std::string current_device_id_;
    lv_obj_t* device_list_ = nullptr;
    lv_obj_t* empty_state_ = nullptr;
    lv_obj_t* bt_scan_btn_ = nullptr;
    lv_obj_t* bt_spinner_ = nullptr;

    // Bluetooth state
    struct BtDeviceInfo {
        std::string mac;
        std::string name;
        std::string vendor_product; // populated after pairing when device appears as HID
        bool paired = false;
        bool is_ble = false;
    };

    struct BtDiscoveryContext {
        std::atomic<bool> alive{true};
        ScannerPickerModal* modal = nullptr;
    };

    helix_bt_context* bt_ctx_ = nullptr;
    std::unique_ptr<BtDiscoveryContext> bt_discovery_ctx_;
    std::vector<BtDeviceInfo> bt_devices_;
    bool bt_discovering_ = false;
    AsyncLifetimeGuard lifetime_;

    /// Active instance pointer (only one picker modal open at a time)
    static ScannerPickerModal* s_active_instance_;
};

} // namespace helix::ui
