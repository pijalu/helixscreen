// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file power_device_state.cpp
 * @brief Power device state tracking from Moonraker WebSocket events
 *
 * Maintains per-device LVGL subjects with values:
 *   0 = off, 1 = on, 2 = locked (device is on but locked_while_printing during a print)
 */

#include "power_device_state.h"

#include "ui_update_queue.h"

#include "app_globals.h"
#include "moonraker_api.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_subject_registry.h"

#include <spdlog/spdlog.h>

namespace helix {

PowerDeviceState& PowerDeviceState::instance() {
    static PowerDeviceState s;
    return s;
}

int PowerDeviceState::status_string_to_int(const std::string& status) {
    if (status == "on") {
        return 1;
    }
    return 0; // "off", "error", or unknown -> off
}

void PowerDeviceState::set_devices(const std::vector<PowerDevice>& devices) {
    // Tear down existing subjects (expire lifetime tokens first)
    if (subjects_initialized_) {
        // Destroy lifetime tokens FIRST to expire weak_ptrs in ObserverGuards
        for (auto& [name, info] : devices_) {
            info.lifetime.reset();
        }
        for (auto& [name, info] : devices_) {
            if (info.status_subject) {
                lv_subject_deinit(info.status_subject.get());
            }
        }
        devices_.clear();
    }

    // Create new device entries with subjects
    for (const auto& dev : devices) {
        DeviceInfo info;
        info.name = dev.device;
        info.type = dev.type;
        info.locked_while_printing = dev.locked_while_printing;
        info.raw_status = status_string_to_int(dev.status);

        info.status_subject = std::make_unique<lv_subject_t>();
        lv_subject_init_int(info.status_subject.get(), info.raw_status);
        info.lifetime = std::make_shared<bool>(true);

        spdlog::trace(
            "[PowerDeviceState] Created subject for '{}': status={} locked_while_printing={}",
            dev.device, dev.status, dev.locked_while_printing);

        devices_.emplace(dev.device, std::move(info));
    }

    // Register with StaticSubjectRegistry on first init
    if (!subjects_initialized_) {
        subjects_initialized_ = true;
        StaticSubjectRegistry::instance().register_deinit(
            "PowerDeviceState", []() { PowerDeviceState::instance().deinit_subjects(); });

        // Observe print state to reevaluate lock states
        auto* print_subj = get_printer_state().get_print_state_enum_subject();
        if (print_subj) {
            print_state_observer_ = ui::observe_int_sync<PowerDeviceState>(
                print_subj, this,
                [](PowerDeviceState* self, int /*state*/) { self->reevaluate_lock_states(); });
        }
    }

    // Evaluate lock states with current print state
    reevaluate_lock_states();

    spdlog::info("[PowerDeviceState] Initialized {} power devices", devices.size());
}

lv_subject_t* PowerDeviceState::get_status_subject(const std::string& device, SubjectLifetime& lt) {
    auto it = devices_.find(device);
    if (it != devices_.end() && it->second.status_subject) {
        lt = it->second.lifetime;
        return it->second.status_subject.get();
    }
    lt.reset();
    return nullptr;
}

bool PowerDeviceState::is_locked_while_printing(const std::string& device) const {
    auto it = devices_.find(device);
    if (it != devices_.end()) {
        return it->second.locked_while_printing;
    }
    return false;
}

std::vector<std::string> PowerDeviceState::device_names() const {
    std::vector<std::string> names;
    names.reserve(devices_.size());
    for (const auto& [name, info] : devices_) {
        names.push_back(name);
    }
    return names;
}

void PowerDeviceState::subscribe(MoonrakerAPI& api) {
    api.register_method_callback("notify_power_changed", "power_device_state",
                                 [this](const nlohmann::json& msg) { on_power_changed(msg); });
    spdlog::debug("[PowerDeviceState] Subscribed to notify_power_changed");
}

void PowerDeviceState::unsubscribe(MoonrakerAPI& api) {
    api.unregister_method_callback("notify_power_changed", "power_device_state");
    print_state_observer_.reset();
    deinit_subjects();
    spdlog::debug("[PowerDeviceState] Unsubscribed and cleaned up");
}

void PowerDeviceState::on_power_changed(const nlohmann::json& msg) {
    // Moonraker sends: {"method": "notify_power_changed", "params": [{"device": "...", "status":
    // "on|off", ...}]}
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty()) {
        return;
    }

    for (const auto& param : msg["params"]) {
        if (!param.is_object() || !param.contains("device") || !param.contains("status")) {
            continue;
        }

        std::string device_name = param["device"].get<std::string>();
        std::string new_status = param["status"].get<std::string>();
        int new_raw = status_string_to_int(new_status);

        spdlog::debug("[PowerDeviceState] notify_power_changed: device='{}' status='{}'",
                      device_name, new_status);

        // Marshal to UI thread for subject updates
        ui::queue_update([this, device_name, new_raw]() {
            auto it = devices_.find(device_name);
            if (it == devices_.end()) {
                spdlog::trace("[PowerDeviceState] Ignoring update for unknown device '{}'",
                              device_name);
                return;
            }

            it->second.raw_status = new_raw;

            // Evaluate effective status (may be locked)
            int effective = new_raw;
            if (it->second.locked_while_printing && new_raw == 1) {
                auto state = static_cast<PrintJobState>(
                    lv_subject_get_int(get_printer_state().get_print_state_enum_subject()));
                if (state == PrintJobState::PRINTING || state == PrintJobState::PAUSED) {
                    effective = 2; // locked
                }
            }

            if (lv_subject_get_int(it->second.status_subject.get()) != effective) {
                lv_subject_set_int(it->second.status_subject.get(), effective);
                spdlog::debug("[PowerDeviceState] Updated '{}' subject to {}", device_name,
                              effective);
            }
        });
    }
}

void PowerDeviceState::reevaluate_lock_states() {
    auto* print_subj = get_printer_state().get_print_state_enum_subject();
    if (!print_subj) {
        return;
    }

    auto state = static_cast<PrintJobState>(lv_subject_get_int(print_subj));
    bool is_printing = (state == PrintJobState::PRINTING || state == PrintJobState::PAUSED);

    for (auto& [name, info] : devices_) {
        if (!info.status_subject) {
            continue;
        }

        int effective = info.raw_status;
        if (info.locked_while_printing && info.raw_status == 1 && is_printing) {
            effective = 2; // locked
        }

        if (lv_subject_get_int(info.status_subject.get()) != effective) {
            lv_subject_set_int(info.status_subject.get(), effective);
            spdlog::debug("[PowerDeviceState] Reevaluated '{}': effective={}", name, effective);
        }
    }
}

void PowerDeviceState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PowerDeviceState] Deinitializing subjects");

    print_state_observer_.reset();

    // Destroy lifetime tokens FIRST to expire weak_ptrs in ObserverGuards
    for (auto& [name, info] : devices_) {
        info.lifetime.reset();
    }

    // Now safe to deinit subjects
    for (auto& [name, info] : devices_) {
        if (info.status_subject) {
            lv_subject_deinit(info.status_subject.get());
        }
    }
    devices_.clear();

    subjects_initialized_ = false;
}

} // namespace helix
