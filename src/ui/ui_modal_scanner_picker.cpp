// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_modal_scanner_picker.h"

#include "ui_event_safety.h"
#include "ui_icon_codepoints.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "bluetooth_loader.h"
#include "bt_scanner_discovery_utils.h"
#include "settings_manager.h"
#include "theme_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <thread>

namespace helix::ui {

ScannerPickerModal* ScannerPickerModal::s_active_instance_ = nullptr;

// ============================================================================
// RowData — attached to each clickable row via lv_obj_set_user_data()
// ============================================================================

struct RowData {
    std::string vendor_product; // "" = auto-detect
    std::string device_name;
    std::string bt_mac; // empty for USB devices
    ScannerPickerModal* modal;
};

// ============================================================================
// PairData — user_data for the pairing confirmation modal callbacks
// ============================================================================

struct PairData {
    std::string mac;
    std::string name;
};

// ============================================================================
// CONSTRUCTION
// ============================================================================

ScannerPickerModal::ScannerPickerModal(SelectionCallback on_select)
    : on_select_(std::move(on_select)) {
    // Read current selection from SettingsManager
    current_device_id_ = helix::SettingsManager::instance().get_scanner_device_id();

    // Register XML callbacks BEFORE show() creates the XML component.
    // Modal::show() calls create_and_show() before on_show(), so callbacks
    // must be registered in the constructor to be available during XML parsing.
    lv_xml_register_event_cb(nullptr, "on_scanner_refresh", [](lv_event_t* /*e*/) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ScannerPickerModal] on_scanner_refresh");
        if (s_active_instance_) {
            s_active_instance_->populate_device_list();
        }
        LVGL_SAFE_EVENT_CB_END();
    });

    lv_xml_register_event_cb(nullptr, "on_scanner_bt_scan", [](lv_event_t* /*e*/) {
        LVGL_SAFE_EVENT_CB_BEGIN("[ScannerPickerModal] on_scanner_bt_scan");
        if (s_active_instance_) {
            if (s_active_instance_->bt_discovering_) {
                s_active_instance_->stop_bt_discovery();
            } else {
                s_active_instance_->start_bt_discovery();
            }
        }
        LVGL_SAFE_EVENT_CB_END();
    });
}

// ============================================================================
// DESTRUCTOR
// ============================================================================

ScannerPickerModal::~ScannerPickerModal() {
    stop_bt_discovery();

    if (bt_ctx_) {
        auto& loader = helix::bluetooth::BluetoothLoader::instance();
        if (loader.deinit) {
            loader.deinit(bt_ctx_);
        }
        bt_ctx_ = nullptr;
    }
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void ScannerPickerModal::on_show() {
    s_active_instance_ = this;

    // Store this pointer on dialog for static callback lookup
    if (dialog()) {
        lv_obj_set_user_data(dialog(), this);
    }

    // Wire close (X) button and Cancel button to Modal cancel handler
    wire_cancel_button("btn_close");
    wire_cancel_button("btn_primary");

    // Find widget pointers from XML
    device_list_ = find_widget("scanner_device_list");
    empty_state_ = find_widget("scanner_empty_state");
    bt_scan_btn_ = find_widget("btn_bt_scan");
    bt_spinner_ = find_widget("bt_scan_spinner");

    // Hide BT scan button if Bluetooth is not available
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available()) {
        if (bt_scan_btn_) {
            lv_obj_add_flag(bt_scan_btn_, LV_OBJ_FLAG_HIDDEN);
        }
        spdlog::debug("[{}] Bluetooth not available, hiding BT scan button", get_name());
    }

    // Hide spinner initially
    if (bt_spinner_) {
        lv_obj_add_flag(bt_spinner_, LV_OBJ_FLAG_HIDDEN);
    }

    populate_device_list();

    spdlog::debug("[{}] Shown, current device: '{}'", get_name(),
                  current_device_id_.empty() ? "auto-detect" : current_device_id_);
}

// ============================================================================
// DEVICE LIST POPULATION
// ============================================================================

