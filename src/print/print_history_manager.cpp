// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "print_history_manager.h"

#include "ui_update_queue.h"

#include "moonraker_api.h"
#include "moonraker_client.h"

#include <spdlog/spdlog.h>

#include <algorithm>

using namespace helix;

// ============================================================================
// Construction / Destruction
// ============================================================================

PrintHistoryManager::PrintHistoryManager(MoonrakerAPI* api, MoonrakerClient* client)
    : api_(api), client_(client) {
    spdlog::debug("[HistoryManager] Created");
    subscribe_to_notifications();
}

PrintHistoryManager::~PrintHistoryManager() {
    // Unregister notification callback
    if (client_) {
        client_->unregister_method_callback("notify_history_changed", "PrintHistoryManager");
    }
}

// ============================================================================
// Fetch / Refresh
// ============================================================================

void PrintHistoryManager::fetch(int page_size) {
    if (is_fetching_) {
        spdlog::debug("[HistoryManager] Fetch already in progress, ignoring");
        return;
    }

    if (!api_) {
        spdlog::warn("[HistoryManager] No API available, cannot fetch");
        return;
    }

    is_fetching_ = true;
    spdlog::debug("[HistoryManager] Fetching all history (page_size={})", page_size);

    // Paginate to fetch ALL jobs, not just the first page
    auto accumulated = std::make_shared<std::vector<PrintHistoryJob>>();
    fetch_page(accumulated, page_size, 0);
}

void PrintHistoryManager::fetch_page(std::shared_ptr<std::vector<PrintHistoryJob>> accumulated,
                                     int page_size, int offset) {
    auto token = lifetime_.token();

    api_->history().get_history_list(
        page_size, offset, 0.0, 0.0,
        [this, token, accumulated, page_size](const std::vector<PrintHistoryJob>& jobs,
                                              uint64_t total) {
            if (token.expired()) return;

            // Accumulate this page
            accumulated->insert(accumulated->end(), jobs.begin(), jobs.end());

            if (!jobs.empty() && accumulated->size() < total) {
                // More pages remain — fetch next
                int next_offset = static_cast<int>(accumulated->size());
                spdlog::debug("[HistoryManager] Fetched page ({}/{} jobs), fetching more",
                              accumulated->size(), total);

                lifetime_.defer("PrintHistoryManager::fetch_next_page",
                                [this, accumulated, page_size, next_offset]() {
                                    fetch_page(accumulated, page_size, next_offset);
                                });
            } else {
                // All pages loaded
                spdlog::debug("[HistoryManager] All pages fetched ({} jobs total)",
                              accumulated->size());

                lifetime_.defer("PrintHistoryManager::fetch_complete",
                                [this, accumulated]() mutable {
                                    on_history_fetched(std::move(*accumulated));
                                });
            }
        },
        [this, token](const MoonrakerError& error) {
            if (token.expired()) return;
            spdlog::warn("[HistoryManager] Failed to fetch history: {}", error.message);
            lifetime_.defer("PrintHistoryManager::fetch_error", [this]() {
                is_fetching_ = false;
            });
        });
}

void PrintHistoryManager::invalidate() {
    spdlog::debug("[HistoryManager] Cache invalidated");
    is_loaded_ = false;
}

// ============================================================================
// Observer Pattern
// ============================================================================

void PrintHistoryManager::add_observer(HistoryChangedCallback* cb) {
    if (cb && *cb) {
        observers_.push_back(cb);
        spdlog::debug("[HistoryManager] Added observer (total: {})", observers_.size());
    }
}

void PrintHistoryManager::remove_observer(HistoryChangedCallback* cb) {
    if (!cb) {
        return;
    }

    auto it = std::find(observers_.begin(), observers_.end(), cb);
    if (it != observers_.end()) {
        observers_.erase(it);
        spdlog::debug("[HistoryManager] Removed observer (remaining: {})", observers_.size());
    }
}

// ============================================================================
// Private Implementation
// ============================================================================

void PrintHistoryManager::on_history_fetched(std::vector<PrintHistoryJob>&& jobs) {
    spdlog::debug("[HistoryManager] Fetched {} jobs", jobs.size());

    cached_jobs_ = std::move(jobs);
    build_filename_stats();

    is_loaded_ = true;
    is_fetching_ = false;

    notify_observers();
}

void PrintHistoryManager::build_filename_stats() {
    filename_stats_.clear();

    for (const auto& job : cached_jobs_) {
        // Strip path from filename to get basename
        std::string basename = job.filename;
        auto slash_pos = basename.rfind('/');
        if (slash_pos != std::string::npos) {
            basename = basename.substr(slash_pos + 1);
        }

        if (basename.empty()) {
            continue;
        }

        auto& stats = filename_stats_[basename];

        // Count successes and failures
        if (job.status == PrintJobStatus::COMPLETED) {
            stats.success_count++;
        } else if (job.status == PrintJobStatus::CANCELLED || job.status == PrintJobStatus::ERROR) {
            stats.failure_count++;
        }

        // Track most recent job for this filename
        if (job.start_time > stats.last_print_time) {
            stats.last_print_time = job.start_time;
            stats.last_status = job.status;
            stats.uuid = job.uuid;
            stats.size_bytes = job.size_bytes;
        }
    }

    spdlog::debug("[HistoryManager] Built stats for {} unique filenames", filename_stats_.size());
}

std::vector<PrintHistoryJob> PrintHistoryManager::get_jobs_since(double since) const {
    std::vector<PrintHistoryJob> filtered;
    filtered.reserve(cached_jobs_.size()); // Avoid reallocation

    for (const auto& job : cached_jobs_) {
        if (job.start_time >= since) {
            filtered.push_back(job);
        }
    }

    return filtered;
}

void PrintHistoryManager::notify_observers() {
    auto observers_copy = observers_;
    for (auto* cb : observers_copy) {
        if (cb && *cb) {
            (*cb)();
        }
    }
}

void PrintHistoryManager::subscribe_to_notifications() {
    if (!client_) {
        return;
    }

    auto token = lifetime_.token();

    client_->register_method_callback("notify_history_changed", "PrintHistoryManager",
                                      [this, token](const nlohmann::json& /*data*/) {
                                          if (token.expired()) return;
                                          spdlog::debug(
                                              "[HistoryManager] Received notify_history_changed");

                                          lifetime_.defer(
                                              "PrintHistoryManager::notify_history_changed",
                                              [this]() {
                                                  invalidate();
                                                  fetch();
                                              });
                                      });
}
