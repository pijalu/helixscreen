// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_label_printer.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "bluetooth_loader.h"
#include "brother_ql_printer.h"
#include "ipp_printer.h"
#include "sheet_label_layout.h"
#include "bt_discovery_utils.h"
#include "label_printer_settings.h"
#include "label_printer_utils.h"
#include "makeid_protocol.h"
#include "niimbot_protocol.h"
#include "phomemo_printer.h"
#include "runtime_config.h"
#include "spoolman_types.h"
#include "static_panel_registry.h"
#include "usb_printer_detector.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

namespace helix::settings {

// ============================================================================
// HELPERS
// ============================================================================

/// Map cryptic BLE chip names to user-friendly product names.
/// Some printers advertise their SoC name rather than the product brand.
static std::string friendly_bt_name(const std::string& name) {
    // MakeID E1 uses Yichip BLE SoC — advertises as "YichipFPGA-XXXX"
    if (strncasecmp(name.c_str(), "YichipFPGA", 10) == 0) {
        return "MakeID (" + name + ")"; // i18n: do not translate — product name
    }
    return name;
}

/// Build dropdown label for a BT device showing connection status.
/// @param paired  BlueZ confirms device is paired
/// @param saved   Device address is saved in our settings (may not be BlueZ-paired)
/// @param connected  Active BLE/RFCOMM connection
static std::string bt_device_label(const std::string& name, bool paired, bool saved,
                                    bool connected) {
    std::string label = friendly_bt_name(name);
    if (connected) {
        label += " (Connected)";
    } else if (paired) {
        label += " (Paired)";
    } else if (saved) {
        label += " (Saved)";
    }
    return label;
}

/// Check BLE connection state for a device via BlueZ D-Bus.
static bool check_bt_connected(helix_bt_context* ctx, const std::string& mac) {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!ctx || !loader.is_connected)
        return false;
    return loader.is_connected(ctx, mac.c_str()) == 1;
}

/// Return the correct label sizes for the current printer configuration.
/// Mirrors the logic in label_printer_utils.cpp print_spool_label().
static std::vector<helix::LabelSize> get_sizes_for_current_printer() {
    auto& settings = LabelPrinterSettingsManager::instance();
    const auto type = settings.get_printer_type();

    if (type == "network" && settings.get_printer_protocol() == "ipp") {
        return helix::IppPrinter::supported_sizes_static();
    }
    if (type == "usb") {
        return helix::PhomemoPrinter::supported_sizes_static();
    }
    if (type == "bluetooth") {
        const auto bt_name = settings.get_bt_name();
        if (helix::bluetooth::is_brother_printer(bt_name.c_str())) {
            return BrotherQLPrinter::supported_sizes_static();
        }
        if (helix::bluetooth::is_niimbot_printer(bt_name.c_str())) {
            return helix::label::niimbot_sizes_for_model(bt_name);
        }
        if (helix::bluetooth::is_makeid_printer(bt_name.c_str())) {
            return helix::label::makeid_default_sizes();
        }
        return helix::PhomemoPrinter::supported_sizes_static();
    }
    return BrotherQLPrinter::supported_sizes_static();
}

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<LabelPrinterSettingsOverlay> g_label_printer_overlay;

