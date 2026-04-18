// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include <cstdint>

namespace helix {

class FilamentConsumptionTracker {
  public:
    static FilamentConsumptionTracker& instance();

    /// Register lifecycle + filament-used observers. Call once during app init
    /// after AmsState and PrinterState subjects are available.
    void start();

    /// Tear down observers. Safe to call multiple times.
    void stop();

    /// True while a snapshot is live (print in progress, spool assigned,
    /// density resolved).
    [[nodiscard]] bool is_active() const {
        return active_;
    }

    /// Test-only: override the 60s persist interval. Pass 0 to restore default.
    void set_persist_interval_for_test(uint32_t ms) {
        persist_interval_override_ms_ = ms;
    }

  private:
    FilamentConsumptionTracker() = default;
    ~FilamentConsumptionTracker() = default;
    FilamentConsumptionTracker(const FilamentConsumptionTracker&) = delete;
    FilamentConsumptionTracker& operator=(const FilamentConsumptionTracker&) = delete;

    // --- Snapshot captured at print start (or on re-snapshot) ---
    float snapshot_mm_ = 0.0f;
    float snapshot_weight_g_ = 0.0f;
    float diameter_mm_ = 1.75f;
    float density_g_cm3_ = 0.0f;

    /// Last weight this tracker wrote. Used for external-write detection
    /// (Task 7 re-snapshots when another writer changes remaining_weight_g).
    float last_written_weight_g_ = 0.0f;

    bool active_ = false;

    uint32_t last_persist_tick_ms_ = 0;
    static constexpr uint32_t kPersistIntervalMs = 60'000;

    uint32_t persist_interval_override_ms_ = 0;
    [[nodiscard]] uint32_t persist_interval_ms() const {
        return persist_interval_override_ms_ ? persist_interval_override_ms_ : kPersistIntervalMs;
    }

    ObserverGuard print_state_obs_;
    ObserverGuard filament_used_obs_;

    void on_print_state_changed(int job_state);
    void on_filament_used_changed(int filament_mm);
    void snapshot();
    void persist();
};

} // namespace helix
