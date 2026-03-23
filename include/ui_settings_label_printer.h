// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "bluetooth_plugin.h"
#include "lvgl/lvgl.h"
#include "mdns_discovery.h"
#include "overlay_base.h"
#include "usb_printer_detector.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace helix::settings {

class LabelPrinterSettingsOverlay : public OverlayBase {
  public:
    LabelPrinterSettingsOverlay();
    ~LabelPrinterSettingsOverlay() override;

    // OverlayBase interface
    void init_subjects() override;
    void register_callbacks() override;
    const char* get_name() const override { return "Label Printer"; }
    void on_activate() override;
    void on_deactivate() override;

    // UI creation
    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);
    bool is_created() const { return overlay_root_ != nullptr; }

    // Event handlers (public for static callbacks)
    void handle_address_changed();
    void handle_port_changed();
    void handle_label_size_changed(int index);
    void handle_preset_changed(int index);
    void handle_test_print();
    void handle_printer_selected(int index);
    void handle_type_changed(int index);
    void handle_usb_printer_selected(int index);
    void handle_bt_printer_selected(int index);
    void handle_bt_scan();
    void handle_bt_connect();

  private:
    void init_address_input();
    void init_port_input();
    void init_label_size_dropdown();
    void init_preset_dropdown();
    void init_discovery_dropdown();
    void init_printer_type_dropdown();
    void init_usb_printer_dropdown();
    void init_bt_printer_dropdown();
    void start_label_printer_discovery();
    void stop_label_printer_discovery();
    void start_usb_detection();
    void stop_usb_detection();
    void start_bt_discovery();
    void stop_bt_discovery();
    void on_printers_discovered(const std::vector<DiscoveredPrinter>& printers);
    void on_usb_printers_detected(const std::vector<helix::UsbPrinterInfo>& printers);

    bool inputs_initialized_ = false; ///< Guard against stacking duplicate event callbacks

    // mDNS discovery for Brother QL printers
    std::unique_ptr<IMdnsDiscovery> mdns_discovery_;
    std::vector<DiscoveredPrinter> discovered_printers_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    // USB printer detection
    std::unique_ptr<helix::UsbPrinterDetector> usb_detector_;
    std::vector<helix::UsbPrinterInfo> detected_usb_printers_;

    // Bluetooth discovery
    /// Info about a discovered BT printer (UI thread copies)
    struct BtDeviceInfo {
        std::string mac;
        std::string name;
        bool paired = false;
        bool connected = false;
        bool is_ble = false;
    };

    /// C callback safety: prevent use-after-free when overlay is destroyed during discovery
    struct BtDiscoveryContext {
        std::atomic<bool> alive{true};
        LabelPrinterSettingsOverlay* overlay = nullptr;
    };

    helix_bt_context* bt_ctx_ = nullptr;
    std::unique_ptr<BtDiscoveryContext> bt_discovery_ctx_;
    std::vector<BtDeviceInfo> bt_devices_;
    bool bt_discovering_ = false;
    lv_subject_t bt_scanning_subject_{}; ///< 0=idle, 1=scanning

    // Static callbacks
    static void on_address_done(lv_event_t* e);
    static void on_port_done(lv_event_t* e);
    static void on_label_size_changed(lv_event_t* e);
    static void on_preset_changed(lv_event_t* e);
    static void on_test_print(lv_event_t* e);
    static void on_printer_selected(lv_event_t* e);
    static void on_type_changed(lv_event_t* e);
    static void on_usb_printer_selected(lv_event_t* e);
    static void on_bt_printer_selected(lv_event_t* e);
    static void on_bt_scan(lv_event_t* e);
    static void on_bt_connect(lv_event_t* e);

    // Pairing modal callbacks
    static void on_pair_confirm(lv_event_t* e);
    static void on_pair_cancel(lv_event_t* e);
};

LabelPrinterSettingsOverlay& get_label_printer_settings_overlay();

} // namespace helix::settings