LabelPrinterSettingsOverlay& get_label_printer_settings_overlay() {
    if (!g_label_printer_overlay) {
        g_label_printer_overlay = std::make_unique<LabelPrinterSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy("LabelPrinterSettingsOverlay",
                                                         []() { g_label_printer_overlay.reset(); });
    }
    return *g_label_printer_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

LabelPrinterSettingsOverlay::LabelPrinterSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

LabelPrinterSettingsOverlay::~LabelPrinterSettingsOverlay() {
    alive_->store(false);
    stop_label_printer_discovery();
    stop_usb_detection();
    stop_bt_discovery();

    // Deinit subjects
    if (subjects_initialized_) {
        lv_subject_deinit(&bt_scanning_subject_);
        lv_subject_deinit(&ipp_selected_subject_);
    }

    // Deinit BT context only in destructor, not in stop_bt_discovery()
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

void LabelPrinterSettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Ensure manager subjects are initialized (reads config for initial value)
    LabelPrinterSettingsManager::instance().init_subjects();

    // Register C++-owned subject globally so XML bind_flag_if_not_eq can find it
    lv_xml_register_subject(nullptr, "printer_type_subject",
                            LabelPrinterSettingsManager::instance().subject_printer_type());

    // BT scanning state subject (0=idle, 1=scanning)
    lv_subject_init_int(&bt_scanning_subject_, 0);
    lv_xml_register_subject(nullptr, "bt_scanning", &bt_scanning_subject_);

    // IPP selected subject (0=not IPP, 1=IPP protocol selected within network type)
    int ipp_initial = (LabelPrinterSettingsManager::instance().get_printer_type() == "network" &&
                       LabelPrinterSettingsManager::instance().get_printer_protocol() == "ipp") ? 1 : 0;
    lv_subject_init_int(&ipp_selected_subject_, ipp_initial);
    lv_xml_register_subject(nullptr, "ipp_selected", &ipp_selected_subject_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void LabelPrinterSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_lp_label_size_changed", on_label_size_changed},
        {"on_lp_preset_changed", on_preset_changed},
        {"on_lp_test_print", on_test_print},
        {"on_lp_printer_selected", on_printer_selected},
        {"on_lp_type_changed", on_type_changed},
        {"on_lp_usb_printer_selected", on_usb_printer_selected},
        {"on_lp_bt_printer_selected", on_bt_printer_selected},
        {"on_lp_bt_scan", on_bt_scan},
        {"on_lp_bt_connect", on_bt_connect},
        {"on_lp_label_count_changed", on_label_count_changed},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* LabelPrinterSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "label_printer_settings", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void LabelPrinterSettingsOverlay::show(lv_obj_t* parent_screen) {
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

void LabelPrinterSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_printer_type_dropdown();
    init_address_input();
    init_port_input();
    init_label_size_dropdown();
    init_preset_dropdown();
    init_discovery_dropdown();
    init_usb_printer_dropdown();
    init_bt_printer_dropdown();
    init_label_count_dropdown();

    // Update IPP selected subject based on current config
    update_ipp_selected_subject();

    // Start detection based on current printer type
    auto& settings = LabelPrinterSettingsManager::instance();
    const auto type = settings.get_printer_type();
    if (type == "usb") {
        start_usb_detection();
    } else if (type == "bluetooth") {
        // BT discovery is on-demand (scan button), not automatic
    } else {
        start_label_printer_discovery();
    }

    inputs_initialized_ = true;
}

void LabelPrinterSettingsOverlay::on_deactivate() {
    stop_label_printer_discovery();
    stop_usb_detection();
    stop_bt_discovery();
    OverlayBase::on_deactivate();
}

// ============================================================================
// INTERNAL METHODS
// ============================================================================

void LabelPrinterSettingsOverlay::init_address_input() {
    if (!overlay_root_)
        return;

    auto& settings = LabelPrinterSettingsManager::instance();
    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_address_port");
    if (!row)
        return;

    lv_obj_t* input = lv_obj_find_by_name(row, "input_address");
    if (input) {
        lv_textarea_set_text(input, settings.get_printer_address().c_str());
        if (!inputs_initialized_) {
            lv_obj_add_event_cb(input, on_address_done, LV_EVENT_DEFOCUSED, nullptr);
        }
        spdlog::trace("[{}] Address input initialized", get_name());
    }
}

void LabelPrinterSettingsOverlay::init_port_input() {
    if (!overlay_root_)
        return;

    auto& settings = LabelPrinterSettingsManager::instance();
    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_address_port");
    if (!row)
        return;

    lv_obj_t* input = lv_obj_find_by_name(row, "input_port");
    if (input) {
        auto port_str = fmt::format("{}", settings.get_printer_port());
        lv_textarea_set_text(input, port_str.c_str());
        if (!inputs_initialized_) {
            lv_obj_add_event_cb(input, on_port_done, LV_EVENT_DEFOCUSED, nullptr);
        }
        spdlog::trace("[{}] Port input initialized", get_name());
    }
}

void LabelPrinterSettingsOverlay::init_label_size_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* size_row = lv_obj_find_by_name(overlay_root_, "row_label_size");
    if (!size_row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(size_row, "dropdown");
    if (dropdown) {
        auto& settings = LabelPrinterSettingsManager::instance();
        auto sizes = get_sizes_for_current_printer();

        std::string options;
        for (size_t i = 0; i < sizes.size(); i++) {
            if (i > 0)
                options += "\n";
            options += sizes[i].name;
        }

        if (!options.empty()) {
            lv_dropdown_set_options(dropdown, options.c_str());
            int current_idx = settings.get_label_size_index();
            int max_idx = static_cast<int>(sizes.size()) - 1;
            if (current_idx > max_idx) {
                current_idx = 0;
                settings.set_label_size_index(0);
            }
            lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(current_idx));
        }
        spdlog::trace("[{}] Label size dropdown initialized ({} sizes)", get_name(), sizes.size());
    }
}

void LabelPrinterSettingsOverlay::init_preset_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* preset_row = lv_obj_find_by_name(overlay_root_, "row_preset");
    if (!preset_row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(preset_row, "dropdown");
    if (dropdown) {
        auto& settings = LabelPrinterSettingsManager::instance();
        // Build translated preset options
        auto options =
            fmt::format("{}\n{}\n{}", lv_tr("Standard"), lv_tr("Compact"), lv_tr("QR Only"));
        lv_dropdown_set_options(dropdown, options.c_str());
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(settings.get_label_preset()));

        // For small square labels, only QR-only is available
        auto sizes = get_sizes_for_current_printer();
        int size_idx = std::clamp(settings.get_label_size_index(), 0,
                                  static_cast<int>(sizes.size()) - 1);
        const auto& sz = sizes[size_idx];
        if (sz.width_px <= 250 && sz.height_px > 0 && sz.height_px <= 250) {
            lv_dropdown_set_options(dropdown, lv_tr("QR Only"));
            lv_dropdown_set_selected(dropdown, 0);
            lv_obj_add_state(dropdown, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(dropdown, LV_STATE_DISABLED);
        }

        spdlog::trace("[{}] Preset dropdown initialized", get_name());
    }
}

// ============================================================================
// MDNS DISCOVERY
// ============================================================================

void LabelPrinterSettingsOverlay::init_discovery_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_discovered_printers");
    if (!row)
        return;

    // Hide the empty description text to vertically center the label
    lv_obj_t* desc = lv_obj_find_by_name(row, "description");
    if (desc) {
        lv_obj_add_flag(desc, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        lv_dropdown_set_options(dropdown, lv_tr("Searching..."));
    }
}

void LabelPrinterSettingsOverlay::start_label_printer_discovery() {
    // Start raw TCP discovery (_pdl-datastream._tcp)
    if (!mdns_discovery_ || !mdns_discovery_->is_discovering()) {
        mdns_discovery_ = std::make_unique<MdnsDiscovery>("_pdl-datastream._tcp.local");
        auto alive = alive_;
        mdns_discovery_->start_discovery(
            [this, alive](const std::vector<DiscoveredPrinter>& printers) {
                if (!alive->load()) return;
                raw_printers_ = printers;
                merge_and_update_discovery();
            });
    }

    // Start IPP discovery (_ipp._tcp)
    if (!ipp_mdns_discovery_ || !ipp_mdns_discovery_->is_discovering()) {
        ipp_mdns_discovery_ = std::make_unique<MdnsDiscovery>("_ipp._tcp.local");
        auto alive = alive_;
        ipp_mdns_discovery_->start_discovery(
            [this, alive](const std::vector<DiscoveredPrinter>& printers) {
                if (!alive->load()) return;
                ipp_printers_ = printers;
                merge_and_update_discovery();
            });
    }

    spdlog::debug("[{}] Started network printer mDNS discovery (raw TCP + IPP)", get_name());
}

void LabelPrinterSettingsOverlay::stop_label_printer_discovery() {
    if (mdns_discovery_) {
        mdns_discovery_->stop_discovery();
        mdns_discovery_.reset();
    }
    if (ipp_mdns_discovery_) {
        ipp_mdns_discovery_->stop_discovery();
        ipp_mdns_discovery_.reset();
    }
    spdlog::debug("[{}] Stopped network printer mDNS discovery", get_name());
}

void LabelPrinterSettingsOverlay::merge_and_update_discovery() {
    // Score and merge results from both raw TCP and IPP discoveries
    discovered_network_printers_.clear();

    for (const auto& p : raw_printers_) {
        int score = helix::label_printer_score(p);
        if (score > 0) {
            discovered_network_printers_.push_back({p, "raw", score});
        } else {
            spdlog::debug("[{}] Filtered out non-label printer: {} ({})", get_name(), p.name,
                          p.ip_address);
        }
    }

    for (const auto& p : ipp_printers_) {
        int score = helix::ipp_printer_score(p);
        if (score > 0) {
            discovered_network_printers_.push_back({p, "ipp", score});
        } else {
            spdlog::debug("[{}] Filtered out non-page printer: {} ({})", get_name(), p.name,
                          p.ip_address);
        }
    }

    // Sort by score descending (most likely printers first)
    std::sort(discovered_network_printers_.begin(), discovered_network_printers_.end(),
              [](const DiscoveredNetworkPrinter& a, const DiscoveredNetworkPrinter& b) {
                  return a.score > b.score;
              });

    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_discovered_printers");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (!dropdown)
        return;

    // Dedup by IP+protocol — keep highest-scored per unique combination
    {
        std::vector<DiscoveredNetworkPrinter> deduped;
        for (const auto& np : discovered_network_printers_) {
            bool duplicate = false;
            for (const auto& existing : deduped) {
                if (existing.printer.ip_address == np.printer.ip_address &&
                    existing.protocol == np.protocol) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                deduped.push_back(np);
            }
        }
        discovered_network_printers_ = std::move(deduped);
    }

    std::string options;
    if (discovered_network_printers_.empty()) {
        options = lv_tr("No printers found");
    } else {
        for (const auto& np : discovered_network_printers_) {
            if (!options.empty()) {
                options += "\n";
            }
            options += np.printer.name + " (" + np.printer.ip_address + ")";
            if (np.protocol == "ipp") {
                options += " [IPP]"; // i18n: do not translate — protocol name
            }
        }
    }

    lv_dropdown_set_options(dropdown, options.c_str());

    spdlog::debug("[{}] Discovery update: {} raw + {} ipp = {} merged printers", get_name(),
                  raw_printers_.size(), ipp_printers_.size(), discovered_network_printers_.size());

    // Auto-select the first discovered printer if no address is configured yet
    if (!discovered_network_printers_.empty()) {
        auto& settings = LabelPrinterSettingsManager::instance();
        if (!settings.is_configured()) {
            handle_printer_selected(0);
            spdlog::info("[{}] Auto-selected first discovered printer: {}", get_name(),
                         discovered_network_printers_[0].printer.name);
        }
    }
}

void LabelPrinterSettingsOverlay::handle_printer_selected(int index) {
    if (index < 0 || index >= static_cast<int>(discovered_network_printers_.size())) {
        return;
    }

    const auto& np = discovered_network_printers_[index];
    const auto& printer = np.printer;
    int port = (np.protocol == "ipp") ? (printer.port > 0 ? printer.port : 631) : printer.port;

    spdlog::info("[{}] Selected printer: {} ({}:{}, protocol={})", get_name(), printer.name,
                 printer.ip_address, port, np.protocol);

    auto& settings = LabelPrinterSettingsManager::instance();
    settings.set_printer_address(printer.ip_address);
    settings.set_printer_port(port);
    settings.set_printer_protocol(np.protocol);

    // Update IPP selected subject and refresh dependent UI
    update_ipp_selected_subject();
    init_label_size_dropdown();
    init_label_count_dropdown();

    // Update address and port input fields
    if (overlay_root_) {
        lv_obj_t* addr_row = lv_obj_find_by_name(overlay_root_, "row_address_port");
        if (addr_row) {
            lv_obj_t* addr_input = lv_obj_find_by_name(addr_row, "input_address");
            if (addr_input) {
                lv_textarea_set_text(addr_input, printer.ip_address.c_str());
            }

            lv_obj_t* port_input = lv_obj_find_by_name(addr_row, "input_port");
            if (port_input) {
                auto port_str = fmt::format("{}", port);
                lv_textarea_set_text(port_input, port_str.c_str());
            }
        }
    }
}

void LabelPrinterSettingsOverlay::update_ipp_selected_subject() {
    if (!subjects_initialized_) return;
    auto& settings = LabelPrinterSettingsManager::instance();
    int ipp_val = (settings.get_printer_type() == "network" &&
                   settings.get_printer_protocol() == "ipp") ? 1 : 0;
    lv_subject_set_int(&ipp_selected_subject_, ipp_val);
}

// ============================================================================
// PRINTER TYPE
// ============================================================================

void LabelPrinterSettingsOverlay::init_printer_type_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_printer_type");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        const bool bt_available = helix::bluetooth::BluetoothLoader::instance().is_available();
        std::string options;
        if (bt_available) {
            options = fmt::format("{}\n{}\n{}", lv_tr("Network"), lv_tr("USB"),
                                 lv_tr("Bluetooth"));
        } else {
            options = fmt::format("{}\n{}", lv_tr("Network"), lv_tr("USB"));
        }
        lv_dropdown_set_options(dropdown, options.c_str());

        auto& settings = LabelPrinterSettingsManager::instance();
        const auto type = settings.get_printer_type();
        int type_idx = 0;
        if (type == "usb")
            type_idx = 1;
        else if (type == "bluetooth" && bt_available)
            type_idx = 2;
        else if (type == "bluetooth" && !bt_available) {
            spdlog::warn("[{}] Saved type is bluetooth but BT unavailable, falling back to network",
                         get_name());
            settings.set_printer_type("network");
        }
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(type_idx));
    }
}

