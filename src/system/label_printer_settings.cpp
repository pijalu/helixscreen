// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "label_printer_settings.h"

#include "config.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"

using namespace helix;

LabelPrinterSettingsManager& LabelPrinterSettingsManager::instance() {
    static LabelPrinterSettingsManager instance;
    return instance;
}

LabelPrinterSettingsManager::LabelPrinterSettingsManager() {
    spdlog::trace("[LabelPrinterSettings] Constructor");
}

void LabelPrinterSettingsManager::init_subjects() {
    if (subjects_initialized_) {
        spdlog::debug("[LabelPrinterSettings] Subjects already initialized, skipping");
        return;
    }

    spdlog::debug("[LabelPrinterSettings] Initializing subjects");

    Config* config = Config::get_instance();

    // Printer type: 0=network, 1=usb
    std::string type_str = config->get<std::string>("/label_printer/type", "network");
    int type_int = (type_str == "usb") ? 1 : 0;
    UI_MANAGED_SUBJECT_INT(printer_type_subject_, type_int, "label_printer_type", subjects_);

    // Configured flag: depends on printer type
    int configured = is_configured() ? 1 : 0;
    UI_MANAGED_SUBJECT_INT(printer_configured_subject_, configured, "label_printer_configured",
                           subjects_);

    subjects_initialized_ = true;

    StaticSubjectRegistry::instance().register_deinit(
        "LabelPrinterSettingsManager",
        []() { LabelPrinterSettingsManager::instance().deinit_subjects(); });

    spdlog::debug("[LabelPrinterSettings] Subjects initialized: type='{}', configured={}",
                  type_str, configured);
}

void LabelPrinterSettingsManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[LabelPrinterSettings] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::trace("[LabelPrinterSettings] Subjects deinitialized");
}

// =============================================================================
// GETTERS / SETTERS
// =============================================================================

std::string LabelPrinterSettingsManager::get_printer_type() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/label_printer/type", "network");
}

void LabelPrinterSettingsManager::set_printer_type(const std::string& type) {
    spdlog::info("[LabelPrinterSettings] set_printer_type('{}')", type);

    Config* config = Config::get_instance();
    config->set<std::string>("/label_printer/type", type);
    config->save();

    // Update type subject
    if (subjects_initialized_) {
        int type_int = (type == "usb") ? 1 : 0;
        lv_subject_set_int(&printer_type_subject_, type_int);
        lv_subject_set_int(&printer_configured_subject_, is_configured() ? 1 : 0);
    }
}

std::string LabelPrinterSettingsManager::get_printer_address() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/label_printer/address", "");
}

void LabelPrinterSettingsManager::set_printer_address(const std::string& addr) {
    spdlog::info("[LabelPrinterSettings] set_printer_address('{}')", addr);

    Config* config = Config::get_instance();
    config->set<std::string>("/label_printer/address", addr);
    config->save();

    // Update configured subject
    if (subjects_initialized_) {
        lv_subject_set_int(&printer_configured_subject_, addr.empty() ? 0 : 1);
    }
}

int LabelPrinterSettingsManager::get_printer_port() const {
    Config* config = Config::get_instance();
    return config->get<int>("/label_printer/port", 9100);
}

void LabelPrinterSettingsManager::set_printer_port(int port) {
    spdlog::info("[LabelPrinterSettings] set_printer_port({})", port);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/port", port);
    config->save();
}

int LabelPrinterSettingsManager::get_label_size_index() const {
    Config* config = Config::get_instance();
    return config->get<int>("/label_printer/label_size", 0);
}

void LabelPrinterSettingsManager::set_label_size_index(int index) {
    spdlog::info("[LabelPrinterSettings] set_label_size_index({})", index);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/label_size", index);
    config->save();
}

int LabelPrinterSettingsManager::get_label_preset() const {
    Config* config = Config::get_instance();
    return config->get<int>("/label_printer/preset", 0);
}

void LabelPrinterSettingsManager::set_label_preset(int preset) {
    spdlog::info("[LabelPrinterSettings] set_label_preset({})", preset);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/preset", preset);
    config->save();
}

uint16_t LabelPrinterSettingsManager::get_usb_vid() const {
    Config* config = Config::get_instance();
    return static_cast<uint16_t>(config->get<int>("/label_printer/usb_vid", 0));
}

void LabelPrinterSettingsManager::set_usb_vid(uint16_t vid) {
    spdlog::info("[LabelPrinterSettings] set_usb_vid(0x{:04x})", vid);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/usb_vid", vid);
    config->save();

    if (subjects_initialized_) {
        lv_subject_set_int(&printer_configured_subject_, is_configured() ? 1 : 0);
    }
}

uint16_t LabelPrinterSettingsManager::get_usb_pid() const {
    Config* config = Config::get_instance();
    return static_cast<uint16_t>(config->get<int>("/label_printer/usb_pid", 0));
}

void LabelPrinterSettingsManager::set_usb_pid(uint16_t pid) {
    spdlog::info("[LabelPrinterSettings] set_usb_pid(0x{:04x})", pid);

    Config* config = Config::get_instance();
    config->set<int>("/label_printer/usb_pid", pid);
    config->save();

    if (subjects_initialized_) {
        lv_subject_set_int(&printer_configured_subject_, is_configured() ? 1 : 0);
    }
}

std::string LabelPrinterSettingsManager::get_usb_serial() const {
    Config* config = Config::get_instance();
    return config->get<std::string>("/label_printer/usb_serial", "");
}

void LabelPrinterSettingsManager::set_usb_serial(const std::string& serial) {
    spdlog::info("[LabelPrinterSettings] set_usb_serial('{}')", serial);

    Config* config = Config::get_instance();
    config->set<std::string>("/label_printer/usb_serial", serial);
    config->save();
}

bool LabelPrinterSettingsManager::is_configured() const {
    if (get_printer_type() == "usb") {
        return get_usb_vid() != 0 && get_usb_pid() != 0;
    }
    return !get_printer_address().empty();
}
