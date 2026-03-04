// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "moonraker_client_mock_internal.h"

#include <algorithm>
#include <ctime>
#include <spdlog/spdlog.h>

namespace mock_internal {

// Stateful mock queue — jobs can be deleted, queue can be paused/started
static struct MockQueueState {
    struct Job {
        std::string job_id;
        std::string filename;
        double time_added;
    };
    std::vector<Job> jobs;
    std::string queue_state = "ready";
    bool initialized = false;

    void ensure_initialized() {
        if (initialized) return;
        double now = static_cast<double>(time(nullptr));
        jobs = {
            {"0001", "benchy_v2.gcode", now - 3600},
            {"0002", "calibration_cube.gcode", now - 1800},
            {"0003", "phone_stand.gcode", now - 300},
        };
        initialized = true;
    }
} s_mock_queue;

void register_queue_handlers(std::unordered_map<std::string, MethodHandler>& registry) {
    // server.job_queue.status — return current mock queue state
    registry["server.job_queue.status"] =
        []([[maybe_unused]] MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           std::function<void(const json&)> success_cb,
           [[maybe_unused]] std::function<void(const MoonrakerError&)> error_cb) -> bool {
        s_mock_queue.ensure_initialized();

        double now = static_cast<double>(time(nullptr));
        json result;
        result["queue_state"] = s_mock_queue.queue_state;
        json jobs_arr = json::array();
        for (const auto& job : s_mock_queue.jobs) {
            jobs_arr.push_back({{"job_id", job.job_id},
                                {"filename", job.filename},
                                {"time_added", job.time_added},
                                {"time_in_queue", now - job.time_added}});
        }
        result["queued_jobs"] = jobs_arr;

        spdlog::debug("[MoonrakerClientMock] Returning mock job queue: {} jobs ({})",
                      s_mock_queue.jobs.size(), s_mock_queue.queue_state);

        if (success_cb) {
            success_cb(json{{"result", result}});
        }
        return true;
    };

    // server.job_queue.start — start processing queue
    registry["server.job_queue.start"] =
        []([[maybe_unused]] MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           std::function<void(const json&)> success_cb,
           [[maybe_unused]] std::function<void(const MoonrakerError&)> error_cb) -> bool {
        s_mock_queue.queue_state = "ready";
        spdlog::info("[MoonrakerClientMock] Job queue started");
        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };

    // server.job_queue.pause — pause queue processing
    registry["server.job_queue.pause"] =
        []([[maybe_unused]] MoonrakerClientMock* self, [[maybe_unused]] const json& params,
           std::function<void(const json&)> success_cb,
           [[maybe_unused]] std::function<void(const MoonrakerError&)> error_cb) -> bool {
        s_mock_queue.queue_state = "paused";
        spdlog::info("[MoonrakerClientMock] Job queue paused");
        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };

    // server.job_queue.post_job — add job(s) to queue
    registry["server.job_queue.post_job"] =
        []([[maybe_unused]] MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           [[maybe_unused]] std::function<void(const MoonrakerError&)> error_cb) -> bool {
        s_mock_queue.ensure_initialized();
        if (params.contains("filenames")) {
            double now = static_cast<double>(time(nullptr));
            for (const auto& f : params["filenames"]) {
                std::string filename = f.get<std::string>();
                // Generate a simple sequential ID
                std::string id = std::to_string(s_mock_queue.jobs.size() + 1);
                s_mock_queue.jobs.push_back({id, filename, now});
                spdlog::info("[MoonrakerClientMock] Added job to queue: {}", filename);
            }
        }
        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };

    // server.job_queue.delete_job — remove job(s) from queue
    registry["server.job_queue.delete_job"] =
        []([[maybe_unused]] MoonrakerClientMock* self, const json& params,
           std::function<void(const json&)> success_cb,
           [[maybe_unused]] std::function<void(const MoonrakerError&)> error_cb) -> bool {
        s_mock_queue.ensure_initialized();
        if (params.contains("job_ids")) {
            for (const auto& id_val : params["job_ids"]) {
                std::string id = id_val.get<std::string>();
                auto it = std::remove_if(s_mock_queue.jobs.begin(), s_mock_queue.jobs.end(),
                                         [&id](const auto& j) { return j.job_id == id; });
                if (it != s_mock_queue.jobs.end()) {
                    spdlog::info("[MoonrakerClientMock] Removed job {} from queue", id);
                    s_mock_queue.jobs.erase(it, s_mock_queue.jobs.end());
                }
            }
        }
        if (success_cb) {
            success_cb(json::object());
        }
        return true;
    };
}

} // namespace mock_internal