void LabelPrinterSettingsOverlay::handle_type_changed(int index) {
    const bool bt_available = helix::bluetooth::BluetoothLoader::instance().is_available();
    std::string type;
    if (bt_available) {
        // 0=network, 1=usb, 2=bluetooth
        if (index == 0)
            type = "network";
        else if (index == 1)
            type = "usb";
        else if (index == 2)
            type = "bluetooth";
        else
            type = "network";
    } else {
        // 0=network, 1=usb (no bluetooth)
        if (index == 0)
            type = "network";
        else if (index == 1)
            type = "usb";
        else
            type = "network";
    }

    spdlog::info("[{}] Printer type changed: {} (index={})", get_name(), type, index);

    auto& settings = LabelPrinterSettingsManager::instance();
    settings.set_printer_type(type);

    // Reset protocol to raw when switching away from network
    if (type != "network") {
        settings.set_printer_protocol("raw");
    }

    // Update IPP selected subject
    update_ipp_selected_subject();

    // Refresh label size dropdown for new backend
    init_label_size_dropdown();

    // Reset label size to first option for new backend
    settings.set_label_size_index(0);

    // Stop all discoveries, then start the appropriate one
    stop_label_printer_discovery();
    stop_usb_detection();
    stop_bt_discovery();

    if (type == "usb") {
        detected_usb_printers_.clear();
        init_usb_printer_dropdown();
        start_usb_detection();
    } else if (type == "bluetooth") {
        bt_devices_.clear();
        init_bt_printer_dropdown();
        // BT discovery is on-demand via scan button
    } else {
        discovered_network_printers_.clear();
        raw_printers_.clear();
        ipp_printers_.clear();
        init_discovery_dropdown();
        start_label_printer_discovery();
    }
}