void ScannerPickerModal::populate_device_list() {
    if (!device_list_) {
        spdlog::warn("[{}] device_list_ widget not found", get_name());
        return;
    }

    // Clean existing rows
    lv_obj_clean(device_list_);

    // Add "Auto-detect (default)" row
    add_device_row(device_list_, lv_tr("Auto-detect (default)"), lv_tr("Uses name-based priority"),
                   "");

    // Enumerate sysfs HID devices (USB + already-connected BT)
    auto devices = helix::input::enumerate_usb_hid_devices();

    spdlog::debug("[{}] Found {} HID devices from sysfs", get_name(), devices.size());

    // Track MACs already present from sysfs (to avoid duplicates from BT discovery list)
    std::vector<std::string> sysfs_macs;

    for (const auto& dev : devices) {
        std::string vendor_product = dev.vendor_id + ":" + dev.product_id;
        bool is_bt = (dev.bus_type == 0x05);
        std::string sublabel = vendor_product + "  " + dev.event_path;
        if (is_bt) {
            sublabel += "  [BT]";
            // dev doesn't have MAC, so we can't de-dup perfectly here
        }
        add_device_row(device_list_, dev.name, sublabel, vendor_product, is_bt, "");
    }

    // Append BT-discovered devices that aren't already showing as sysfs devices
    // (i.e., discovered but not yet paired/connected as HID)
    const std::string saved_bt_mac = helix::SettingsManager::instance().get_scanner_bt_address();

    for (const auto& bt_dev : bt_devices_) {
        // Skip if this MAC is already represented by a sysfs device
        // (We use name matching as a heuristic since sysfs doesn't give us MAC)
        bool already_shown = false;
        for (const auto& dev : devices) {
            if (dev.name == bt_dev.name) {
                already_shown = true;
                break;
            }
        }
        if (already_shown)
            continue;

        std::string sublabel;
        if (bt_dev.paired) {
            sublabel = fmt::format("{} — {}", bt_dev.mac, lv_tr("Paired"));
        } else if (bt_dev.mac == saved_bt_mac && !saved_bt_mac.empty()) {
            sublabel = fmt::format("{} — {}", bt_dev.mac, lv_tr("Saved"));
        } else {
            sublabel = bt_dev.mac;
        }

        // Use MAC as vendor_product placeholder for BT-only devices
        add_device_row(device_list_, bt_dev.name, sublabel, "", true, bt_dev.mac);
    }

    // Show/hide empty state (ignore auto-detect row: devices.empty() && bt_devices_.empty())
    if (empty_state_) {
        if (devices.empty() && bt_devices_.empty()) {
            lv_obj_remove_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ============================================================================
// ROW CREATION
// ============================================================================

void ScannerPickerModal::add_device_row(lv_obj_t* list, const std::string& label,
                                        const std::string& sublabel,
                                        const std::string& vendor_product, bool is_bluetooth,
                                        const std::string& bt_mac) {
    // Create a row container
    lv_obj_t* row = lv_obj_create(list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, theme_manager_get_spacing("space_md"), 0);
    lv_obj_set_style_pad_gap(row, 4, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    // Highlight selected device (for USB devices with vendor_product; BT uses MAC)
    const std::string saved_bt_mac = helix::SettingsManager::instance().get_scanner_bt_address();
    bool is_selected =
        (!vendor_product.empty() && vendor_product == current_device_id_) ||
        (is_bluetooth && !bt_mac.empty() && bt_mac == saved_bt_mac && current_device_id_.empty());

    if (is_selected) {
        auto primary = theme_manager_get_color("primary");
        lv_obj_set_style_bg_color(row, primary, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, primary, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_radius(row, 8, 0);
    } else {
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    }

    // Add bottom border as separator (except for selected items which have full border)
    if (!is_selected) {
        auto border_color = theme_manager_get_color("border");
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, border_color, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_30, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    }

    // Name row: flex row containing optional BT icon + name label
    lv_obj_t* name_row = lv_obj_create(row);
    lv_obj_set_width(name_row, lv_pct(100));
    lv_obj_set_height(name_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(name_row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(name_row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_pad_all(name_row, 0, 0);
    lv_obj_set_style_pad_gap(name_row, 4, 0);
    lv_obj_set_style_border_width(name_row, 0, 0);
    lv_obj_set_style_bg_opa(name_row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(name_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(name_row, LV_OBJ_FLAG_EVENT_BUBBLE);

    if (is_bluetooth) {
        // BT icon badge
        lv_obj_t* bt_icon = lv_label_create(name_row);
        const char* bt_codepoint = ui_icon::lookup_codepoint("bluetooth");
        lv_label_set_text(bt_icon, bt_codepoint ? bt_codepoint : "");
        lv_obj_set_style_text_font(bt_icon, theme_manager_get_font("icon_font_sm"), 0);
        lv_obj_set_style_text_color(bt_icon, theme_manager_get_color("accent"), 0);
        lv_obj_remove_flag(bt_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(bt_icon, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // Name label (body font) — clickable=false so clicks bubble to row
    lv_obj_t* name_label = lv_label_create(name_row);
    lv_label_set_text(name_label, label.c_str());
    lv_obj_set_style_text_font(name_label, theme_manager_get_font("font_body"), 0);
    lv_obj_set_style_flex_grow(name_label, 1, 0);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_WRAP);
    lv_obj_remove_flag(name_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(name_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Sublabel (small font, muted color) — clickable=false so clicks bubble to row
    lv_obj_t* sub_label = lv_label_create(row);
    lv_label_set_text(sub_label, sublabel.c_str());
    lv_obj_set_style_text_font(sub_label, theme_manager_get_font("font_small"), 0);
    lv_obj_set_style_text_color(sub_label, theme_manager_get_color("text_muted"), 0);
    lv_obj_set_width(sub_label, lv_pct(100));
    lv_label_set_long_mode(sub_label, LV_LABEL_LONG_WRAP);
    lv_obj_remove_flag(sub_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sub_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Attach RowData for click handling
    auto* data = new RowData{vendor_product, label, bt_mac, this};
    lv_obj_set_user_data(row, data);

    // Click handler — dynamic list content, lv_obj_add_event_cb is acceptable
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            auto* row_obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
            auto* d = static_cast<RowData*>(lv_obj_get_user_data(row_obj));
            if (d && d->modal) {
                d->modal->handle_device_selected(d->vendor_product, d->device_name, d->bt_mac);
            }
        },
        LV_EVENT_CLICKED, nullptr);

    // Clean up RowData on delete to prevent leaks
    lv_obj_add_event_cb(
        row,
        [](lv_event_t* e) {
            auto* row_obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
            delete static_cast<RowData*>(lv_obj_get_user_data(row_obj));
            lv_obj_set_user_data(row_obj, nullptr);
        },
        LV_EVENT_DELETE, nullptr);
}

// ============================================================================
// SELECTION HANDLING
// ============================================================================

void ScannerPickerModal::handle_device_selected(const std::string& vendor_product,
                                                const std::string& device_name,
                                                const std::string& bt_mac) {
    // If this is an unpaired BT-only device, prompt for pairing first
    if (!bt_mac.empty() && vendor_product.empty()) {
        // Check if already paired in our discovered list
        bool already_paired = false;
        for (const auto& dev : bt_devices_) {
            if (dev.mac == bt_mac && dev.paired) {
                already_paired = true;
                break;
            }
        }

        if (!already_paired) {
            pair_bt_device(bt_mac, device_name);
            return;
        }
    }

    spdlog::info("[{}] Selected device: '{}' ({}{})", get_name(), device_name,
                 vendor_product.empty() ? "auto-detect" : vendor_product,
                 bt_mac.empty() ? "" : " bt:" + bt_mac);

    // Persist selection
    helix::SettingsManager::instance().set_scanner_device_id(vendor_product);
    helix::SettingsManager::instance().set_scanner_device_name(
        vendor_product.empty() && bt_mac.empty() ? "" : device_name);

    // Persist BT address (clear it for USB selections)
    if (!bt_mac.empty()) {
        helix::SettingsManager::instance().set_scanner_bt_address(bt_mac);
    } else {
        helix::SettingsManager::instance().set_scanner_bt_address("");
    }

    // Fire callback
    if (on_select_) {
        on_select_(vendor_product, device_name);
    }

    // Clear active instance before hide (hide destroys us)
    s_active_instance_ = nullptr;

    hide();
}

// ============================================================================
// BLUETOOTH DISCOVERY
// ============================================================================

void ScannerPickerModal::start_bt_discovery() {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available() || !loader.discover) {
        spdlog::warn("[{}] BT discovery unavailable", get_name());
        return;
    }

    if (bt_discovering_) {
        spdlog::debug("[{}] BT discovery already in progress", get_name());
        return;
    }

    // Initialize BT context if needed
    if (!bt_ctx_ && loader.init) {
        bt_ctx_ = loader.init();
        if (!bt_ctx_) {
            spdlog::error("[{}] Failed to init BT context", get_name());
            ToastManager::instance().show(ToastSeverity::ERROR,
                                          lv_tr("Bluetooth initialization failed"));
            return;
        }
    }

    bt_discovering_ = true;

    // Keep already-paired devices; remove unpaired (stale discovery results)
    bt_devices_.erase(std::remove_if(bt_devices_.begin(), bt_devices_.end(),
                                     [](const BtDeviceInfo& d) { return !d.paired; }),
                      bt_devices_.end());

    // Show spinner
    if (bt_spinner_) {
        lv_obj_remove_flag(bt_spinner_, LV_OBJ_FLAG_HIDDEN);
    }

    // Set up C callback safety context
    bt_discovery_ctx_ = std::make_unique<BtDiscoveryContext>();
    bt_discovery_ctx_->alive.store(true);
    bt_discovery_ctx_->modal = this;

    auto* disc_ctx = bt_discovery_ctx_.get();
    auto* ctx = bt_ctx_;
    auto token = lifetime_.token();

    // Run discovery on a detached thread
    std::thread([ctx, disc_ctx, token, &loader]() {
        loader.discover(
            ctx, 15000,
            [](const helix_bt_device* dev, void* user_data) {
                auto* dctx = static_cast<BtDiscoveryContext*>(user_data);
                if (!dctx->alive.load())
                    return;

                // Filter: only include devices that look like HID scanners
                bool looks_like_scanner =
                    helix::bluetooth::is_hid_scanner_uuid(dev->service_uuid) ||
                    helix::bluetooth::is_likely_bt_scanner(dev->name);

                if (!looks_like_scanner) {
                    spdlog::trace("[ScannerPickerModal] Skipping non-scanner BT device: {}",
                                  dev->name ? dev->name : "(null)");
                    return;
                }

                // Copy device info to avoid dangling pointers
                BtDeviceInfo info;
                info.mac = dev->mac ? dev->mac : "";
                info.name = dev->name ? dev->name : lv_tr("Unknown");
                info.paired = dev->paired;
                info.is_ble = dev->is_ble;

                // Marshal to UI thread
                helix::ui::queue_update([dctx, info]() {
                    if (!dctx->alive.load())
                        return;
                    auto* modal = dctx->modal;

                    // Add to device list (avoid duplicates by MAC)
                    bool found = false;
                    for (const auto& existing : modal->bt_devices_) {
                        if (existing.mac == info.mac) {
                            found = true;
                            break;
                        }
                    }

                    if (!found) {
                        modal->bt_devices_.push_back(info);
                        spdlog::debug("[ScannerPickerModal] BT discovered: {} ({})", info.name,
                                      info.mac);
                        modal->populate_device_list();
                    }
                });
            },
            disc_ctx);

        // Discovery completed (timeout or stopped)
        helix::ui::queue_update([disc_ctx, token]() {
            if (token.expired())
                return;
            if (!disc_ctx->alive.load())
                return;

            auto* modal = disc_ctx->modal;
            modal->bt_discovering_ = false;

            // Hide spinner
            if (modal->bt_spinner_) {
                lv_obj_add_flag(modal->bt_spinner_, LV_OBJ_FLAG_HIDDEN);
            }

            spdlog::info("[ScannerPickerModal] BT discovery finished, {} scanner devices found",
                         modal->bt_devices_.size());

            // Final refresh to ensure list is up to date
            modal->populate_device_list();
        });
    }).detach();

    spdlog::info("[{}] Started Bluetooth scanner discovery", get_name());
}

void ScannerPickerModal::stop_bt_discovery() {
    if (!bt_discovering_)
        return;

    // Invalidate the discovery context to prevent callbacks
    if (bt_discovery_ctx_) {
        bt_discovery_ctx_->alive.store(false);
    }

    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (bt_ctx_ && loader.stop_discovery) {
        loader.stop_discovery(bt_ctx_);
    }

    bt_discovering_ = false;

    if (bt_spinner_) {
        lv_obj_add_flag(bt_spinner_, LV_OBJ_FLAG_HIDDEN);
    }

    spdlog::debug("[{}] Stopped Bluetooth discovery", get_name());
}

// ============================================================================
// BLUETOOTH PAIRING
// ============================================================================

void ScannerPickerModal::pair_bt_device(const std::string& mac, const std::string& name) {
    auto* pair_data = new PairData{mac, name};

    auto msg = fmt::format("{} {}?", lv_tr("Pair with"), name);
    auto* dialog = helix::ui::modal_show_confirmation(lv_tr("Pair Bluetooth Scanner"), msg.c_str(),
                                                      ModalSeverity::Info, lv_tr("Pair"),
                                                      on_pair_confirm, on_pair_cancel, pair_data);

    if (!dialog) {
        delete pair_data;
        spdlog::warn("[{}] Failed to show pairing confirmation modal", get_name());
    }
}

void ScannerPickerModal::on_pair_confirm(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ScannerPickerModal] on_pair_confirm");

    auto* pair_data = static_cast<PairData*>(lv_event_get_user_data(e));
    if (!pair_data) {
        spdlog::warn("[ScannerPickerModal] on_pair_confirm: null pair_data");
    } else {
        std::string mac = pair_data->mac;
        std::string name = pair_data->name;
        delete pair_data;

        // Close the confirmation modal
        auto* top = Modal::get_top();
        if (top)
            Modal::hide(top);

        auto& loader = helix::bluetooth::BluetoothLoader::instance();
        if (!loader.is_available() || !loader.pair) {
            ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Bluetooth not available"));
        } else if (!s_active_instance_ || !s_active_instance_->bt_ctx_) {
            ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Bluetooth not initialized"));
        } else {
            ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Pairing..."), 5000);

            auto* bt_ctx = s_active_instance_->bt_ctx_;
            auto token = s_active_instance_->lifetime_.token();

            std::thread([mac, name, bt_ctx, token]() {
                auto& ldr = helix::bluetooth::BluetoothLoader::instance();
                int ret = ldr.pair(bt_ctx, mac.c_str());

                int paired_r = -1;
                if (ret == 0) {
                    paired_r = ldr.is_paired ? ldr.is_paired(bt_ctx, mac.c_str()) : -1;
                    spdlog::info("[ScannerPickerModal] Post-pair: is_paired={}", paired_r);
                }

                helix::ui::queue_update([ret, mac, name, token, bt_ctx, paired_r]() {
                    if (token.expired())
                        return;

                    if (ret == 0) {
                        ToastManager::instance().show(ToastSeverity::SUCCESS,
                                                      lv_tr("Paired successfully"), 2000);

                        if (s_active_instance_) {
                            // Update device paired state
                            for (auto& dev : s_active_instance_->bt_devices_) {
                                if (dev.mac == mac) {
                                    dev.paired = (paired_r == 1);
                                    break;
                                }
                            }

                            // Persist the pairing
                            helix::SettingsManager::instance().set_scanner_bt_address(mac);
                            helix::SettingsManager::instance().set_scanner_device_name(name);

                            // Fire selection callback and close modal
                            if (s_active_instance_->on_select_) {
                                s_active_instance_->on_select_("", name);
                            }
                            s_active_instance_ = nullptr;
                            // Note: we can't call hide() here since we may be in a queued callback
                            // The confirmation modal was already closed above; the picker modal
                            // will close on next user interaction or we close it now via the stack
                            auto* picker = Modal::get_top();
                            if (picker)
                                Modal::hide(picker);
                        }
                    } else {
                        auto& ldr2 = helix::bluetooth::BluetoothLoader::instance();
                        const char* err =
                            ldr2.last_error ? ldr2.last_error(bt_ctx) : "Unknown error";
                        spdlog::error("[ScannerPickerModal] Pairing failed: {}", err);
                        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Pairing failed"),
                                                      3000);
                    }
                });
            }).detach();
        } // else (BT available and context valid)
    } // else (pair_data valid)

    LVGL_SAFE_EVENT_CB_END();
}

void ScannerPickerModal::on_pair_cancel(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[ScannerPickerModal] on_pair_cancel");

    auto* pair_data = static_cast<PairData*>(lv_event_get_user_data(e));
    delete pair_data;

    // Close the confirmation modal
    auto* top = Modal::get_top();
    if (top)
        Modal::hide(top);

    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::ui
