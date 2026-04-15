// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_barcode_scanner.h"

#include "bluetooth_loader.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "usb_scanner_monitor.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>

namespace helix::ui {

BarcodeScannerSettingsOverlay* BarcodeScannerSettingsOverlay::s_active_instance_ = nullptr;

namespace {
std::unique_ptr<BarcodeScannerSettingsOverlay> g_barcode_scanner_overlay;
}

BarcodeScannerSettingsOverlay& get_barcode_scanner_settings_overlay() {
    if (!g_barcode_scanner_overlay) {
        g_barcode_scanner_overlay = std::make_unique<BarcodeScannerSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "BarcodeScannerSettingsOverlay", []() { g_barcode_scanner_overlay.reset(); });
    }
    return *g_barcode_scanner_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

BarcodeScannerSettingsOverlay::BarcodeScannerSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

BarcodeScannerSettingsOverlay::~BarcodeScannerSettingsOverlay() {
    stop_bt_discovery();

    if (subjects_initialized_) {
        lv_subject_deinit(&bt_available_subject_);
        lv_subject_deinit(&bt_discovering_subject_);
        lv_subject_deinit(&keymap_index_subject_);
        lv_subject_deinit(&has_devices_subject_);
        lv_subject_deinit(&current_device_label_subject_);
    }

    if (bt_ctx_) {
        auto& loader = helix::bluetooth::BluetoothLoader::instance();
        if (loader.deinit) {
            loader.deinit(bt_ctx_);
        }
        bt_ctx_ = nullptr;
    }

    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void BarcodeScannerSettingsOverlay::init_subjects() {
    if (subjects_initialized_) return;

    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    lv_subject_init_int(&bt_available_subject_, loader.is_available() ? 1 : 0);
    lv_xml_register_subject(nullptr, "scanner_bt_available", &bt_available_subject_);

    lv_subject_init_int(&bt_discovering_subject_, 0);
    lv_xml_register_subject(nullptr, "scanner_bt_discovering", &bt_discovering_subject_);

    lv_subject_init_int(&has_devices_subject_, 0);
    lv_xml_register_subject(nullptr, "scanner_has_devices", &has_devices_subject_);

    const std::string km = helix::SettingsManager::instance().get_scanner_keymap();
    int km_idx = 0;
    if (km == "qwertz") km_idx = 1;
    else if (km == "azerty") km_idx = 2;
    lv_subject_init_int(&keymap_index_subject_, km_idx);
    lv_xml_register_subject(nullptr, "scanner_keymap_index", &keymap_index_subject_);

    current_device_label_buf_[0] = '\0';
    lv_subject_init_string(&current_device_label_subject_, current_device_label_buf_, nullptr,
                           sizeof(current_device_label_buf_), current_device_label_buf_);
    lv_xml_register_subject(nullptr, "scanner_current_device_label",
                            &current_device_label_subject_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void BarcodeScannerSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_bs_scan_bluetooth", on_bs_scan_bluetooth},
        {"on_bs_refresh_usb", on_bs_refresh_usb},
        {"on_bs_keymap_changed", on_bs_keymap_changed},
        {"on_bs_row_clicked", on_bs_row_clicked},
        {"on_bs_row_forget", on_bs_row_forget},
        {"on_bs_auto_detect_clicked", on_bs_auto_detect_clicked},
        {"on_bs_pair_confirm", on_bs_pair_confirm},
        {"on_bs_pair_cancel", on_bs_pair_cancel},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* BarcodeScannerSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ = static_cast<lv_obj_t*>(
        lv_xml_create(parent, "barcode_scanner_settings", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void BarcodeScannerSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void BarcodeScannerSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    s_active_instance_ = this;

    usb_list_ = lv_obj_find_by_name(overlay_root_, "usb_device_list");
    bt_list_  = lv_obj_find_by_name(overlay_root_, "bt_device_list");

    refresh_current_selection_label();
    populate_device_list();
}

void BarcodeScannerSettingsOverlay::on_deactivate() {
    stop_bt_discovery();
    s_active_instance_ = nullptr;
    usb_list_ = nullptr;
    bt_list_ = nullptr;
    OverlayBase::on_deactivate();
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

void BarcodeScannerSettingsOverlay::refresh_current_selection_label() {
    auto id = helix::SettingsManager::instance().get_scanner_device_id();
    auto name = helix::SettingsManager::instance().get_scanner_device_name();
    const char* text = id.empty() ? "Auto-detect" : name.c_str();
    lv_subject_copy_string(&current_device_label_subject_, text);
}

// ============================================================================
// STUBS — Tasks 6 and 7 fill these in
// ============================================================================

void BarcodeScannerSettingsOverlay::populate_device_list() {}

void BarcodeScannerSettingsOverlay::add_usb_row(lv_obj_t*, const std::string&,
                                                 const std::string&, const std::string&) {}

void BarcodeScannerSettingsOverlay::add_bt_row(lv_obj_t*, const std::string&, const std::string&,
                                                const std::string&, const std::string&, bool) {}

void BarcodeScannerSettingsOverlay::add_auto_detect_row(lv_obj_t*) {}

void BarcodeScannerSettingsOverlay::handle_device_selected(const std::string&,
                                                            const std::string&,
                                                            const std::string&) {}

void BarcodeScannerSettingsOverlay::handle_keymap_changed(int dropdown_index) {
    const char* values[] = {"qwerty", "qwertz", "azerty"};
    if (dropdown_index < 0 || dropdown_index >= 3) return;
    helix::SettingsManager::instance().set_scanner_keymap(values[dropdown_index]);
    helix::UsbScannerMonitor::set_active_layout(
        helix::UsbScannerMonitor::parse_keymap(values[dropdown_index]));
}

void BarcodeScannerSettingsOverlay::start_bt_discovery() {}

void BarcodeScannerSettingsOverlay::stop_bt_discovery() {}

void BarcodeScannerSettingsOverlay::pair_bt_device(const std::string&, const std::string&) {}

void BarcodeScannerSettingsOverlay::handle_bt_forget(const std::string&) {}

// ============================================================================
// Static XML event callbacks
// ============================================================================

void BarcodeScannerSettingsOverlay::on_bs_scan_bluetooth(lv_event_t*) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BarcodeScannerSettings] on_bs_scan_bluetooth");
    if (!s_active_instance_) return;
    if (s_active_instance_->bt_discovering_)
        s_active_instance_->stop_bt_discovery();
    else
        s_active_instance_->start_bt_discovery();
    LVGL_SAFE_EVENT_CB_END();
}

void BarcodeScannerSettingsOverlay::on_bs_refresh_usb(lv_event_t*) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BarcodeScannerSettings] on_bs_refresh_usb");
    if (s_active_instance_) s_active_instance_->populate_device_list();
    LVGL_SAFE_EVENT_CB_END();
}

void BarcodeScannerSettingsOverlay::on_bs_keymap_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BarcodeScannerSettings] on_bs_keymap_changed");
    if (!s_active_instance_) return;
    auto* dd = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    s_active_instance_->handle_keymap_changed(static_cast<int>(lv_dropdown_get_selected(dd)));
    LVGL_SAFE_EVENT_CB_END();
}

void BarcodeScannerSettingsOverlay::on_bs_row_clicked(lv_event_t*) {}

void BarcodeScannerSettingsOverlay::on_bs_row_forget(lv_event_t*) {}

void BarcodeScannerSettingsOverlay::on_bs_auto_detect_clicked(lv_event_t*) {}

void BarcodeScannerSettingsOverlay::on_bs_pair_confirm(lv_event_t*) {}

void BarcodeScannerSettingsOverlay::on_bs_pair_cancel(lv_event_t*) {}

} // namespace helix::ui