// ============================================================================
// LABEL COUNT
// ============================================================================

void LabelPrinterSettingsOverlay::init_label_count_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_label_count");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (!dropdown)
        return;

    // Determine max labels from current sheet template
    auto& settings = LabelPrinterSettingsManager::instance();
    const auto& templates = helix::label::get_sheet_templates();
    int tmpl_idx = std::clamp(settings.get_label_size_index(), 0,
                              static_cast<int>(templates.size()) - 1);
    int max_labels = templates[tmpl_idx].labels_per_sheet();

    std::string options;
    for (int i = 1; i <= max_labels; i++) {
        if (i > 1)
            options += "\n";
        options += fmt::format("{}", i);
    }
    lv_dropdown_set_options(dropdown, options.c_str());

    int saved_count = std::clamp(settings.get_label_count(), 1, max_labels);
    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(saved_count - 1));

    spdlog::trace("[{}] Label count dropdown initialized (max={})", get_name(), max_labels);
}

void LabelPrinterSettingsOverlay::handle_label_count_changed(int index) {
    int count = index + 1; // dropdown index 0 = "1 label"
    spdlog::info("[{}] Label count changed: {} (index {})", get_name(), count, index);
    LabelPrinterSettingsManager::instance().set_label_count(count);
}

// ============================================================================
// USB DETECTION
// ============================================================================

void LabelPrinterSettingsOverlay::init_usb_printer_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_usb_printers");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        lv_dropdown_set_options(dropdown, lv_tr("Searching..."));
    }
}

void LabelPrinterSettingsOverlay::start_usb_detection() {
    if (usb_detector_ && usb_detector_->is_polling())
        return;

    usb_detector_ = std::make_unique<helix::UsbPrinterDetector>();
    auto alive = alive_;
    usb_detector_->start_polling([this, alive](const std::vector<helix::UsbPrinterInfo>& printers) {
        if (!alive->load())
            return;
        on_usb_printers_detected(printers);
    });
    spdlog::debug("[{}] Started USB printer detection", get_name());
}

void LabelPrinterSettingsOverlay::stop_usb_detection() {
    if (usb_detector_) {
        usb_detector_->stop_polling();
        usb_detector_.reset();
        spdlog::debug("[{}] Stopped USB printer detection", get_name());
    }
}

void LabelPrinterSettingsOverlay::on_usb_printers_detected(
    const std::vector<helix::UsbPrinterInfo>& printers) {
    detected_usb_printers_ = printers;

    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_usb_printers");
    if (!row)
        return;
    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (!dropdown)
        return;

    std::string options;
    if (detected_usb_printers_.empty()) {
        options = lv_tr("No USB printers found");
    } else {
        for (const auto& p : detected_usb_printers_) {
            if (!options.empty())
                options += "\n";
            if (get_runtime_config()->is_test_mode()) {
                options += fmt::format("{} (Bus {}, Dev {})", p.product_name, p.bus, p.address);
            } else {
                options += p.product_name;
            }
        }
    }

    lv_dropdown_set_options(dropdown, options.c_str());
    spdlog::debug("[{}] USB detection: {} printers found", get_name(), printers.size());

    // Auto-select first if not configured yet
    if (!detected_usb_printers_.empty()) {
        auto& settings = LabelPrinterSettingsManager::instance();
        if (settings.get_usb_vid() == 0) {
            handle_usb_printer_selected(0);
            spdlog::info("[{}] Auto-selected USB printer: {}", get_name(),
                         detected_usb_printers_[0].product_name);
        }
    }
}

void LabelPrinterSettingsOverlay::handle_usb_printer_selected(int index) {
    if (index < 0 || index >= static_cast<int>(detected_usb_printers_.size()))
        return;

    const auto& printer = detected_usb_printers_[index];
    spdlog::info("[{}] Selected USB printer: {} ({:04x}:{:04x})", get_name(), printer.product_name,
                 printer.vid, printer.pid);

    auto& settings = LabelPrinterSettingsManager::instance();
    settings.set_usb_vid(printer.vid);
    settings.set_usb_pid(printer.pid);
    settings.set_usb_serial(printer.serial);
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void LabelPrinterSettingsOverlay::handle_address_changed() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_address_port");
    if (!row)
        return;

    lv_obj_t* input = lv_obj_find_by_name(row, "input_address");
    if (input) {
        const char* text = lv_textarea_get_text(input);
        std::string addr = text ? text : "";
        spdlog::info("[{}] Address changed: {}", get_name(), addr);
        LabelPrinterSettingsManager::instance().set_printer_address(addr);
    }
}

