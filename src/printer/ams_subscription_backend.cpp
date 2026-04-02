// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_subscription_backend.h"

#include "hv/json.hpp"
#include "moonraker_error.h"

AmsSubscriptionBackend::AmsSubscriptionBackend(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : api_(api), client_(client) {
    // Common defaults -- derived constructors set type-specific fields
    system_info_.version = "unknown";
    system_info_.current_tool = -1;
    system_info_.current_slot = -1;
    system_info_.filament_loaded = false;
    system_info_.action = AmsAction::IDLE;
    system_info_.total_slots = 0;
}

AmsSubscriptionBackend::~AmsSubscriptionBackend() {
    // Release without unsubscribe -- MoonrakerClient may already be destroyed
    subscription_.release();
}

AmsError AmsSubscriptionBackend::start() {
    bool should_emit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (running_) {
            return AmsErrorHelper::success();
        }

        if (!client_) {
            spdlog::error("{} Cannot start: MoonrakerClient is null", backend_log_tag());
            return AmsErrorHelper::not_connected("MoonrakerClient not provided");
        }

        if (!api_) {
            spdlog::error("{} Cannot start: MoonrakerAPI is null", backend_log_tag());
            return AmsErrorHelper::not_connected("MoonrakerAPI not provided");
        }

        // Derived class extra checks (e.g., ToolChanger requires tools discovered)
        auto extra_check = additional_start_checks();
        if (!extra_check.success()) {
            return extra_check;
        }

        helix::SubscriptionId id = client_->register_notify_update(
            [this, token = lifetime_.token()](const nlohmann::json& notification) {
                if (token.expired()) return;  // Backend destroyed, skip (#621)
                handle_status_update(notification);
            });

        if (id == helix::INVALID_SUBSCRIPTION_ID) {
            spdlog::error("{} Failed to register for status updates", backend_log_tag());
            return AmsErrorHelper::not_connected("Failed to subscribe to Moonraker updates");
        }

        subscription_ = SubscriptionGuard(client_, id);
        running_ = true;
        spdlog::info("{} Backend started, subscription ID: {}", backend_log_tag(), id);
        should_emit = true;
    }

    // Emit initial state event OUTSIDE the lock to avoid deadlock
    if (should_emit) {
        emit_event(EVENT_STATE_CHANGED);
    }

    // Derived class post-start work (version detection, config loading, etc.)
    on_started();

    return AmsErrorHelper::success();
}

void AmsSubscriptionBackend::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }
    on_stopping();
    subscription_.reset();
    running_ = false;
    spdlog::info("{} Backend stopped", backend_log_tag());
}

void AmsSubscriptionBackend::release_subscriptions() {
    subscription_.release();
}

bool AmsSubscriptionBackend::is_running() const {
    return running_;
}

void AmsSubscriptionBackend::set_event_callback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}

void AmsSubscriptionBackend::emit_event(const std::string& event, const std::string& data) {
    EventCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = event_callback_;
    }
    if (cb) {
        cb(event, data);
    }
}

AmsAction AmsSubscriptionBackend::get_current_action() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.action;
}

int AmsSubscriptionBackend::get_current_tool() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_tool;
}

int AmsSubscriptionBackend::get_current_slot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.current_slot;
}

bool AmsSubscriptionBackend::is_filament_loaded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_info_.filament_loaded;
}

AmsError AmsSubscriptionBackend::check_preconditions() const {
    if (!running_) {
        return AmsErrorHelper::not_connected(std::string(backend_log_tag()) +
                                             " backend not started");
    }
    if (system_info_.is_busy()) {
        return AmsErrorHelper::busy(ams_action_to_string(system_info_.action));
    }
    return AmsErrorHelper::success();
}

AmsError AmsSubscriptionBackend::ensure_homed_then(std::string gcode) {
    if (!client_) {
        spdlog::debug("{} No client for homing query, executing directly", backend_log_tag());
        return execute_gcode(gcode);
    }

    auto token = lifetime_.token();
    auto gcode_copy = std::move(gcode);
    client_->send_jsonrpc(
        "printer.objects.query",
        json{{"objects", json{{"toolhead", json::array({"homed_axes"})}}}},
        [this, token, gcode_copy](const json& response) {
            if (token.expired()) return;

            bool needs_home = true;
            if (response.contains("result") && response["result"].contains("status")) {
                const auto& status = response["result"]["status"];
                if (status.contains("toolhead") &&
                    status["toolhead"].contains("homed_axes") &&
                    status["toolhead"]["homed_axes"].is_string()) {
                    std::string axes = status["toolhead"]["homed_axes"].get<std::string>();
                    needs_home = (axes.find("xyz") == std::string::npos);
                }
            }

            if (needs_home) {
                spdlog::info("{} Not homed, sending G28 before operation", backend_log_tag());
                api_->execute_gcode(
                    "G28",
                    [this, token, gcode_copy]() {
                        if (token.expired()) return;
                        spdlog::info("{} Homing complete, proceeding with: {}",
                                     backend_log_tag(), gcode_copy);
                        execute_gcode(gcode_copy);
                    },
                    [this, token](const MoonrakerError& err) {
                        if (token.expired()) return;
                        spdlog::error("{} Homing failed: {}", backend_log_tag(), err.message);
                        std::lock_guard<std::mutex> lock(mutex_);
                        system_info_.action = AmsAction::IDLE;
                    },
                    MoonrakerAPI::HOMING_TIMEOUT_MS);
            } else {
                execute_gcode(gcode_copy);
            }
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired()) return;
            spdlog::error("{} Homed axes query failed: {}", backend_log_tag(), err.message);
            std::lock_guard<std::mutex> lock(mutex_);
            system_info_.action = AmsAction::IDLE;
        });

    return AmsErrorHelper::success();
}

AmsError AmsSubscriptionBackend::execute_gcode(const std::string& gcode) {
    if (!api_) {
        return AmsErrorHelper::not_connected("MoonrakerAPI not available");
    }
    const char* tag = backend_log_tag();
    spdlog::info("{} Executing G-code: {}", tag, gcode);
    api_->execute_gcode(
        gcode, [tag]() { spdlog::debug("{} G-code executed successfully", tag); },
        [tag, gcode](const MoonrakerError& err) {
            if (err.type == MoonrakerErrorType::TIMEOUT) {
                spdlog::warn("{} G-code response timed out (may still be running): {}", tag, gcode);
            } else {
                spdlog::error("{} G-code failed: {} - {}", tag, gcode, err.message);
            }
        },
        MoonrakerAPI::AMS_OPERATION_TIMEOUT_MS);
    return AmsErrorHelper::success();
}
