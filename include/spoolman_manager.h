// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "lvgl/lvgl.h"

#include <atomic>
#include <cstdint>
#include <mutex>

class MoonrakerAPI;

/**
 * @brief Centralized Spoolman weight polling and circuit breaker management
 *
 * Extracted from AmsState to decouple Spoolman from AMS hardware.
 * Spoolman works independently — printers without AMS can still use
 * Spoolman for filament tracking.
 *
 * Owns:
 * - Periodic weight polling via lv_timer (refcounted start/stop)
 * - Circuit breaker to suppress error toasts when Spoolman is unavailable
 * - Print state observer to auto-refresh weights on print start/end/pause
 * - Spoolman availability observer to auto-stop polling when Spoolman disappears
 */
class SpoolmanManager {
  public:
    static SpoolmanManager& instance();

    SpoolmanManager(const SpoolmanManager&) = delete;
    SpoolmanManager& operator=(const SpoolmanManager&) = delete;

    void init_subjects();
    void deinit_subjects();

    void set_api(MoonrakerAPI* api);

    void refresh_spoolman_weights();
    void start_spoolman_polling();
    void stop_spoolman_polling();

  private:
    friend class SpoolmanManagerTestAccess;

    SpoolmanManager() = default;
    ~SpoolmanManager();

    static std::atomic<bool> s_shutdown_flag;

    mutable std::recursive_mutex mutex_;
    MoonrakerAPI* api_ = nullptr;
    bool initialized_ = false;

    // Polling
    lv_timer_t* poll_timer_ = nullptr;
    int poll_refcount_ = 0;

    // Circuit breaker / debounce
    static constexpr int CB_FAILURE_THRESHOLD = 3;
    static constexpr uint32_t CB_BACKOFF_MS = 30000;
    static constexpr uint32_t DEBOUNCE_MS = 5000;
    static constexpr uint32_t POLL_INTERVAL_MS = 30000;

    uint32_t last_refresh_ms_ = 0;
    int consecutive_failures_ = 0;
    uint32_t cb_tripped_at_ms_ = 0;
    bool cb_open_ = false;
    bool unavailable_notified_ = false;

    void reset_circuit_breaker();

    // Observers
    ObserverGuard print_state_observer_;
    ObserverGuard spoolman_availability_observer_;
};