void LabelPrinterSettingsOverlay::handle_port_changed() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_address_port");
    if (!row)
        return;

    lv_obj_t* input = lv_obj_find_by_name(row, "input_port");
    if (input) {
        const char* text = lv_textarea_get_text(input);
        if (text && text[0] != '\0') {
            int port = std::atoi(text);
            if (port > 0 && port <= 65535) {
                spdlog::info("[{}] Port changed: {}", get_name(), port);
                LabelPrinterSettingsManager::instance().set_printer_port(port);
            } else {
                spdlog::warn("[{}] Invalid port: {}", get_name(), text);
                ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("Invalid port number"));
            }
        }
    }
}

void LabelPrinterSettingsOverlay::handle_label_size_changed(int index) {
    auto& settings = LabelPrinterSettingsManager::instance();
    auto sizes = get_sizes_for_current_printer();
    if (index >= 0 && index < static_cast<int>(sizes.size())) {
        spdlog::info("[{}] Label size changed: {} (index {})", get_name(), sizes[index].name,
                     index);
        settings.set_label_size_index(index);

        // Force QR-only for small square labels where text won't fit
        const auto& sz = sizes[index];
        bool force_qr = (sz.width_px <= 250 && sz.height_px > 0 && sz.height_px <= 250);
        if (overlay_root_) {
            lv_obj_t* preset_row = lv_obj_find_by_name(overlay_root_, "row_preset");
            if (preset_row) {
                lv_obj_t* dd = lv_obj_find_by_name(preset_row, "dropdown");
                if (dd) {
                    if (force_qr) {
                        lv_dropdown_set_options(dd, lv_tr("QR Only"));
                        lv_dropdown_set_selected(dd, 0);
                        lv_obj_add_state(dd, LV_STATE_DISABLED);
                        settings.set_label_preset(static_cast<int>(LabelPreset::MINIMAL));
                    } else {
                        // Restore full options
                        auto opts = fmt::format("{}\n{}\n{}",
                            lv_tr("Standard"), lv_tr("Compact"), lv_tr("QR Only"));
                        lv_dropdown_set_options(dd, opts.c_str());
                        lv_dropdown_set_selected(dd,
                            static_cast<uint32_t>(settings.get_label_preset()));
                        lv_obj_remove_state(dd, LV_STATE_DISABLED);
                    }
                }
            }
        }
    } else {
        spdlog::warn("[{}] Label size index {} out of range ({})", get_name(), index, sizes.size());
    }
}

void LabelPrinterSettingsOverlay::handle_preset_changed(int index) {
    if (index >= 0 && index <= 2) {
        spdlog::info("[{}] Preset changed: {} (index {})", get_name(),
                     label_preset_name(static_cast<LabelPreset>(index)), index);
        LabelPrinterSettingsManager::instance().set_label_preset(index);
    } else {
        spdlog::warn("[{}] Preset index {} out of range", get_name(), index);
    }
}

void LabelPrinterSettingsOverlay::handle_test_print() {
    auto& settings = LabelPrinterSettingsManager::instance();

    if (!settings.is_configured()) {
        const auto type = settings.get_printer_type();
        const char* msg;
        if (type == "usb") {
            msg = lv_tr("Connect a USB label printer first");
        } else if (type == "bluetooth") {
            msg = lv_tr("Scan and select a Bluetooth printer first");
        } else {
            msg = lv_tr("Enter printer IP address first");
        }
        ToastManager::instance().show(ToastSeverity::WARNING, msg);
        return;
    }

    spdlog::info("[{}] Test print requested", get_name());

    // Create a mock spool for the test label
    SpoolInfo mock_spool;
    mock_spool.id = 42;
    mock_spool.vendor = "Hatchbox";
    mock_spool.material = "PLA";
    mock_spool.color_name = "Red";
    mock_spool.color_hex = "#FF0000";
    mock_spool.remaining_weight_g = 800;
    mock_spool.initial_weight_g = 1000;
    mock_spool.remaining_length_m = 265;
    mock_spool.lot_nr = "LOT-2026A";
    mock_spool.comment = "Sample spool";
    mock_spool.nozzle_temp_recommended = 210;
    mock_spool.bed_temp_recommended = 60;

    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Printing test label..."), 2000);

    auto alive = alive_;
    helix::print_spool_label(mock_spool, [alive](bool success, const std::string& error) {
        if (success) {
            ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Test label printed"),
                                          2000);
        } else {
            spdlog::error("[LabelPrinterSettings] Test print failed: {}", error);
            ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Print failed"), 3000);
        }

        // Note: Don't refresh BT connection state here — the overlay's bt_ctx_
        // is separate from the print thread's persistent connection and may be
        // invalid. The user can re-open the overlay or press Scan to refresh.
    });
}

// ============================================================================
// BLUETOOTH DISCOVERY
// ============================================================================

