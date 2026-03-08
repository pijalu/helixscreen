// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_label_printer.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"

#include "brother_ql_printer.h"
#include "label_bitmap.h"
#include "label_printer_settings.h"
#include "static_panel_registry.h"
#include "ui_toast_manager.h"

#include <algorithm>

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<LabelPrinterSettingsOverlay> g_label_printer_overlay;

LabelPrinterSettingsOverlay& get_label_printer_settings_overlay() {
    if (!g_label_printer_overlay) {
        g_label_printer_overlay = std::make_unique<LabelPrinterSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "LabelPrinterSettingsOverlay", []() { g_label_printer_overlay.reset(); });
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
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void LabelPrinterSettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void LabelPrinterSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_lp_label_size_changed", on_label_size_changed},
        {"on_lp_preset_changed", on_preset_changed},
        {"on_lp_test_print", on_test_print},
        {"on_lp_printer_selected", on_printer_selected},
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

    init_address_input();
    init_port_input();
    init_label_size_dropdown();
    init_preset_dropdown();
    init_discovery_dropdown();
    start_label_printer_discovery();
    inputs_initialized_ = true;
}

void LabelPrinterSettingsOverlay::on_deactivate() {
    stop_label_printer_discovery();
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
        auto sizes = BrotherQLPrinter::supported_sizes();

        std::string options;
        for (size_t i = 0; i < sizes.size(); i++) {
            if (i > 0)
                options += "\n";
            options += sizes[i].name;
        }

        if (!options.empty()) {
            lv_dropdown_set_options(dropdown, options.c_str());
            lv_dropdown_set_selected(dropdown,
                                     static_cast<uint32_t>(settings.get_label_size_index()));
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
            fmt::format("{}\n{}\n{}", lv_tr("Standard"), lv_tr("Compact"), lv_tr("Minimal"));
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

/**
 * @brief Score how likely a discovered printer is a label printer
 *
 * Checks the display name (from TXT ty/product or hostname) for known
 * label printer model patterns. Higher score = more likely label printer.
 * Returns 0 for printers that are definitely NOT label printers.
 */
static int label_printer_score(const DiscoveredPrinter& printer) {
    // Case-insensitive matching on the display name and hostname
    auto lower = [](const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(), ::tolower);
        return out;
    };
    std::string name = lower(printer.name);
    std::string host = lower(printer.hostname);

    // Strong signals — these are definitely label printers
    // Brother QL series (QL-800, QL-820NWB, QL-1110NWB, etc.)
    if (name.find("ql-") != std::string::npos || name.find("ql ") != std::string::npos)
        return 100;
    // Brother TD series (thermal direct label printers)
    if (name.find("td-") != std::string::npos || name.find("td ") != std::string::npos)
        return 100;
    // DYMO LabelWriter
    if (name.find("labelwriter") != std::string::npos || name.find("dymo") != std::string::npos)
        return 90;
    // Zebra label printers (ZD, ZT, GK, GX series)
    if (name.find("zebra") != std::string::npos)
        return 80;
    // Rollo, Munbyn, JADENS — common USB/network label printers
    if (name.find("rollo") != std::string::npos || name.find("munbyn") != std::string::npos)
        return 80;
    // Generic "label" in name
    if (name.find("label") != std::string::npos)
        return 70;

    // Moderate signals from hostname (BRW prefix = Brother wireless)
    if (host.find("brw") != std::string::npos)
        return 50;

    // Negative signals — definitely NOT label printers
    if (name.find("laserjet") != std::string::npos || name.find("officejet") != std::string::npos)
        return 0;
    if (name.find("inkjet") != std::string::npos || name.find("pixma") != std::string::npos)
        return 0;
    if (name.find("ecotank") != std::string::npos || name.find("envy") != std::string::npos)
        return 0;

    // Unknown — could be anything, low score but still show
    return 10;
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
        int score = label_printer_score(p);
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
    auto sizes = BrotherQLPrinter::supported_sizes();
    if (index >= 0 && index < static_cast<int>(sizes.size())) {
        spdlog::info("[{}] Label size changed: {} (index {})", get_name(), sizes[index].name,
                     index);
        LabelPrinterSettingsManager::instance().set_label_size_index(index);
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
        ToastManager::instance().show(ToastSeverity::WARNING, lv_tr("Enter printer IP address first"));
        return;
    }

    spdlog::info("[{}] Test print requested ({}:{})", get_name(),
                 settings.get_printer_address(), settings.get_printer_port());

    // Create a simple test bitmap — small border pattern
    auto bitmap = helix::LabelBitmap::create(200, 100);
    for (int x = 0; x < 200; x++) {
        bitmap.set_pixel(x, 0, true);
        bitmap.set_pixel(x, 99, true);
    }
    for (int y = 0; y < 100; y++) {
        bitmap.set_pixel(0, y, true);
        bitmap.set_pixel(199, y, true);
    }

    auto sizes = helix::BrotherQLPrinter::supported_sizes();
    int size_idx = std::clamp(settings.get_label_size_index(), 0,
                              static_cast<int>(sizes.size()) - 1);

    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Printing test label..."), 2000);

    static helix::BrotherQLPrinter test_printer;
    test_printer.print_label(
        settings.get_printer_address(), settings.get_printer_port(), bitmap, sizes[size_idx],
        [](bool success, const std::string& error) {
            if (success) {
                ToastManager::instance().show(ToastSeverity::SUCCESS, lv_tr("Test label printed"),
                                              2000);
            } else {
                spdlog::error("[LabelPrinterSettings] Test print failed: {}", error);
                ToastManager::instance().show(ToastSeverity::ERROR, lv_tr("Print failed"), 3000);
            }
        });
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

} // namespace helix::settings
