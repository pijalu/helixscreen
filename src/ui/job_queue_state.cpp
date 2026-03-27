// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "job_queue_state.h"

#include "moonraker_api.h"
#include "moonraker_client.h"
#include "static_subject_registry.h"
#include "subject_debug_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

JobQueueState::JobQueueState(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : api_(api), client_(client) {
    std::memset(state_buffer_, 0, sizeof(state_buffer_));
    std::memset(summary_buffer_, 0, sizeof(summary_buffer_));

    subscribe_to_notifications();
    spdlog::debug("[JobQueueState] Created");
}

JobQueueState::~JobQueueState() {
    // lifetime_ destructor calls invalidate() automatically

    if (client_) {
        client_->unregister_method_callback("notify_job_queue_changed", "JobQueueState");
    }

    spdlog::debug("[JobQueueState] Destroyed");
}

void JobQueueState::init_subjects() {
    if (subjects_initialized_) return;

    lv_subject_init_int(&job_queue_count_subject_, 0);
    lv_xml_register_subject(nullptr, "job_queue_count", &job_queue_count_subject_);

    lv_subject_init_string(&job_queue_state_subject_, state_buffer_, nullptr, sizeof(state_buffer_),
                           "Ready");
    lv_xml_register_subject(nullptr, "job_queue_state_text", &job_queue_state_subject_);

    lv_subject_init_string(&job_queue_summary_subject_, summary_buffer_, nullptr,
                           sizeof(summary_buffer_), "Queue empty");
    lv_xml_register_subject(nullptr, "job_queue_summary_text", &job_queue_summary_subject_);

    // Register with debug registry for diagnostics
    SubjectDebugRegistry::instance().register_subject(&job_queue_count_subject_, "job_queue_count",
                                                      LV_SUBJECT_TYPE_INT, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&job_queue_state_subject_,
                                                      "job_queue_state_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);
    SubjectDebugRegistry::instance().register_subject(&job_queue_summary_subject_,
                                                      "job_queue_summary_text",
                                                      LV_SUBJECT_TYPE_STRING, __FILE__, __LINE__);

    subjects_initialized_ = true;

    // Co-locate cleanup registration with init (CLAUDE.md mandate)
    StaticSubjectRegistry::instance().register_deinit("JobQueueState",
                                                      [this]() { deinit_subjects(); });

    spdlog::debug("[JobQueueState] Subjects initialized");
}

void JobQueueState::deinit_subjects() {
    if (!subjects_initialized_) return;

    lv_subject_deinit(&job_queue_summary_subject_);
    lv_subject_deinit(&job_queue_state_subject_);
    lv_subject_deinit(&job_queue_count_subject_);

    subjects_initialized_ = false;
    spdlog::debug("[JobQueueState] Subjects deinitialized");
}

void JobQueueState::fetch() {
    if (is_fetching_ || !api_) return;
    is_fetching_ = true;

    auto token = lifetime_.token();
    api_->queue().get_queue_status(
        [this, token](const JobQueueStatus& status) {
            if (token.expired()) return;
            on_queue_fetched(status);
        },
        [this, token](const MoonrakerError& err) {
            if (token.expired()) return;
            lifetime_.defer("JobQueueState::fetch_error", [this, msg = err.message]() {
                is_fetching_ = false;
                spdlog::warn("[JobQueueState] Fetch failed: {}", msg);
            });
        });
}

void JobQueueState::on_queue_fetched(const JobQueueStatus& status) {
    // Thread safety: API callbacks may fire on background thread.
    // Use queue_update to marshal onto the LVGL main thread.
    // Guard must be captured into the lambda to prevent use-after-free
    // if JobQueueState is destroyed before the queued update executes.
    lifetime_.defer("JobQueueState::on_queue_fetched", [this, status]() {
        cached_jobs_ = status.queued_jobs;
        queue_state_ = status.queue_state;
        is_loaded_ = true;
        is_fetching_ = false;
        update_subjects();
        spdlog::debug("[JobQueueState] Updated: state={}, jobs={}", queue_state_,
                      cached_jobs_.size());
    });
}

void JobQueueState::update_subjects() {
    if (!subjects_initialized_) return;

    int count = static_cast<int>(cached_jobs_.size());
    lv_subject_set_int(&job_queue_count_subject_, count);

    // State text: capitalize first letter for display
    std::string state_display = queue_state_;
    if (!state_display.empty()) {
        state_display[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(state_display[0])));
    }
    std::snprintf(state_buffer_, sizeof(state_buffer_), "%s", state_display.c_str());
    lv_subject_copy_string(&job_queue_state_subject_, state_buffer_);

    // Summary text
    if (count == 0) {
        std::snprintf(summary_buffer_, sizeof(summary_buffer_), "Queue empty");
    } else if (count == 1) {
        std::snprintf(summary_buffer_, sizeof(summary_buffer_), "1 job queued");
    } else {
        std::snprintf(summary_buffer_, sizeof(summary_buffer_), "%d jobs queued", count);
    }
    lv_subject_copy_string(&job_queue_summary_subject_, summary_buffer_);
}

void JobQueueState::subscribe_to_notifications() {
    if (!client_) return;

    auto token = lifetime_.token();
    client_->register_method_callback(
        "notify_job_queue_changed", "JobQueueState", [this, token](const nlohmann::json& /*data*/) {
            if (token.expired()) return;
            // Re-fetch full queue status on any change notification
            fetch();
        });

    spdlog::debug("[JobQueueState] Subscribed to notify_job_queue_changed");
}