void LabelPrinterSettingsOverlay::init_bt_printer_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_bt_printers");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (!dropdown)
        return;

    // Pre-populate with saved paired device if available
    auto& settings = LabelPrinterSettingsManager::instance();
    std::string saved_addr = settings.get_bt_address();
    std::string saved_name = settings.get_bt_name();

    if (!saved_addr.empty()) {
        // Add saved device to bt_devices_ so selection index works
        BtDeviceInfo saved_dev;
        saved_dev.mac = saved_addr;
        saved_dev.name = saved_name.empty() ? saved_addr : saved_name;
        // Name-based BLE detection overrides saved transport setting —
        // some devices were incorrectly saved as "spp" before the fix
        saved_dev.is_ble = helix::bluetooth::name_suggests_ble(saved_dev.name.c_str())
            || (settings.get_bt_transport() == "ble");
        if (saved_dev.is_ble && settings.get_bt_transport() != "ble") {
            spdlog::info("[{}] Correcting saved transport from spp to ble for {}",
                         get_name(), saved_dev.name);
            settings.set_bt_transport("ble");
        }

        // Show saved device immediately (assume saved = usable)
        saved_dev.paired = false;
        saved_dev.connected = false;

        bt_devices_.clear();
        bt_devices_.push_back(saved_dev);

        // Show "(Saved)" immediately — BLE devices don't persist pairing so
        // our settings ARE the "saved" state
        lv_dropdown_set_options(
            dropdown,
            bt_device_label(saved_dev.name, false, true, false).c_str());
        lv_dropdown_set_selected(dropdown, 0);

        // Enable connect button (will update after async check)
        lv_obj_t* btn = lv_obj_find_by_name(overlay_root_, "btn_bt_connect");
        if (btn) {
            lv_obj_remove_state(btn, LV_STATE_DISABLED);
        }

        // Check actual paired/connected state asynchronously to avoid
        // blocking the UI thread on D-Bus calls (25-second default timeout)
        auto& loader = helix::bluetooth::BluetoothLoader::instance();
        if (!bt_ctx_ && loader.is_available() && loader.init) {
            bt_ctx_ = loader.init();
        }
        if (bt_ctx_ && loader.is_paired) {
            auto alive = alive_;
            auto* ctx = bt_ctx_;
            std::string addr = saved_addr;
            std::thread([alive, ctx, addr]() {
                auto& ldr = helix::bluetooth::BluetoothLoader::instance();
                int paired_r = ldr.is_paired ? ldr.is_paired(ctx, addr.c_str()) : -1;
                bool connected = check_bt_connected(ctx, addr);
                spdlog::debug("[Label Printer] Async BT state: is_paired={} connected={}",
                              paired_r, connected);

                helix::ui::queue_update([alive, paired_r, connected, addr]() {
                    if (!alive->load())
                        return;
                    auto& ov = get_label_printer_settings_overlay();
                    for (auto& dev : ov.bt_devices_) {
                        if (dev.mac == addr) {
                            dev.paired = (paired_r == 1);
                            dev.connected = connected;
                            break;
                        }
                    }
                    // Refresh dropdown with actual state
                    if (ov.overlay_root_) {
                        lv_obj_t* row = lv_obj_find_by_name(ov.overlay_root_, "row_bt_printers");
                        if (row) {
                            lv_obj_t* dd = lv_obj_find_by_name(row, "dropdown");
                            if (dd) {
                                std::string options;
                                for (const auto& d : ov.bt_devices_) {
                                    if (!options.empty())
                                        options += "\n";
                                    options += bt_device_label(d.name, d.paired, !d.mac.empty(),
                                                               d.connected);
                                }
                                lv_dropdown_set_options(dd, options.c_str());
                            }
                        }
                        // Update connect button
                        lv_obj_t* b = lv_obj_find_by_name(ov.overlay_root_, "btn_bt_connect");
                        if (b) {
                            if (connected)
                                lv_obj_add_state(b, LV_STATE_DISABLED);
                            else
                                lv_obj_remove_state(b, LV_STATE_DISABLED);
                        }
                    }
                });
            }).detach();
        }
    } else {
        lv_dropdown_set_options(dropdown, lv_tr("Press Scan to search"));
    }
}

void LabelPrinterSettingsOverlay::start_bt_discovery() {
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
    // Keep saved paired device so it stays visible during scan
    bt_devices_.erase(std::remove_if(bt_devices_.begin(), bt_devices_.end(),
                                     [](const BtDeviceInfo& d) { return !d.paired; }),
                      bt_devices_.end());

    // Update UI to show scanning state
    lv_subject_set_int(&bt_scanning_subject_, 1);
    if (overlay_root_) {
        lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_bt_printers");
        if (row) {
            lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
            if (dropdown) {
                lv_dropdown_set_options(dropdown, lv_tr("Scanning..."));
            }
        }
    }

    // Set up C callback safety context
    bt_discovery_ctx_ = std::make_unique<BtDiscoveryContext>();
    bt_discovery_ctx_->alive.store(true);
    bt_discovery_ctx_->overlay = this;

    auto* disc_ctx = bt_discovery_ctx_.get();
    auto* ctx = bt_ctx_;
    auto alive = alive_;

    // Run discovery on a detached thread
    std::thread([ctx, disc_ctx, alive, &loader]() {
        loader.discover(
            ctx, 15000,
            [](const helix_bt_device* dev, void* user_data) {
                auto* dctx = static_cast<BtDiscoveryContext*>(user_data);
                if (!dctx->alive.load())
                    return;

                // Copy device info to avoid dangling pointers
                BtDeviceInfo info;
                info.mac = dev->mac ? dev->mac : "";
                info.name = dev->name ? dev->name : "Unknown";
                info.paired = dev->paired;
                info.connected = false; // updated on UI thread below
                info.is_ble = dev->is_ble;

                // Marshal to UI thread
                helix::ui::queue_update([dctx, info]() {
                    if (!dctx->alive.load())
                        return;
                    auto* overlay = dctx->overlay;

                    // Add to device list (avoid duplicates)
                    bool found = false;
                    for (const auto& existing : overlay->bt_devices_) {
                        if (existing.mac == info.mac) {
                            found = true;
                            break;
                        }
                    }

                    if (!found) {
                        overlay->bt_devices_.push_back(info);
                        spdlog::debug("[Label Printer] BT discovered: {} ({})", info.name,
                                      info.mac);

                        // Update dropdown
                        if (overlay->overlay_root_) {
                            lv_obj_t* row =
                                lv_obj_find_by_name(overlay->overlay_root_, "row_bt_printers");
                            if (row) {
                                lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
                                if (dropdown) {
                                    std::string options;
                                    for (const auto& d : overlay->bt_devices_) {
                                        if (!options.empty())
                                            options += "\n";
                                        options += bt_device_label(d.name, d.paired, !d.mac.empty(), d.connected);
                                    }
                                    lv_dropdown_set_options(dropdown, options.c_str());
                                }
                            }
                        }
                    }
                });
            },
            disc_ctx);

        // Discovery completed (timeout or stopped)
        helix::ui::queue_update([disc_ctx, alive]() {
            if (!alive->load())
                return;
            if (!disc_ctx->alive.load())
                return;

            auto* overlay = disc_ctx->overlay;
            overlay->bt_discovering_ = false;
            lv_subject_set_int(&overlay->bt_scanning_subject_, 0);

            if (overlay->overlay_root_) {
                lv_obj_t* row = lv_obj_find_by_name(overlay->overlay_root_, "row_bt_printers");
                if (row) {
                    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
                    if (dropdown) {
                        if (overlay->bt_devices_.empty()) {
                            lv_dropdown_set_options(dropdown, lv_tr("No Bluetooth printers found"));
                        } else {
                            // Refresh dropdown with final device list (handles case where
                            // all discovered devices were already known — dropdown still
                            // shows "Scanning..." without this)
                            std::string options;
                            for (const auto& d : overlay->bt_devices_) {
                                if (!options.empty())
                                    options += "\n";
                                options += bt_device_label(d.name, d.paired, !d.mac.empty(), d.connected);
                            }
                            lv_dropdown_set_options(dropdown, options.c_str());
                        }
                    }
                }
            }

            spdlog::info("[Label Printer] BT discovery finished, {} devices found",
                         overlay->bt_devices_.size());
        });
    }).detach();

    spdlog::info("[{}] Started Bluetooth discovery", get_name());
}

