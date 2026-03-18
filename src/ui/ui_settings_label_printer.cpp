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
#include "bt_discovery_utils.h"
#include "label_printer_settings.h"
#include "label_printer_utils.h"
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

/// Build dropdown label for a BT device showing paired/connected status.
static std::string bt_device_label(const std::string& name, bool paired, bool connected) {
    std::string label = name;
    if (paired && connected) {
        label += " (Paired, Connected)";
    } else if (paired) {
        label += " (Paired)";
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
    if (mdns_discovery_ && mdns_discovery_->is_discovering()) {
        return;
    }

    mdns_discovery_ = std::make_unique<MdnsDiscovery>("_pdl-datastream._tcp.local");

    auto alive = alive_;
    mdns_discovery_->start_discovery([this, alive](const std::vector<DiscoveredPrinter>& printers) {
        if (!alive->load()) {
            return;
        }
        on_printers_discovered(printers);
    });

    spdlog::debug("[{}] Started label printer mDNS discovery", get_name());
}

void LabelPrinterSettingsOverlay::stop_label_printer_discovery() {
    if (mdns_discovery_) {
        mdns_discovery_->stop_discovery();
        mdns_discovery_.reset();
        spdlog::debug("[{}] Stopped label printer mDNS discovery", get_name());
    }
}

void LabelPrinterSettingsOverlay::on_printers_discovered(
    const std::vector<DiscoveredPrinter>& printers) {
    // Score and sort: likely label printers first, non-label printers excluded
    struct ScoredPrinter {
        DiscoveredPrinter printer;
        int score;
    };

    std::vector<ScoredPrinter> scored;
    for (const auto& p : printers) {
        int score = helix::label_printer_score(p);
        if (score > 0) {
            scored.push_back({p, score});
        } else {
            spdlog::debug("[{}] Filtered out non-label printer: {} ({})", get_name(), p.name,
                          p.ip_address);
        }
    }

    // Sort by score descending (most likely label printers first)
    std::sort(scored.begin(), scored.end(),
              [](const ScoredPrinter& a, const ScoredPrinter& b) { return a.score > b.score; });

    // Store only the filtered/sorted list for selection
    discovered_printers_.clear();
    for (const auto& sp : scored) {
        discovered_printers_.push_back(sp.printer);
    }

    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_discovered_printers");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (!dropdown)
        return;

    std::string options;
    if (discovered_printers_.empty()) {
        options = lv_tr("No label printers found");
    } else {
        for (const auto& p : discovered_printers_) {
            if (!options.empty()) {
                options += "\n";
            }
            options += p.name + " (" + p.ip_address + ")";
        }
    }

    lv_dropdown_set_options(dropdown, options.c_str());

    spdlog::debug("[{}] Discovery update: {} total, {} likely label printers", get_name(),
                  printers.size(), discovered_printers_.size());

    // Auto-select the first discovered printer if no address is configured yet
    if (!discovered_printers_.empty()) {
        auto& settings = LabelPrinterSettingsManager::instance();
        if (!settings.is_configured()) {
            handle_printer_selected(0);
            spdlog::info("[{}] Auto-selected first discovered printer: {}", get_name(),
                         discovered_printers_[0].name);
        }
    }
}

void LabelPrinterSettingsOverlay::handle_printer_selected(int index) {
    if (index < 0 || index >= static_cast<int>(discovered_printers_.size())) {
        return;
    }

    const auto& printer = discovered_printers_[index];
    spdlog::info("[{}] Selected printer: {} ({}:{})", get_name(), printer.name, printer.ip_address,
                 printer.port);

    auto& settings = LabelPrinterSettingsManager::instance();
    settings.set_printer_address(printer.ip_address);
    settings.set_printer_port(printer.port);

    // Update address and port input fields
    if (overlay_root_) {
        lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_address_port");
        if (row) {
            lv_obj_t* addr_input = lv_obj_find_by_name(row, "input_address");
            if (addr_input) {
                lv_textarea_set_text(addr_input, printer.ip_address.c_str());
            }

            lv_obj_t* port_input = lv_obj_find_by_name(row, "input_port");
            if (port_input) {
                auto port_str = fmt::format("{}", printer.port);
                lv_textarea_set_text(port_input, port_str.c_str());
            }
        }
    }
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
            options = fmt::format("{}\n{}\n{}", lv_tr("Network"), lv_tr("USB"), lv_tr("Bluetooth"));
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
    std::string type;
    if (index == 0)
        type = "network";
    else if (index == 1)
        type = "usb";
    else if (index == 2)
        type = "bluetooth";
    else
        type = "network";

    spdlog::info("[{}] Printer type changed: {} (index={})", get_name(), type, index);

    LabelPrinterSettingsManager::instance().set_printer_type(type);

    // Refresh label size dropdown for new backend
    init_label_size_dropdown();

    // Reset label size to first option for new backend
    LabelPrinterSettingsManager::instance().set_label_size_index(0);

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
        discovered_printers_.clear();
        init_discovery_dropdown();
        start_label_printer_discovery();
    }
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
        saved_dev.paired = true;
        saved_dev.is_ble = (settings.get_bt_transport() == "ble");

        // Check current connection state (need a BT context)
        auto& loader = helix::bluetooth::BluetoothLoader::instance();
        if (!bt_ctx_ && loader.is_available() && loader.init) {
            bt_ctx_ = loader.init();
        }
        saved_dev.connected = check_bt_connected(bt_ctx_, saved_addr);

        bt_devices_.clear();
        bt_devices_.push_back(saved_dev);

        lv_dropdown_set_options(
            dropdown,
            bt_device_label(saved_dev.name, saved_dev.paired, saved_dev.connected).c_str());
        lv_dropdown_set_selected(dropdown, 0);

        // Enable connect button if paired but not connected
        lv_obj_t* btn = lv_obj_find_by_name(overlay_root_, "btn_bt_connect");
        if (btn) {
            if (saved_dev.paired && !saved_dev.connected) {
                lv_obj_remove_state(btn, LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(btn, LV_STATE_DISABLED);
            }
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

    // Update dropdown to show scanning state
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
                                        options += bt_device_label(d.name, d.paired, d.connected);
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
                                options += bt_device_label(d.name, d.paired, d.connected);
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
    spdlog::debug("[{}] Stopped Bluetooth discovery", get_name());
}

void LabelPrinterSettingsOverlay::handle_bt_printer_selected(int index) {
    if (index < 0 || index >= static_cast<int>(bt_devices_.size()))
        return;

    const auto& device = bt_devices_[index];
    spdlog::info("[{}] Selected BT printer: {} ({})", get_name(), device.name, device.mac);

    // If not paired, prompt for pairing
    if (!device.paired) {
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

    // Already paired — save settings
    auto& settings = LabelPrinterSettingsManager::instance();
    settings.set_bt_address(device.mac);
    settings.set_bt_name(device.name);
    settings.set_bt_transport(device.is_ble ? "ble" : "spp");

    // Enable/disable connect button based on connection state
    if (overlay_root_) {
        lv_obj_t* btn = lv_obj_find_by_name(overlay_root_, "btn_bt_connect");
        if (btn) {
            if (device.paired && !device.connected) {
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
        // BlueZ Device1.Connect() — establishes BLE connection
        auto& ldr = helix::bluetooth::BluetoothLoader::instance();
        auto* init_ctx = ctx;
        int ret = -1;
        if (ldr.pair) {
            // pair() already does Connect() for BLE devices
            ret = ldr.pair(init_ctx, mac.c_str());
        }

        bool connected = (ret == 0);
        if (connected && ldr.is_connected) {
            connected = (ldr.is_connected(init_ctx, mac.c_str()) == 1);
        }

        helix::ui::queue_update([mac, connected, alive]() {
            if (!alive->load())
                return;

            auto& ov = get_label_printer_settings_overlay();

            // Update device state
            for (auto& dev : ov.bt_devices_) {
                if (dev.mac == mac) {
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
                            options += bt_device_label(d.name, d.paired, d.connected);
                        }
                        lv_dropdown_set_options(dropdown, options.c_str());
                    }
                }

                // Update connect button state
                lv_obj_t* btn = lv_obj_find_by_name(ov.overlay_root_, "btn_bt_connect");
                if (btn) {
                    if (connected) {
                        lv_obj_add_state(btn, LV_STATE_DISABLED);
                    } else {
                        lv_obj_remove_state(btn, LV_STATE_DISABLED);
                    }
                }
            }

            if (connected) {
                ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Connected"), 2000);
            } else {
                ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Connection failed"),
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

        helix::ui::queue_update([ret, mac, alive, bt_ctx]() {
            if (!alive->load())
                return;

            if (ret == 0) {
                ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Paired successfully"),
                                              2000);

                // Find the device info, mark paired, and save settings
                auto& ov = get_label_printer_settings_overlay();
                for (auto& dev : ov.bt_devices_) {
                    if (dev.mac == mac) {
                        dev.paired = true;
                        dev.connected = check_bt_connected(ov.bt_ctx_, mac);
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
                                options += bt_device_label(d.name, d.paired, d.connected);
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