void LabelPrinterSettingsOverlay::stop_bt_discovery() {
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
    lv_subject_set_int(&bt_scanning_subject_, 0);
    spdlog::debug("[{}] Stopped Bluetooth discovery", get_name());
}

void LabelPrinterSettingsOverlay::handle_bt_printer_selected(int index) {
    if (index < 0 || index >= static_cast<int>(bt_devices_.size()))
        return;

    const auto& device = bt_devices_[index];
    spdlog::info("[{}] Selected BT printer: {} ({})", get_name(), device.name, device.mac);

    // If not paired and not already saved (i.e. a newly discovered device), prompt for pairing.
    // Saved devices (address matches settings) can skip re-pairing — BLE devices don't
    // persist pairing between sessions but the saved address is enough to reconnect.
    auto& settings = LabelPrinterSettingsManager::instance();
    bool is_saved = (device.mac == settings.get_bt_address());
    if (!device.paired && !is_saved) {
        // Allocate a copy of the MAC for the modal callback user_data
        auto* mac_copy = new std::string(device.mac);

        auto msg = fmt::format("{} {}?", lv_tr("Pair with"), device.name);
        auto* dialog = helix::ui::modal_show_confirmation(
            lv_tr("Pair Bluetooth Printer"), msg.c_str(), ModalSeverity::Info, lv_tr("Pair"),
            on_pair_confirm, on_pair_cancel, mac_copy);

        if (!dialog) {
            delete mac_copy;
            spdlog::warn("[{}] Failed to show pairing modal", get_name());
        }
        return;
    }

    // Already paired or saved — update settings
    settings.set_bt_address(device.mac);
    settings.set_bt_name(device.name);
    settings.set_bt_transport(device.is_ble ? "ble" : "spp");

    // Enable connect button when not connected
    if (overlay_root_) {
        lv_obj_t* btn = lv_obj_find_by_name(overlay_root_, "btn_bt_connect");
        if (btn) {
            if (!device.connected) {
                lv_obj_remove_state(btn, LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(btn, LV_STATE_DISABLED);
            }
        }
    }

    // Refresh label size dropdown for the selected printer's transport
    init_label_size_dropdown();
}

void LabelPrinterSettingsOverlay::handle_bt_scan() {
    if (bt_discovering_) {
        stop_bt_discovery();
    } else {
        start_bt_discovery();
    }
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void LabelPrinterSettingsOverlay::on_address_done(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_address_done");
    get_label_printer_settings_overlay().handle_address_changed();
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_port_done(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_port_done");
    get_label_printer_settings_overlay().handle_port_changed();
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_label_size_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_label_size_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_label_printer_settings_overlay().handle_label_size_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_preset_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_preset_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_label_printer_settings_overlay().handle_preset_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_test_print(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_test_print");
    get_label_printer_settings_overlay().handle_test_print();
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_printer_selected(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_printer_selected");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_label_printer_settings_overlay().handle_printer_selected(index);
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_type_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_type_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_label_printer_settings_overlay().handle_type_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_usb_printer_selected(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_usb_printer_selected");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_label_printer_settings_overlay().handle_usb_printer_selected(index);
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_bt_printer_selected(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_bt_printer_selected");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_label_printer_settings_overlay().handle_bt_printer_selected(index);
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_bt_scan(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_bt_scan");
    get_label_printer_settings_overlay().handle_bt_scan();
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_label_count_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_label_count_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_label_printer_settings_overlay().handle_label_count_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::handle_bt_connect() {
    auto& settings = LabelPrinterSettingsManager::instance();
    std::string mac = settings.get_bt_address();
    if (mac.empty())
        return;

    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available())
        return;

    // Ensure BT context
    if (!bt_ctx_ && loader.init) {
        bt_ctx_ = loader.init();
    }
    if (!bt_ctx_)
        return;

    // Disable button while connecting
    if (overlay_root_) {
        lv_obj_t* btn = lv_obj_find_by_name(overlay_root_, "btn_bt_connect");
        if (btn)
            lv_obj_add_state(btn, LV_STATE_DISABLED);
    }

    auto alive = alive_;
    auto* ctx = bt_ctx_;

    std::thread([mac, ctx, alive]() {
        auto& ldr = helix::bluetooth::BluetoothLoader::instance();
        auto* init_ctx = ctx;
        int ret = -1;

        if (ldr.pair) {
            ret = ldr.pair(init_ctx, mac.c_str());
        }

        // If pair failed (device may have been removed from BlueZ cache),
        // try a brief scan to rediscover, then retry
        if (ret < 0 && ldr.discover && ldr.pair) {
            spdlog::info("[LabelPrinterSettings] Pair failed, scanning to rediscover {}...", mac);
            struct ScanCtx { std::string target; bool found = false; };
            ScanCtx scan_ctx{mac};
            ldr.discover(init_ctx, 8000,
                [](const helix_bt_device* dev, void* user_data) {
                    auto* sc = static_cast<ScanCtx*>(user_data);
                    if (dev->mac && sc->target == dev->mac) {
                        sc->found = true;
                    }
                },
                &scan_ctx);

            if (scan_ctx.found) {
                spdlog::info("[LabelPrinterSettings] Rediscovered {}, retrying pair", mac);
                ret = ldr.pair(init_ctx, mac.c_str());
            } else {
                spdlog::warn("[LabelPrinterSettings] Device {} not found during rescan", mac);
            }
        }

        // For SPP printers (Brother QL, etc.), BlueZ "Connected" only reflects
        // an active RFCOMM/BLE session — pair success is sufficient. Only check
        // is_connected for BLE devices that maintain persistent connections.
        bool paired_ok = (ret == 0);
        bool connected = false;
        if (paired_ok && ldr.is_paired) {
            paired_ok = (ldr.is_paired(init_ctx, mac.c_str()) == 1);
        }

        helix::ui::queue_update([mac, paired_ok, connected, alive]() {
            if (!alive->load())
                return;

            auto& ov = get_label_printer_settings_overlay();

            // Update device state
            for (auto& dev : ov.bt_devices_) {
                if (dev.mac == mac) {
                    dev.paired = paired_ok;
                    dev.connected = connected;
                    break;
                }
            }

            // Refresh dropdown labels
            if (ov.overlay_root_) {
                lv_obj_t* row = lv_obj_find_by_name(ov.overlay_root_, "row_bt_printers");
                if (row) {
                    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
                    if (dropdown) {
                        std::string options;
                        for (const auto& d : ov.bt_devices_) {
                            if (!options.empty())
                                options += "\n";
                            options += bt_device_label(d.name, d.paired, !d.mac.empty(), d.connected);
                        }
                        lv_dropdown_set_options(dropdown, options.c_str());
                    }
                }

                // Update connect button state
                lv_obj_t* btn = lv_obj_find_by_name(ov.overlay_root_, "btn_bt_connect");
                if (btn) {
                    if (paired_ok) {
                        lv_obj_add_state(btn, LV_STATE_DISABLED);
                    } else {
                        lv_obj_remove_state(btn, LV_STATE_DISABLED);
                    }
                }
            }

            if (paired_ok) {
                ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Paired"), 2000);
            } else {
                ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Pairing failed"),
                                              2000);
            }
        });
    }).detach();
}

void LabelPrinterSettingsOverlay::on_bt_connect(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_bt_connect");
    get_label_printer_settings_overlay().handle_bt_connect();
    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_pair_confirm(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_pair_confirm");

    auto* mac_copy = static_cast<std::string*>(lv_event_get_user_data(e));
    std::string mac = *mac_copy;
    delete mac_copy;

    // Close the modal via the Modal system
    auto* top = Modal::get_top();
    if (top)
        Modal::hide(top);

    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available() || !loader.pair) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Bluetooth not available"));
        return;
    }

    auto& overlay = get_label_printer_settings_overlay();
    auto* bt_ctx = overlay.bt_ctx_;

    if (!bt_ctx) {
        ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Bluetooth not initialized"));
        return;
    }

    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Pairing..."), 5000);

    auto alive = overlay.alive_;

    // Pair on a detached thread
    std::thread([mac, bt_ctx, alive]() {
        auto& ldr = helix::bluetooth::BluetoothLoader::instance();
        int ret = ldr.pair(bt_ctx, mac.c_str());

        // Check paired/connected state on this worker thread (D-Bus calls can
        // take seconds — must not block the UI thread)
        int paired_r = -1;
        bool connected = false;
        if (ret == 0) {
            paired_r = ldr.is_paired ? ldr.is_paired(bt_ctx, mac.c_str()) : -1;
            connected = check_bt_connected(bt_ctx, mac);
            spdlog::info("[LabelPrinterSettings] Post-pair: is_paired={} connected={}",
                         paired_r, connected);
        }

        helix::ui::queue_update([ret, mac, alive, bt_ctx, paired_r, connected]() {
            if (!alive->load())
                return;

            if (ret == 0) {
                ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Paired successfully"),
                                              2000);

                // Update device info and save settings
                auto& ov = get_label_printer_settings_overlay();
                for (auto& dev : ov.bt_devices_) {
                    if (dev.mac == mac) {
                        dev.paired = (paired_r == 1);
                        dev.connected = connected;
                        auto& settings = LabelPrinterSettingsManager::instance();
                        settings.set_bt_address(mac);
                        settings.set_bt_name(dev.name);
                        settings.set_bt_transport(dev.is_ble ? "ble" : "spp");
                        ov.init_label_size_dropdown();
                        break;
                    }
                }

                // Refresh dropdown to show paired checkmark
                if (ov.overlay_root_) {
                    lv_obj_t* row = lv_obj_find_by_name(ov.overlay_root_, "row_bt_printers");
                    if (row) {
                        lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
                        if (dropdown) {
                            std::string options;
                            for (const auto& d : ov.bt_devices_) {
                                if (!options.empty())
                                    options += "\n";
                                options += bt_device_label(d.name, d.paired, !d.mac.empty(), d.connected);
                            }
                            lv_dropdown_set_options(dropdown, options.c_str());
                        }
                    }
                }
            } else {
                auto& ldr = helix::bluetooth::BluetoothLoader::instance();
                const char* err = ldr.last_error ? ldr.last_error(bt_ctx) : "Unknown error";
                spdlog::error("[LabelPrinterSettings] Pairing failed: {}", err);
                ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Pairing failed"), 3000);
            }
        });
    }).detach();

    LVGL_SAFE_EVENT_CB_END();
}

void LabelPrinterSettingsOverlay::on_pair_cancel(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LabelPrinterSettings] on_pair_cancel");

    auto* mac_copy = static_cast<std::string*>(lv_event_get_user_data(e));
    delete mac_copy;

    // Close the modal via the Modal system
    auto* top = Modal::get_top();
    if (top)
        Modal::hide(top);

    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
