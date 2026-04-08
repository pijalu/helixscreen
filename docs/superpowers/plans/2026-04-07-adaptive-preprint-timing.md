# Adaptive Pre-Print Timing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the empirical pre-print timing system with a physics-based heating model shared with PID calibration, improved operational learning, a detail view time estimate, smoother live progress, and integrated finish time prediction.

**Architecture:** Extract the EMA heating rate logic from `PidProgressTracker` into a shared `ThermalRateModel` class. The pre-print predictor queries `ThermalRateModel` for heating estimates (degrees x rate) and keeps improved empirical learning for non-heating operations. A new estimate subject on `PrintPreparationManager` surfaces prep time on the detail view. The print start collector derives phase weights from predicted durations for proportional progress. Finish time integrates prep time during the pre-print phase.

**Tech Stack:** C++17, LVGL 9.5 subjects/observers, Config (JSON persistence via `settings.json`), Catch2 tests

**Key docs:** `docs/superpowers/specs/2026-04-07-adaptive-preprint-timing-design.md`, `docs/devel/ARCHITECTURE.md`

---

### Task 1: Create ThermalRateModel with EMA heating rate logic

Extract the core EMA heating rate algorithm from `PidProgressTracker` into a reusable class. This is the foundation everything else builds on.

**Files:**
- Create: `include/thermal_rate_model.h`
- Create: `src/temperature/thermal_rate_model.cpp`
- Create: `tests/unit/test_thermal_rate_model.cpp`

- [ ] **Step 1: Write the failing test for EMA rate measurement**

In `tests/unit/test_thermal_rate_model.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../../include/thermal_rate_model.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("ThermalRateModel basic rate measurement", "[thermal_rate]") {
    ThermalRateModel model;
    model.reset(25.0f);

    // First few samples — insufficient delta, no rate yet
    model.record_sample(25.0f, 0);
    model.record_sample(26.0f, 1000);
    REQUIRE_FALSE(model.measured_rate().has_value());

    // Need >= 5°C total movement for first usable measurement
    model.record_sample(28.0f, 3000);
    REQUIRE_FALSE(model.measured_rate().has_value());

    // 30°C = 5°C above start, and >= 2°C from last sample — first measurement
    model.record_sample(31.0f, 5000);
    REQUIRE(model.measured_rate().has_value());

    // Rate should be roughly 5000ms / (31-25)°C = ~0.83 s/°C
    // (cumulative seed, not instantaneous)
    float rate = model.measured_rate().value();
    REQUIRE(rate > 0.5f);
    REQUIRE(rate < 1.2f);
}

TEST_CASE("ThermalRateModel EMA smoothing", "[thermal_rate]") {
    ThermalRateModel model;
    model.reset(25.0f);

    // Seed the rate with initial measurement
    model.record_sample(25.0f, 0);
    model.record_sample(31.0f, 5000);  // 5s for 6°C = 0.83 s/°C
    REQUIRE(model.measured_rate().has_value());
    float initial_rate = model.measured_rate().value();

    // Suddenly much faster heating (instantaneous: 2s for 3°C = 0.67 s/°C)
    model.record_sample(34.0f, 7000);
    float after_fast = model.measured_rate().value();

    // EMA (30% new, 70% old) should pull rate down slightly, not jump to 0.67
    REQUIRE(after_fast < initial_rate);
    REQUIRE(after_fast > 0.6f);  // Not fully at the instantaneous rate

    // Suddenly much slower heating (instantaneous: 5s for 2°C = 2.5 s/°C)
    model.record_sample(36.0f, 12000);
    float after_slow = model.measured_rate().value();

    // Should drift up, heavily damped
    REQUIRE(after_slow > after_fast);
    REQUIRE(after_slow < 2.0f);  // Not fully at the instantaneous rate
}

TEST_CASE("ThermalRateModel estimate_seconds", "[thermal_rate]") {
    ThermalRateModel model;
    model.reset(25.0f);

    // Seed: 10s for 10°C = 1.0 s/°C
    model.record_sample(25.0f, 0);
    model.record_sample(35.0f, 10000);

    // Estimate 100°C of heating at ~1.0 s/°C should be ~100s
    float estimate = model.estimate_seconds(100.0f, 200.0f);
    REQUIRE(estimate > 80.0f);
    REQUIRE(estimate < 120.0f);

    // Already at target — should be 0
    float at_target = model.estimate_seconds(200.0f, 200.0f);
    REQUIRE(at_target == Catch::Approx(0.0f));

    // Above target — should be 0
    float above = model.estimate_seconds(210.0f, 200.0f);
    REQUIRE(above == Catch::Approx(0.0f));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[thermal_rate]" -v`
Expected: Compilation error — `thermal_rate_model.h` does not exist yet.

- [ ] **Step 3: Write ThermalRateModel header**

In `include/thermal_rate_model.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>

/// Per-heater thermal rate model using exponential moving average.
/// Tracks heating rate in seconds-per-degree (s/°C).
/// Extracted from PidProgressTracker for reuse in pre-print timing.
class ThermalRateModel {
public:
    /// Record a live temperature sample. Updates internal EMA if sufficient
    /// delta from last sample (>= 2°C) and total movement (>= 5°C from start).
    void record_sample(float temp_c, uint32_t tick_ms);

    /// Estimate seconds to reach target_temp from current_temp using best
    /// available rate. Returns 0 if current >= target.
    float estimate_seconds(float current_temp, float target_temp) const;

    /// Get the live-measured rate, or nullopt if insufficient data.
    [[nodiscard]] std::optional<float> measured_rate() const;

    /// Best available rate: measured > history > default.
    [[nodiscard]] float best_rate() const;

    /// Load persisted rate from a previous session.
    void load_history(float heat_rate_s_per_deg);

    /// Get the history-blended rate for persistence.
    /// Blends 70% measured + 30% old history. Returns 0 if no measurement.
    [[nodiscard]] float blended_rate_for_save() const;

    /// Set the fallback default rate (used when no measurement or history).
    void set_default_rate(float rate_s_per_deg);

    /// Reset for a new tracking session.
    void reset(float start_temp);

private:
    static constexpr float MIN_DELTA_FROM_LAST = 2.0f;
    static constexpr float MIN_TOTAL_MOVEMENT = 5.0f;
    static constexpr float EMA_NEW_WEIGHT = 0.3f;
    static constexpr float EMA_OLD_WEIGHT = 0.7f;
    static constexpr float SAVE_NEW_WEIGHT = 0.7f;
    static constexpr float SAVE_OLD_WEIGHT = 0.3f;
    static constexpr float FALLBACK_DEFAULT_RATE = 0.5f;  // s/°C

    float measured_heat_rate_ = 0.0f;
    bool has_measured_heat_rate_ = false;
    float hist_heat_rate_ = 0.0f;
    bool has_history_ = false;
    float default_rate_ = FALLBACK_DEFAULT_RATE;
    float start_temp_ = 0.0f;
    float last_temp_ = 0.0f;
    uint32_t last_tick_ = 0;
    uint32_t start_tick_ = 0;
};
```

- [ ] **Step 4: Write ThermalRateModel implementation**

In `src/temperature/thermal_rate_model.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermal_rate_model.h"

#include <algorithm>
#include <cmath>

void ThermalRateModel::reset(float start_temp) {
    measured_heat_rate_ = 0.0f;
    has_measured_heat_rate_ = false;
    start_temp_ = start_temp;
    last_temp_ = start_temp;
    last_tick_ = 0;
    start_tick_ = 0;
}

void ThermalRateModel::record_sample(float temp_c, uint32_t tick_ms) {
    if (start_tick_ == 0) {
        start_tick_ = tick_ms;
    }

    float delta_from_last = temp_c - last_temp_;
    if (delta_from_last >= MIN_DELTA_FROM_LAST && tick_ms > last_tick_ && last_tick_ > 0) {
        float inst_rate =
            static_cast<float>(tick_ms - last_tick_) / 1000.0f / delta_from_last;
        if (has_measured_heat_rate_) {
            measured_heat_rate_ =
                EMA_NEW_WEIGHT * inst_rate + EMA_OLD_WEIGHT * measured_heat_rate_;
        } else if (temp_c - start_temp_ >= MIN_TOTAL_MOVEMENT) {
            float elapsed_s = static_cast<float>(tick_ms - start_tick_) / 1000.0f;
            measured_heat_rate_ = elapsed_s / (temp_c - start_temp_);
            has_measured_heat_rate_ = true;
        }
    }

    if (delta_from_last >= MIN_DELTA_FROM_LAST || last_tick_ == 0) {
        last_temp_ = temp_c;
        last_tick_ = tick_ms;
    }
}

std::optional<float> ThermalRateModel::measured_rate() const {
    if (has_measured_heat_rate_) {
        return measured_heat_rate_;
    }
    return std::nullopt;
}

float ThermalRateModel::best_rate() const {
    if (has_measured_heat_rate_) {
        return measured_heat_rate_;
    }
    if (has_history_ && hist_heat_rate_ > 0.0f) {
        return hist_heat_rate_;
    }
    return default_rate_;
}

float ThermalRateModel::estimate_seconds(float current_temp, float target_temp) const {
    if (current_temp >= target_temp) {
        return 0.0f;
    }
    return (target_temp - current_temp) * best_rate();
}

void ThermalRateModel::load_history(float heat_rate_s_per_deg) {
    if (heat_rate_s_per_deg > 0.0f) {
        hist_heat_rate_ = heat_rate_s_per_deg;
        has_history_ = true;
    }
}

float ThermalRateModel::blended_rate_for_save() const {
    if (!has_measured_heat_rate_) {
        return 0.0f;
    }
    if (has_history_ && hist_heat_rate_ > 0.0f) {
        return SAVE_NEW_WEIGHT * measured_heat_rate_ + SAVE_OLD_WEIGHT * hist_heat_rate_;
    }
    return measured_heat_rate_;
}

void ThermalRateModel::set_default_rate(float rate_s_per_deg) {
    if (rate_s_per_deg > 0.0f) {
        default_rate_ = rate_s_per_deg;
    }
}
```

- [ ] **Step 5: Add to build system**

Add `src/temperature/thermal_rate_model.cpp` to the source list in the Makefile. Check how existing `src/temperature/` files are included (grep for `temperature_history_manager` in the Makefile) and follow the same pattern.

Add `tests/unit/test_thermal_rate_model.cpp` to the test source list.

- [ ] **Step 6: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[thermal_rate]" -v`
Expected: All 3 test cases PASS.

- [ ] **Step 7: Write tests for history and default rate**

Append to `tests/unit/test_thermal_rate_model.cpp`:

```cpp
TEST_CASE("ThermalRateModel rate priority", "[thermal_rate]") {
    ThermalRateModel model;

    SECTION("default rate when no data") {
        model.set_default_rate(1.5f);
        REQUIRE(model.best_rate() == Catch::Approx(1.5f));
    }

    SECTION("history overrides default") {
        model.set_default_rate(1.5f);
        model.load_history(0.8f);
        REQUIRE(model.best_rate() == Catch::Approx(0.8f));
    }

    SECTION("measured overrides history") {
        model.set_default_rate(1.5f);
        model.load_history(0.8f);
        model.reset(25.0f);

        // Generate a live measurement
        model.record_sample(25.0f, 0);
        model.record_sample(35.0f, 5000);  // 0.5 s/°C
        REQUIRE(model.measured_rate().has_value());
        REQUIRE(model.best_rate() != Catch::Approx(0.8f));
    }
}

TEST_CASE("ThermalRateModel blended_rate_for_save", "[thermal_rate]") {
    ThermalRateModel model;
    model.load_history(1.0f);
    model.reset(25.0f);

    // Generate measurement: ~0.5 s/°C
    model.record_sample(25.0f, 0);
    model.record_sample(35.0f, 5000);

    float blended = model.blended_rate_for_save();
    float measured = model.measured_rate().value();
    // Blended = 70% measured + 30% history(1.0)
    float expected = 0.7f * measured + 0.3f * 1.0f;
    REQUIRE(blended == Catch::Approx(expected).epsilon(0.05));
}

TEST_CASE("ThermalRateModel no measurement returns 0 for save", "[thermal_rate]") {
    ThermalRateModel model;
    model.load_history(1.0f);
    // No record_sample calls
    REQUIRE(model.blended_rate_for_save() == Catch::Approx(0.0f));
}
```

- [ ] **Step 8: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[thermal_rate]" -v`
Expected: All 6 test cases PASS.

- [ ] **Step 9: Commit**

```bash
git add include/thermal_rate_model.h src/temperature/thermal_rate_model.cpp tests/unit/test_thermal_rate_model.cpp Makefile
git commit -m "feat: extract ThermalRateModel from PID progress tracker"
```

---

### Task 2: Create ThermalRateManager singleton with persistence and smart defaults

Wraps per-heater `ThermalRateModel` instances, handles Config persistence, and provides printer-archetype-aware defaults.

**Files:**
- Modify: `include/thermal_rate_model.h` (add ThermalRateManager class)
- Modify: `src/temperature/thermal_rate_model.cpp` (add ThermalRateManager implementation)
- Modify: `tests/unit/test_thermal_rate_model.cpp` (add manager tests)
- Read: `include/printer_detector.h` (for build volume / archetype info)

- [ ] **Step 1: Write the failing test for ThermalRateManager**

Append to `tests/unit/test_thermal_rate_model.cpp`:

```cpp
#include "../../include/config.h"

TEST_CASE("ThermalRateManager get_model returns per-heater instances", "[thermal_rate]") {
    ThermalRateManager manager;

    auto& extruder = manager.get_model("extruder");
    auto& bed = manager.get_model("heater_bed");

    // They should be different instances
    REQUIRE(&extruder != &bed);

    // Same name returns same instance
    auto& extruder2 = manager.get_model("extruder");
    REQUIRE(&extruder == &extruder2);
}

TEST_CASE("ThermalRateManager estimate_heating_seconds", "[thermal_rate]") {
    ThermalRateManager manager;

    // Set known defaults
    manager.get_model("extruder").set_default_rate(0.5f);
    manager.get_model("heater_bed").set_default_rate(1.5f);

    // Extruder: 175°C delta at 0.5 s/°C = 87.5s
    float ext_est = manager.estimate_heating_seconds("extruder", 25.0f, 200.0f);
    REQUIRE(ext_est == Catch::Approx(87.5f));

    // Bed: 75°C delta at 1.5 s/°C = 112.5s
    float bed_est = manager.estimate_heating_seconds("heater_bed", 25.0f, 100.0f);
    REQUIRE(bed_est == Catch::Approx(112.5f));

    // Already at temp
    REQUIRE(manager.estimate_heating_seconds("extruder", 200.0f, 200.0f) == Catch::Approx(0.0f));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[thermal_rate]" -v`
Expected: Compilation error — `ThermalRateManager` not defined.

- [ ] **Step 3: Add ThermalRateManager to header**

Add to `include/thermal_rate_model.h` after the `ThermalRateModel` class:

```cpp
#include <map>
#include <string>

class Config;

/// Singleton managing per-heater ThermalRateModel instances.
/// Handles persistence to Config and printer-archetype defaults.
class ThermalRateManager {
public:
    static ThermalRateManager& instance();

    /// Get or create model for a heater by name (e.g., "extruder", "heater_bed").
    ThermalRateModel& get_model(const std::string& heater_name);

    /// Estimate seconds for a heater to go from current to target temp.
    float estimate_heating_seconds(const std::string& heater_name, float current_temp,
                                   float target_temp) const;

    /// Load all persisted rates from config.
    void load_from_config(Config& config);

    /// Save all models with measurements to config.
    void save_to_config(Config& config);

    /// Apply smart defaults based on printer build volume.
    /// Call after printer detection. bed_x_max in mm (e.g., 235, 350).
    void apply_archetype_defaults(float bed_x_max);

    /// Reset all models (for testing).
    void reset();

private:
    ThermalRateManager() = default;
    std::map<std::string, ThermalRateModel> models_;
};
```

- [ ] **Step 4: Implement ThermalRateManager**

Add to `src/temperature/thermal_rate_model.cpp`:

```cpp
#include "config.h"
#include "spdlog/spdlog.h"

ThermalRateManager& ThermalRateManager::instance() {
    static ThermalRateManager inst;
    return inst;
}

ThermalRateModel& ThermalRateManager::get_model(const std::string& heater_name) {
    return models_[heater_name];
}

float ThermalRateManager::estimate_heating_seconds(const std::string& heater_name,
                                                    float current_temp,
                                                    float target_temp) const {
    auto it = models_.find(heater_name);
    if (it != models_.end()) {
        return it->second.estimate_seconds(current_temp, target_temp);
    }
    // Unknown heater — use generic default
    ThermalRateModel temp;
    return temp.estimate_seconds(current_temp, target_temp);
}

void ThermalRateManager::load_from_config(Config& config) {
    const std::string base = "/thermal/rates/";
    for (const auto& heater : {"extruder", "heater_bed"}) {
        std::string path = base + heater + "/heat_rate";
        float rate = config.get<float>(path, 0.0f);
        if (rate > 0.0f) {
            models_[heater].load_history(rate);
            spdlog::info("[ThermalRateManager] Loaded {} rate: {:.3f} s/°C", heater, rate);
        }
    }
}

void ThermalRateManager::save_to_config(Config& config) {
    const std::string base = "/thermal/rates/";
    for (auto& [name, model] : models_) {
        float blended = model.blended_rate_for_save();
        if (blended > 0.0f) {
            std::string path = base + name + "/heat_rate";
            config.set<float>(path, blended);
            spdlog::info("[ThermalRateManager] Saved {} rate: {:.3f} s/°C", name, blended);
        }
    }
    config.save();
}

void ThermalRateManager::apply_archetype_defaults(float bed_x_max) {
    // Extruder defaults — hotend type is hard to detect, use a moderate default.
    // Small enclosed printers (Bambu, K1) heat faster but we can't reliably
    // distinguish from build volume alone. 0.4 s/°C is a reasonable middle ground.
    models_["extruder"].set_default_rate(0.4f);

    // Bed defaults scale with surface area (proxy: max X dimension)
    float bed_rate;
    if (bed_x_max >= 350.0f) {
        bed_rate = 2.0f;   // Large bed (350mm+)
    } else if (bed_x_max >= 250.0f) {
        bed_rate = 1.5f;   // Medium bed (250-349mm)
    } else if (bed_x_max > 0.0f) {
        bed_rate = 1.2f;   // Small bed (<250mm)
    } else {
        bed_rate = 1.5f;   // Unknown — use medium default
    }
    models_["heater_bed"].set_default_rate(bed_rate);

    spdlog::info("[ThermalRateManager] Applied archetype defaults: bed_x_max={:.0f}mm, "
                 "extruder={:.2f} s/°C, bed={:.2f} s/°C",
                 bed_x_max, 0.4f, bed_rate);
}

void ThermalRateManager::reset() {
    models_.clear();
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[thermal_rate]" -v`
Expected: All test cases PASS.

- [ ] **Step 6: Commit**

```bash
git add include/thermal_rate_model.h src/temperature/thermal_rate_model.cpp tests/unit/test_thermal_rate_model.cpp
git commit -m "feat: add ThermalRateManager singleton with persistence and archetype defaults"
```

---

### Task 3: Refactor PidProgressTracker to use ThermalRateModel

Replace the inline EMA fields in `PidProgressTracker` with a `ThermalRateModel` instance. Oscillation tracking stays in PID. Behavior must be identical — existing PID tests must still pass.

**Files:**
- Modify: `include/pid_progress_tracker.h`
- Modify: `src/ui/pid_progress_tracker.cpp`
- Read: `tests/unit/test_pid_progress_tracker.cpp` (run existing tests to verify)

- [ ] **Step 1: Run existing PID tests to establish baseline**

Run: `make test && ./build/bin/helix-tests "[pid_progress]" -v`
Expected: All existing tests PASS. Record the test names and count.

- [ ] **Step 2: Modify PidProgressTracker header**

In `include/pid_progress_tracker.h`:

1. Add `#include "thermal_rate_model.h"` at the top.

2. Replace the private EMA fields (approximately lines 88-93):
```cpp
// REMOVE these fields:
//   float measured_heat_rate_ = 0.0f;
//   bool has_measured_heat_rate_ = false;
//   float hist_heat_rate_ = 0.0f;
//   bool has_history_ = false;

// ADD this:
ThermalRateModel thermal_model_;
```

3. Keep oscillation fields (`measured_cycle_period_`, `has_measured_cycle_`, `hist_oscillation_duration_`, oscillation constants) — those are PID-specific.

4. Change `best_heat_rate()` to delegate:
```cpp
float best_heat_rate() const { return thermal_model_.best_rate(); }
```

5. Change `set_history()` signature body — it now delegates the heat rate part:
```cpp
void set_history(float heat_rate_s_per_deg, float oscillation_duration_s);
```

6. Add accessor for the thermal model (so PID panel can save the rate):
```cpp
const ThermalRateModel& thermal_model() const { return thermal_model_; }
```

- [ ] **Step 3: Modify PidProgressTracker implementation**

In `src/ui/pid_progress_tracker.cpp`:

1. In `start()`: Replace EMA field resets with `thermal_model_.reset(current_temp)`. Also set default rate based on heater type:
```cpp
void PidProgressTracker::start(Heater heater, int target_temp, float current_temp) {
    // ... existing phase/target setup ...
    thermal_model_.reset(current_temp);
    thermal_model_.set_default_rate(
        heater == Heater::EXTRUDER ? DEFAULT_EXTRUDER_HEAT_RATE : DEFAULT_BED_HEAT_RATE);
    // ... rest of start() ...
}
```

2. In `on_temperature()` HEATING phase (lines ~44-56): Replace the inline EMA block with:
```cpp
thermal_model_.record_sample(temp, tick_ms);
```
Remove the `delta_from_last`, `inst_rate`, `has_measured_heat_rate_` logic entirely.

3. In `set_history()`: Delegate heat rate to thermal model:
```cpp
void PidProgressTracker::set_history(float heat_rate_s_per_deg, float osc_duration_s) {
    thermal_model_.load_history(heat_rate_s_per_deg);
    if (osc_duration_s > 0.0f) {
        hist_oscillation_duration_ = osc_duration_s;
        // has_history_ only needed for oscillation now
        has_history_ = true;
    }
}
```

4. In the PID calibration panel (`ui_panel_calibration_pid.cpp`), update the save logic to use `thermal_model().blended_rate_for_save()` instead of the raw `measured_heat_rate_` field. The save path changes from `/calibration/pid_history/{heater}/heat_rate` to `/thermal/rates/{heater}/heat_rate` (done in Task 6 migration, but the code change is here).

- [ ] **Step 4: Run existing PID tests to verify no regression**

Run: `make test && ./build/bin/helix-tests "[pid_progress]" -v`
Expected: Same tests PASS as baseline. Same count, same names.

- [ ] **Step 5: Commit**

```bash
git add include/pid_progress_tracker.h src/ui/pid_progress_tracker.cpp
git commit -m "refactor: PidProgressTracker delegates heating rate to ThermalRateModel"
```

---

### Task 4: Update PID calibration panel to save via ThermalRateManager

The PID calibration panel saves measured heating rates after calibration completes. Update it to save through `ThermalRateManager` at the new config path.

**Files:**
- Modify: `src/ui/ui_panel_calibration_pid.cpp` (save logic, ~lines 1120-1162)

- [ ] **Step 1: Update the save logic**

In `src/ui/ui_panel_calibration_pid.cpp`, find the block that saves heat rate to config after calibration completes (around lines 1120-1162). Replace the direct config writes with `ThermalRateManager`:

```cpp
// OLD:
// std::string heat_path = "/calibration/pid_history/" + heater_key + "/heat_rate";
// float old_heat = config->get<float>(heat_path, 0.0f);
// float new_heat = 0.7f * heat_rate + 0.3f * old_heat;
// config->set<float>(heat_path, new_heat);

// NEW:
auto& thermal_mgr = ThermalRateManager::instance();
auto& model = thermal_mgr.get_model(heater_key);
// The blended rate is already computed by ThermalRateModel
thermal_mgr.save_to_config(*config);
```

Add `#include "thermal_rate_model.h"` to the includes.

Keep the oscillation duration save logic unchanged — it stays at `/calibration/pid_history/{heater}/oscillation_duration` since oscillation is PID-specific.

- [ ] **Step 2: Update the load logic**

In the same file, find where history is loaded at calibration start (around lines 1110-1119). Update to load from `ThermalRateManager`:

```cpp
// Heat rate now comes from ThermalRateManager (already loaded from config)
auto& thermal_mgr = ThermalRateManager::instance();
float heat_rate = thermal_mgr.get_model(heater_key).best_rate();

// Oscillation duration still from old path
std::string osc_path = "/calibration/pid_history/" + std::string(heater_key) +
                        "/oscillation_duration";
float osc_dur = config->get<float>(osc_path, 0.0f);

progress_tracker_.set_history(heat_rate, osc_dur);
```

- [ ] **Step 3: Build and run PID tests**

Run: `make -j && make test && ./build/bin/helix-tests "[pid_progress]" -v`
Expected: Build succeeds. PID tests PASS.

- [ ] **Step 4: Commit**

```bash
git add src/ui/ui_panel_calibration_pid.cpp
git commit -m "refactor: PID calibration saves heat rate via ThermalRateManager"
```

---

### Task 5: Enhance PreprintPredictor with improved learning

Improve the non-heating phase learning: more entries, exponential time-decay weighting, MAD anomaly rejection, missing-phase normalization, simplified bucketing.

**Files:**
- Modify: `include/preprint_predictor.h`
- Modify: `src/print/preprint_predictor.cpp`
- Modify: `tests/unit/test_preprint_predictor.cpp`

- [ ] **Step 1: Write failing tests for improved predictor**

Add new test cases to `tests/unit/test_preprint_predictor.cpp`:

```cpp
TEST_CASE("PreprintPredictor time-decay weighting", "[preprint_predictor]") {
    PreprintPredictor predictor;

    // Create 5 entries with different QGL durations, newest last
    std::vector<PreprintEntry> entries;
    int64_t base_time = 1700000000;
    for (int i = 0; i < 5; i++) {
        PreprintEntry e;
        e.total_seconds = 60 + i * 10;  // 60, 70, 80, 90, 100
        e.timestamp = base_time + i * 3600;  // 1 hour apart
        e.phase_durations[static_cast<int>(PrintStartPhase::QGL)] = 60 + i * 10;
        entries.push_back(e);
    }

    predictor.load_entries(entries);
    auto phases = predictor.predicted_phases();
    int qgl_phase = static_cast<int>(PrintStartPhase::QGL);
    REQUIRE(phases.count(qgl_phase) == 1);

    // Newest entries (90, 100) should dominate over oldest (60, 70)
    // With time-decay, prediction should be closer to 100 than 60
    REQUIRE(phases[qgl_phase] > 80);
}

TEST_CASE("PreprintPredictor MAD anomaly rejection", "[preprint_predictor]") {
    PreprintPredictor predictor;

    int64_t base_time = 1700000000;
    std::vector<PreprintEntry> entries;
    int mesh_phase = static_cast<int>(PrintStartPhase::BED_MESH);

    // 4 normal entries: ~90s bed mesh
    for (int i = 0; i < 4; i++) {
        PreprintEntry e;
        e.total_seconds = 90;
        e.timestamp = base_time + i * 3600;
        e.phase_durations[mesh_phase] = 85 + i * 3;  // 85, 88, 91, 94
        entries.push_back(e);
    }

    // 1 anomalous entry: 800s bed mesh (stuck probe or something)
    PreprintEntry anomaly;
    anomaly.total_seconds = 800;
    anomaly.timestamp = base_time + 5 * 3600;
    anomaly.phase_durations[mesh_phase] = 800;
    entries.push_back(anomaly);

    predictor.load_entries(entries);
    auto phases = predictor.predicted_phases();

    // Anomaly should be rejected; prediction should be near 90, not inflated
    REQUIRE(phases[mesh_phase] < 120);
}

TEST_CASE("PreprintPredictor missing-phase normalization", "[preprint_predictor]") {
    PreprintPredictor predictor;

    int64_t base_time = 1700000000;
    int mesh_phase = static_cast<int>(PrintStartPhase::BED_MESH);
    int qgl_phase = static_cast<int>(PrintStartPhase::QGL);

    // Entry 1: has both mesh and QGL
    PreprintEntry e1;
    e1.total_seconds = 150;
    e1.timestamp = base_time;
    e1.phase_durations[mesh_phase] = 90;
    e1.phase_durations[qgl_phase] = 60;

    // Entry 2: has only QGL (no mesh this print)
    PreprintEntry e2;
    e2.total_seconds = 60;
    e2.timestamp = base_time + 3600;
    e2.phase_durations[qgl_phase] = 55;

    predictor.load_entries({e1, e2});
    auto phases = predictor.predicted_phases();

    // Mesh prediction should come from e1 only (the only entry that has mesh)
    REQUIRE(phases[mesh_phase] == 90);
    // QGL should average from both entries
    REQUIRE(phases[qgl_phase] > 50);
    REQUIRE(phases[qgl_phase] < 65);
}

TEST_CASE("PreprintPredictor accepts up to 10 entries", "[preprint_predictor]") {
    PreprintPredictor predictor;

    int64_t base_time = 1700000000;
    std::vector<PreprintEntry> entries;
    for (int i = 0; i < 12; i++) {
        PreprintEntry e;
        e.total_seconds = 100;
        e.timestamp = base_time + i * 3600;
        e.phase_durations[static_cast<int>(PrintStartPhase::HOMING)] = 20;
        entries.push_back(e);
    }

    predictor.load_entries(entries);
    // Should trim to 10, not 3
    REQUIRE(predictor.get_entries().size() == 10);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[preprint_predictor]" -v`
Expected: New tests FAIL (old MAX_ENTRIES=3 behavior, old weighting).

- [ ] **Step 3: Update PreprintPredictor header**

In `include/preprint_predictor.h`:

1. Change `MAX_ENTRIES` from 3 to 10:
```cpp
static constexpr int MAX_ENTRIES = 10;
```

2. Remove `MAX_TOTAL_SECONDS` constant (replaced by MAD-based rejection).

3. Change bucketing — remove temp_bucket from `load_entries` signature, add a simpler warm/cold distinction:
```cpp
enum class StartCondition { COLD, WARM };

void load_entries(const std::vector<PreprintEntry>& entries,
                  StartCondition condition = StartCondition::COLD);
```

- [ ] **Step 4: Update PreprintPredictor implementation**

In `src/print/preprint_predictor.cpp`:

1. Replace the fixed positional weights with exponential time-decay:

```cpp
std::vector<double> PreprintPredictor::compute_weights() const {
    if (entries_.empty()) return {};

    std::vector<double> weights(entries_.size());
    // Exponential decay: weight = exp(-lambda * age_index)
    // Lambda chosen so oldest of 10 entries has ~10% the weight of newest
    constexpr double lambda = 0.23;  // ln(10) / 10 ≈ 0.23

    double total = 0.0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        weights[i] = std::exp(lambda * static_cast<double>(i));  // newest last = highest
        total += weights[i];
    }
    // Normalize
    for (auto& w : weights) {
        w /= total;
    }
    return weights;
}
```

2. Replace `add_entry()` anomaly rejection with MAD:

```cpp
void PreprintPredictor::add_entry(const PreprintEntry& entry) {
    entries_.push_back(entry);

    // FIFO trim to MAX_ENTRIES
    while (entries_.size() > static_cast<size_t>(MAX_ENTRIES)) {
        entries_.erase(entries_.begin());
    }
}
```

Move anomaly rejection into `predicted_phases()` — reject per-phase outliers, not whole entries:

```cpp
// In predicted_phases(), for each phase:
// 1. Collect all durations for this phase across entries
// 2. Compute median
// 3. Compute MAD = median(|x_i - median|)
// 4. Reject entries where |duration - median| > 3 * MAD (if MAD > 0)
// 5. Weighted average of remaining entries
```

3. Update `load_entries()` for simplified bucketing:

```cpp
void PreprintPredictor::load_entries(const std::vector<PreprintEntry>& entries,
                                     StartCondition condition) {
    entries_.clear();
    for (const auto& e : entries) {
        // Warm/cold bucketing: bucket 0 (legacy) matches everything
        // New entries use 1=cold, 2=warm
        bool matches = (e.temp_bucket == 0) ||
                       (condition == StartCondition::COLD && e.temp_bucket == 1) ||
                       (condition == StartCondition::WARM && e.temp_bucket == 2);
        if (matches) {
            entries_.push_back(e);
        }
    }

    while (entries_.size() > static_cast<size_t>(MAX_ENTRIES)) {
        entries_.erase(entries_.begin());
    }
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[preprint_predictor]" -v`
Expected: All new and existing preprint_predictor tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/preprint_predictor.h src/print/preprint_predictor.cpp tests/unit/test_preprint_predictor.cpp
git commit -m "feat: enhanced PreprintPredictor with time-decay, MAD rejection, 10 entries"
```

---

### Task 6: Config migration v10 → v11

Migrate PID heat rates to the shared thermal path, strip heating phases from old predictor entries.

**Files:**
- Modify: `include/config.h` (bump version)
- Modify: `src/system/config.cpp` (add migration)
- Modify: `tests/unit/test_config.cpp` (add migration test)

- [ ] **Step 1: Write the failing migration test**

Add to `tests/unit/test_config.cpp`:

```cpp
TEST_CASE("Config migration v10 to v11", "[config][migration]") {
    // Create a v10 config with PID heat rate and predictor entries
    json config;
    config["config_version"] = 10;
    config["calibration"]["pid_history"]["extruder"]["heat_rate"] = 0.45f;
    config["calibration"]["pid_history"]["extruder"]["oscillation_duration"] = 110.0f;
    config["calibration"]["pid_history"]["heater_bed"]["heat_rate"] = 1.3f;
    config["calibration"]["pid_history"]["heater_bed"]["oscillation_duration"] = 340.0f;

    // Predictor entries with heating phases
    json entry1;
    entry1["total_seconds"] = 180;
    entry1["timestamp"] = 1700000000;
    entry1["temp_bucket"] = 200;
    entry1["phase_durations"]["3"] = 45;   // HEATING_BED = 3
    entry1["phase_durations"]["4"] = 30;   // HEATING_NOZZLE = 4
    entry1["phase_durations"]["5"] = 60;   // QGL = 5
    entry1["phase_durations"]["7"] = 90;   // BED_MESH = 7
    config["print_start_history"]["entries"] = json::array({entry1});

    // Run migration
    migrate_v10_to_v11(config);

    // Heat rates moved to new path
    REQUIRE(config.contains("/thermal/rates/extruder/heat_rate"_json_pointer));
    REQUIRE(config["/thermal/rates/extruder/heat_rate"_json_pointer].get<float>() ==
            Catch::Approx(0.45f));
    REQUIRE(config["/thermal/rates/heater_bed/heat_rate"_json_pointer].get<float>() ==
            Catch::Approx(1.3f));

    // Old PID heat_rate removed, oscillation_duration preserved
    REQUIRE_FALSE(config["calibration"]["pid_history"]["extruder"].contains("heat_rate"));
    REQUIRE(config["calibration"]["pid_history"]["extruder"]["oscillation_duration"].get<float>() ==
            Catch::Approx(110.0f));

    // Predictor entries: heating phases stripped, non-heating preserved
    auto& entries = config["print_start_history"]["entries"];
    REQUIRE(entries.size() == 1);
    auto& phases = entries[0]["phase_durations"];
    REQUIRE_FALSE(phases.contains("3"));  // HEATING_BED stripped
    REQUIRE_FALSE(phases.contains("4"));  // HEATING_NOZZLE stripped
    REQUIRE(phases["5"].get<int>() == 60);  // QGL preserved
    REQUIRE(phases["7"].get<int>() == 90);  // BED_MESH preserved
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[config][migration]" -v`
Expected: FAIL — `migrate_v10_to_v11` not defined.

- [ ] **Step 3: Bump CURRENT_CONFIG_VERSION**

In `include/config.h`, change line 58:
```cpp
static constexpr int CURRENT_CONFIG_VERSION = 11;
```

- [ ] **Step 4: Implement migrate_v10_to_v11**

In `src/system/config.cpp`, add in the anonymous namespace near other migrations:

```cpp
static void migrate_v10_to_v11(json& config) {
    // 1. Move PID heat rates to shared thermal path
    auto move_rate = [&](const std::string& heater) {
        json_pointer src("/calibration/pid_history/" + heater + "/heat_rate");
        json_pointer dst("/thermal/rates/" + heater + "/heat_rate");

        if (config.contains(src) && !config.contains(dst)) {
            // Ensure parent exists
            config[json_pointer("/thermal/rates/" + heater)] =
                json::object({{"heat_rate", config[src].get<float>()}});
            config[json_pointer("/calibration/pid_history/" + heater)].erase("heat_rate");
            spdlog::info("[Config] Migration v11: Moved {}/heat_rate to thermal/rates", heater);
        }
    };

    move_rate("extruder");
    move_rate("heater_bed");

    // 2. Strip heating phases from predictor entries
    if (config.contains("/print_start_history/entries"_json_pointer)) {
        auto& entries = config["/print_start_history/entries"_json_pointer];
        if (entries.is_array()) {
            for (auto& entry : entries) {
                if (entry.contains("phase_durations") && entry["phase_durations"].is_object()) {
                    auto& phases = entry["phase_durations"];
                    phases.erase("3");  // PrintStartPhase::HEATING_BED
                    phases.erase("4");  // PrintStartPhase::HEATING_NOZZLE
                }
            }
            spdlog::info("[Config] Migration v11: Stripped heating phases from {} predictor entries",
                         entries.size());
        }
    }
}
```

Register in `run_versioned_migrations()`:
```cpp
if (version < 11)
    migrate_v10_to_v11(config);
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[config][migration]" -v`
Expected: Migration test PASSES.

Also run full config tests: `./build/bin/helix-tests "[config]" -v`
Expected: All config tests PASS.

- [ ] **Step 6: Commit**

```bash
git add include/config.h src/system/config.cpp tests/unit/test_config.cpp
git commit -m "feat: config migration v10->v11 for shared thermal rate paths"
```

---

### Task 7: Integrate ThermalRateManager into PrintStartCollector

Wire the collector to feed live temperature samples to `ThermalRateModel` during heating phases, and use the thermal model for heating progress instead of static weights.

**Files:**
- Modify: `include/print_start_collector.h`
- Modify: `src/print/print_start_collector.cpp`

- [ ] **Step 1: Add ThermalRateManager to collector**

In `include/print_start_collector.h`, add `#include "thermal_rate_model.h"`.

Add a private method:
```cpp
void feed_thermal_sample();
```

- [ ] **Step 2: Implement thermal sample feeding**

In `src/print/print_start_collector.cpp`, add:

```cpp
void PrintStartCollector::feed_thermal_sample() {
    auto& mgr = ThermalRateManager::instance();
    auto now_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - printing_state_start_)
            .count());

    if (current_phase_ == PrintStartPhase::HEATING_BED ||
        current_phase_ == PrintStartPhase::HEATING_NOZZLE) {
        int ext_temp = lv_subject_get_int(state_.get_active_extruder_temp_subject());
        int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject());

        // Temps are in decidegrees
        mgr.get_model("extruder").record_sample(ext_temp / 10.0f, now_ms);
        mgr.get_model("heater_bed").record_sample(bed_temp / 10.0f, now_ms);
    }
}
```

Call `feed_thermal_sample()` from `update_eta_display()`, right after the elapsed time update (before the predictor snapshot).

- [ ] **Step 3: Reset thermal models on collector start**

In the collector's `start()` method, reset the thermal models with current temps:

```cpp
auto& mgr = ThermalRateManager::instance();
int ext_temp = lv_subject_get_int(state_.get_active_extruder_temp_subject()) / 10;
int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject()) / 10;
mgr.get_model("extruder").reset(static_cast<float>(ext_temp));
mgr.get_model("heater_bed").reset(static_cast<float>(bed_temp));
```

- [ ] **Step 4: Save thermal rates on collector completion**

In `save_prediction_entry()`, after saving the predictor entry, also save thermal rates:

```cpp
ThermalRateManager::instance().save_to_config(*config);
```

- [ ] **Step 5: Remove heating phases from saved predictor entries**

In `save_prediction_entry()`, when building the `PreprintEntry`, skip HEATING_BED and HEATING_NOZZLE phases from `phase_durations`. These are now handled by the thermal model.

```cpp
// When building phase_durations from phase_enter_times_:
if (phase_int == static_cast<int>(PrintStartPhase::HEATING_BED) ||
    phase_int == static_cast<int>(PrintStartPhase::HEATING_NOZZLE)) {
    continue;  // Heating is tracked by ThermalRateModel, not predictor
}
```

- [ ] **Step 6: Build and verify**

Run: `make -j`
Expected: Clean build. No errors.

- [ ] **Step 7: Commit**

```bash
git add include/print_start_collector.h src/print/print_start_collector.cpp
git commit -m "feat: collector feeds thermal samples and excludes heating from predictor"
```

---

### Task 8: Duration-proportional progress and smoother updates

Replace static profile weights with duration-proportional weights derived from predicted durations. Use temperature-driven progress during heating phases. Remove the `max(time, phase)` hack.

**Files:**
- Modify: `src/print/print_start_collector.cpp` (calculate_progress, update_eta_display)
- Modify: `include/print_start_collector.h` (add predicted weight fields)

- [ ] **Step 1: Add predicted duration fields to collector**

In `include/print_start_collector.h`, add private fields:

```cpp
std::map<int, float> predicted_phase_weights_;  ///< Phase -> fraction of total time (0.0-1.0)
float predicted_total_seconds_ = 0.0f;          ///< Total predicted duration
```

- [ ] **Step 2: Compute predicted weights at collector start**

In the collector's `start()` method, after loading prediction history, compute the weights:

```cpp
void PrintStartCollector::compute_predicted_weights() {
    auto& mgr = ThermalRateManager::instance();
    int ext_temp = lv_subject_get_int(state_.get_active_extruder_temp_subject()) / 10;
    int ext_target = lv_subject_get_int(state_.get_active_extruder_target_subject()) / 10;
    int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject()) / 10;
    int bed_target = lv_subject_get_int(state_.get_bed_target_subject()) / 10;

    std::map<int, float> durations;

    // Heating phases from thermal model
    float ext_heat = mgr.estimate_heating_seconds("extruder",
        static_cast<float>(ext_temp), static_cast<float>(ext_target));
    float bed_heat = mgr.estimate_heating_seconds("heater_bed",
        static_cast<float>(bed_temp), static_cast<float>(bed_target));
    if (ext_heat > 0) durations[static_cast<int>(PrintStartPhase::HEATING_NOZZLE)] = ext_heat;
    if (bed_heat > 0) durations[static_cast<int>(PrintStartPhase::HEATING_BED)] = bed_heat;

    // Non-heating phases from predictor
    auto pred_phases = predictor_.predicted_phases();
    for (const auto& [phase, secs] : pred_phases) {
        if (phase != static_cast<int>(PrintStartPhase::HEATING_BED) &&
            phase != static_cast<int>(PrintStartPhase::HEATING_NOZZLE)) {
            durations[phase] = static_cast<float>(secs);
        }
    }

    // Homing always happens — ensure it has at least a default
    int homing_phase = static_cast<int>(PrintStartPhase::HOMING);
    if (durations.find(homing_phase) == durations.end()) {
        durations[homing_phase] = 20.0f;
    }

    // Compute total and normalize to weights
    predicted_total_seconds_ = 0.0f;
    for (const auto& [_, dur] : durations) {
        predicted_total_seconds_ += dur;
    }

    predicted_phase_weights_.clear();
    if (predicted_total_seconds_ > 0.0f) {
        for (const auto& [phase, dur] : durations) {
            predicted_phase_weights_[phase] = dur / predicted_total_seconds_;
        }
    }
}
```

- [ ] **Step 3: Replace calculate_progress_locked() with proportional progress**

Replace the implementation in `src/print/print_start_collector.cpp`:

```cpp
int PrintStartCollector::calculate_progress_locked() const {
    if (predicted_phase_weights_.empty()) {
        // Fallback to profile weights if no predictions available
        if (!profile_) return 0;
        int total_weight = 0;
        for (const auto& phase : detected_phases_) {
            total_weight += profile_->get_phase_weight(phase);
        }
        return std::min(total_weight, 95);
    }

    float progress = 0.0f;

    // Add completed phases
    for (int phase : detected_phases_) {
        if (phase == static_cast<int>(current_phase_)) continue;  // Current = in-progress
        auto it = predicted_phase_weights_.find(phase);
        if (it != predicted_phase_weights_.end()) {
            progress += it->second;
        }
    }

    // Add partial progress for current phase
    auto cur_weight_it = predicted_phase_weights_.find(static_cast<int>(current_phase_));
    if (cur_weight_it != predicted_phase_weights_.end()) {
        float phase_fraction = 0.0f;

        if (current_phase_ == PrintStartPhase::HEATING_BED ||
            current_phase_ == PrintStartPhase::HEATING_NOZZLE) {
            // Temperature-driven progress for heating phases
            phase_fraction = compute_heating_fraction();
        } else {
            // Time-based progress for non-heating phases
            auto it = phase_enter_times_.find(static_cast<int>(current_phase_));
            if (it != phase_enter_times_.end()) {
                float elapsed = static_cast<float>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - it->second)
                        .count());
                auto pred = predictor_.predicted_phases();
                auto pred_it = pred.find(static_cast<int>(current_phase_));
                if (pred_it != pred.end() && pred_it->second > 0) {
                    phase_fraction = std::min(elapsed / static_cast<float>(pred_it->second), 0.95f);
                }
            }
        }

        progress += cur_weight_it->second * phase_fraction;
    }

    return std::min(static_cast<int>(progress * 95.0f), 95);  // Cap at 95%
}
```

- [ ] **Step 4: Add compute_heating_fraction helper**

Add to the collector (private method in header, implementation in cpp):

```cpp
float PrintStartCollector::compute_heating_fraction() const {
    int ext_temp = lv_subject_get_int(state_.get_active_extruder_temp_subject()) / 10;
    int ext_target = lv_subject_get_int(state_.get_active_extruder_target_subject()) / 10;
    int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject()) / 10;
    int bed_target = lv_subject_get_int(state_.get_bed_target_subject()) / 10;

    float ext_frac = 0.0f;
    float bed_frac = 0.0f;

    if (current_phase_ == PrintStartPhase::HEATING_NOZZLE && ext_target > 0) {
        // Use start temp from phase_enter_times_, fall back to 25°C
        float start = start_ext_temp_ > 0 ? static_cast<float>(start_ext_temp_) : 25.0f;
        float range = static_cast<float>(ext_target) - start;
        if (range > 0.0f) {
            ext_frac = std::clamp((static_cast<float>(ext_temp) - start) / range, 0.0f, 1.0f);
        }
        return ext_frac;
    }

    if (current_phase_ == PrintStartPhase::HEATING_BED && bed_target > 0) {
        float start = start_bed_temp_ > 0 ? static_cast<float>(start_bed_temp_) : 25.0f;
        float range = static_cast<float>(bed_target) - start;
        if (range > 0.0f) {
            bed_frac = std::clamp((static_cast<float>(bed_temp) - start) / range, 0.0f, 1.0f);
        }
        return bed_frac;
    }

    return 0.0f;
}
```

Add `int start_ext_temp_` and `int start_bed_temp_` fields to the header, captured in `start()`.

- [ ] **Step 5: Simplify update_eta_display()**

In `update_eta_display()`, replace the `max(time_progress, phase_progress)` block (around lines 800-802) with:

```cpp
int effective_progress = calculate_progress();
auto* subj = state_.get_print_start_progress_subject();
if (subj && lv_subject_get_int(subj) != effective_progress) {
    lv_subject_set_int(subj, effective_progress);
}
```

Remove the separate `time_progress` and `phase_progress` variables and the `std::max` call.

- [ ] **Step 6: Add monotonic bias under 2 minutes**

In `update_eta_display()`, after computing `remaining`:

```cpp
// Monotonic bias: once under 2 minutes, only decrease unless overrun > 20%
if (remaining < 120 && last_remaining_ > 0 && last_remaining_ < 120) {
    if (remaining > last_remaining_) {
        // Allow increase only if significant overrun (> 20% of prediction)
        float overrun_pct = static_cast<float>(remaining - last_remaining_) /
                            std::max(1.0f, predicted_total_seconds_) * 100.0f;
        if (overrun_pct < 20.0f) {
            remaining = last_remaining_;  // Clamp to previous
        }
    }
}
last_remaining_ = remaining;
```

Add `int last_remaining_ = 0;` to the header's private fields.

- [ ] **Step 7: Build and verify**

Run: `make -j`
Expected: Clean build.

- [ ] **Step 8: Commit**

```bash
git add include/print_start_collector.h src/print/print_start_collector.cpp
git commit -m "feat: duration-proportional progress with temperature-driven heating"
```

---

### Task 9: Detail view pre-print estimate

Add an estimated prep time display to the print file detail view that updates as the user toggles checkboxes.

**Files:**
- Modify: `include/ui_print_preparation_manager.h`
- Modify: `src/ui/ui_print_preparation_manager.cpp`
- Modify: `ui_xml/print_file_detail.xml`

- [ ] **Step 1: Add estimate subject to PrintPreparationManager**

In `include/ui_print_preparation_manager.h`, add:

```cpp
#include "thermal_rate_model.h"

// In the private section:
lv_subject_t preprint_estimate_subject_;  ///< Estimated prep time in seconds
bool estimate_subject_initialized_ = false;

// Public:
lv_subject_t* get_preprint_estimate_subject();
void recalculate_estimate();
```

- [ ] **Step 2: Initialize subject and wire recalculation**

In `src/ui/ui_print_preparation_manager.cpp`:

Initialize in the constructor or init method:
```cpp
lv_subject_init_int(&preprint_estimate_subject_, 0);
estimate_subject_initialized_ = true;
```

Implement `recalculate_estimate()`:
```cpp
void PrintPreparationManager::recalculate_estimate() {
    if (!estimate_subject_initialized_) return;

    auto& mgr = ThermalRateManager::instance();
    auto& state = PrinterState::instance();

    // Get current and target temps (decidegrees -> degrees)
    float ext_temp = static_cast<float>(
        lv_subject_get_int(state.get_active_extruder_temp_subject())) / 10.0f;
    float ext_target = static_cast<float>(
        lv_subject_get_int(state.get_active_extruder_target_subject())) / 10.0f;
    float bed_temp = static_cast<float>(
        lv_subject_get_int(state.get_bed_temp_subject())) / 10.0f;
    float bed_target = static_cast<float>(
        lv_subject_get_int(state.get_bed_target_subject())) / 10.0f;

    float total = 0.0f;

    // Heating estimates from thermal model
    total += mgr.estimate_heating_seconds("extruder", ext_temp, ext_target);
    total += mgr.estimate_heating_seconds("heater_bed", bed_temp, bed_target);

    // Non-heating operation estimates from predictor
    total += 20.0f;  // HOMING always happens

    auto pred = PreprintPredictor::load_entries_from_config();
    PreprintPredictor predictor;
    bool is_warm = bed_temp >= 40.0f;
    predictor.load_entries(pred,
        is_warm ? PreprintPredictor::StartCondition::WARM
                : PreprintPredictor::StartCondition::COLD);
    auto phases = predictor.predicted_phases();

    // Add enabled operations
    auto add_if_enabled = [&](lv_subject_t* subj, PrintStartPhase phase, float default_s) {
        if (subj && lv_subject_get_int(subj) == 1) {
            auto it = phases.find(static_cast<int>(phase));
            total += (it != phases.end()) ? static_cast<float>(it->second) : default_s;
        }
    };

    add_if_enabled(preprint_bed_mesh_subject_, PrintStartPhase::BED_MESH, 90.0f);
    add_if_enabled(preprint_qgl_subject_, PrintStartPhase::QGL, 60.0f);
    add_if_enabled(preprint_z_tilt_subject_, PrintStartPhase::Z_TILT, 45.0f);
    add_if_enabled(preprint_nozzle_clean_subject_, PrintStartPhase::CLEANING, 15.0f);
    add_if_enabled(preprint_purge_subject_, PrintStartPhase::PURGING, 10.0f);

    int estimate_s = static_cast<int>(total);
    if (lv_subject_get_int(&preprint_estimate_subject_) != estimate_s) {
        lv_subject_set_int(&preprint_estimate_subject_, estimate_s);
    }
}
```

- [ ] **Step 3: Call recalculate on checkbox toggles**

In each checkbox callback (`on_preprint_bed_mesh_toggled`, etc.), add a call to `recalculate_estimate()` at the end. Also call it when the filament preset changes and when the detail view opens.

Find the existing toggle callback implementations and append:
```cpp
recalculate_estimate();
```

- [ ] **Step 4: Add estimate label to XML**

In `ui_xml/print_file_detail.xml`, add a label near the print button (around line 275, before the button row):

```xml
<lv_obj name="estimate_row" width="100%" flex_flow="row" style_pad_gap="#space_sm"
        flex_main_place="center" style_pad_top="#space_xs">
    <text_small name="prep_estimate_label" bind_text="preprint_estimate_formatted"
                style_text_color="#text_secondary"/>
</lv_obj>
```

The formatting (seconds -> "~X min") should be done via a string subject that observes the int subject and formats it. Follow existing patterns in the codebase for this — look at how `print_start_time_left` formats remaining time.

- [ ] **Step 5: Register the subject with XML**

Register `preprint_estimate_formatted` as an LVGL string subject that formats the int estimate. In the panel setup code:

```cpp
// Format: "~X min" for > 120s, "~X:XX" for <= 120s, empty if 0
static void format_estimate(lv_subject_t* /*src*/, lv_observer_t* observer) {
    auto* mgr = static_cast<PrintPreparationManager*>(lv_observer_get_user_data(observer));
    int seconds = lv_subject_get_int(mgr->get_preprint_estimate_subject());
    if (seconds <= 0) {
        lv_subject_set_pointer(&mgr->estimate_text_subject_, "");
        return;
    }
    // Round: >120s to nearest 30s, <=120s to nearest 10s
    if (seconds > 120) {
        seconds = ((seconds + 15) / 30) * 30;
    } else {
        seconds = ((seconds + 5) / 10) * 10;
    }
    std::string text = "~" + helix::format::duration_remaining(seconds);
    // Store in member to keep pointer valid
    mgr->estimate_text_ = text;
    lv_subject_set_pointer(&mgr->estimate_text_subject_, mgr->estimate_text_.c_str());
}
```

- [ ] **Step 6: Build and test manually**

Run: `make -j && HELIX_HOT_RELOAD=1 ./build/bin/helix-screen --test -vv`
Expected: Navigate to file detail view. See estimated prep time. Toggle checkboxes and observe the estimate change.

- [ ] **Step 7: Commit**

```bash
git add include/ui_print_preparation_manager.h src/ui/ui_print_preparation_manager.cpp ui_xml/print_file_detail.xml
git commit -m "feat: pre-print time estimate on file detail view"
```

---

### Task 10: Integrate pre-print estimate into finish time ETA

Wire the pre-print remaining time into the overall finish time calculation so "finish at X:XX" is accurate from the moment the user starts a print.

**Files:**
- Modify: `src/printer/printer_print_state.cpp`
- Modify: `src/ui/ui_panel_print_status.cpp`

- [ ] **Step 1: Add prep time to ETA during pre-print phase**

In `src/printer/printer_print_state.cpp`, find where `print_time_left_` is set (around lines 298-340). Add handling for the pre-print phase (progress == 0, print hasn't started yet):

Before the existing `if (progress >= 1 ...)` block, add:

```cpp
// During pre-print phase: combine prep remaining with slicer print estimate
int preprint_remaining = lv_subject_get_int(&preprint_remaining_);
if (progress == 0 && preprint_remaining > 0 && estimated_print_time_ > 0) {
    int total_remaining = preprint_remaining + estimated_print_time_;
    if (lv_subject_get_int(&print_time_left_) != total_remaining) {
        lv_subject_set_int(&print_time_left_, total_remaining);
    }
}
```

Note: The `preprint_remaining_` subject is already on PrinterState — it's set by the collector's `update_eta_display()`. Verify this subject is accessible here. If it's not a member of `PrinterPrintState`, it may need to be accessed via `PrinterState::instance().get_preprint_remaining_subject()`.

- [ ] **Step 2: Use slicer estimate at low progress**

In the existing remaining time calculation, modify the early-progress behavior. Around lines 298-310, when `progress < 5`:

```cpp
if (progress >= 1 && progress < 5 && estimated_print_time_ > 0) {
    // Very early in print — use slicer estimate directly, don't extrapolate
    // from tiny progress which is wildly noisy
    int remaining = estimated_print_time_ * (100 - progress) / 100;
    if (lv_subject_get_int(&print_time_left_) != remaining) {
        lv_subject_set_int(&print_time_left_, remaining);
    }
}
```

This replaces the previous behavior where progress < 15% used a blend — now progress < 5% uses pure slicer estimate, and the existing blend logic kicks in at 5-15%.

- [ ] **Step 3: Build and verify**

Run: `make -j`
Expected: Clean build.

- [ ] **Step 4: Commit**

```bash
git add src/printer/printer_print_state.cpp
git commit -m "feat: integrate pre-print time into finish time ETA"
```

---

### Task 11: Load ThermalRateManager at startup

Wire `ThermalRateManager` loading into the application initialization flow so persisted rates are available when the detail view or collector needs them. Apply archetype defaults after printer detection.

**Files:**
- Modify: `src/application/moonraker_manager.cpp` (or wherever printer init happens)
- Read: `include/printer_detector.h` (for build volume access)

- [ ] **Step 1: Load thermal rates from config on startup**

Find where `Config::init()` completes and PrinterState is initialized (likely in `Application::init()` or `MoonrakerManager` initialization). After config is loaded:

```cpp
#include "thermal_rate_model.h"

// After config init:
ThermalRateManager::instance().load_from_config(Config::instance());
```

- [ ] **Step 2: Apply archetype defaults after printer detection**

Find where `PrinterDetector` results are available (after connecting to a printer). This is likely in `MoonrakerManager` after printer discovery:

```cpp
// After printer detection:
auto& detector = PrinterDetector::instance();
auto build_vol = detector.get_build_volume();  // Check exact API
ThermalRateManager::instance().apply_archetype_defaults(build_vol.x_max);
```

Check the exact `PrinterDetector` API for build volume access. The build volume might be on a `PrinterInfo` struct or similar. Read `include/printer_detector.h` lines around `BuildVolume` to find the accessor.

- [ ] **Step 3: Build and verify**

Run: `make -j && ./build/bin/helix-screen --test -vv`
Expected: See log messages from `ThermalRateManager` on startup (loaded rates or applied defaults).

- [ ] **Step 4: Commit**

```bash
git add src/application/moonraker_manager.cpp
git commit -m "feat: load ThermalRateManager at startup with archetype defaults"
```

---

### Task 12: Update collector temperature bucketing

Update `save_prediction_entry()` to use the new cold/warm bucketing instead of 25°C nozzle temperature buckets. Update `load_prediction_history()` to pass the simplified bucket.

**Files:**
- Modify: `src/print/print_start_collector.cpp`

- [ ] **Step 1: Update save_prediction_entry bucketing**

In `save_prediction_entry()`, replace the temperature bucket calculation (around line 876):

```cpp
// OLD:
// int current_bucket = (ext_target > 0) ? ((ext_target + 12) / 25) * 25 : 0;
// entry.temp_bucket = current_bucket;

// NEW: Cold (1) vs Warm (2) based on bed temp at start
// start_bed_temp_ captured at collector start()
entry.temp_bucket = (start_bed_temp_ >= 40) ? 2 : 1;
```

- [ ] **Step 2: Update load_prediction_history bucketing**

In `load_prediction_history()`, replace the bucket calculation:

```cpp
// OLD:
// int ext_target = lv_subject_get_int(state_.get_active_extruder_target_subject()) / 10;
// loaded_temp_bucket_ = (ext_target > 0) ? ((ext_target + 12) / 25) * 25 : 0;

// NEW:
int bed_temp = lv_subject_get_int(state_.get_bed_temp_subject()) / 10;
loaded_temp_bucket_ = (bed_temp >= 40) ? 2 : 1;
```

Pass the corresponding `StartCondition` to `predictor_.load_entries()`.

- [ ] **Step 3: Remove the mid-print bucket reload**

In `update_eta_display()`, remove the block that checks for nozzle target bucket changes and reloads predictions (around lines 763-775). With cold/warm bucketing, the bucket doesn't change mid-print.

- [ ] **Step 4: Build and verify**

Run: `make -j`
Expected: Clean build.

- [ ] **Step 5: Commit**

```bash
git add src/print/print_start_collector.cpp
git commit -m "refactor: simplify predictor bucketing to cold/warm start"
```

---

### Task 13: Smarter non-heating defaults from PrinterDetector

When no prediction history exists, use printer capabilities to provide better default estimates for non-heating operations.

**Files:**
- Modify: `src/print/preprint_predictor.cpp` (or wherever defaults are provided)
- Modify: `include/preprint_predictor.h`

- [ ] **Step 1: Add static default durations method**

In `include/preprint_predictor.h`, add:

```cpp
/// Default operation durations in seconds. Used when no history exists.
/// Keyed by PrintStartPhase enum value.
static std::map<int, int> default_phase_durations();
```

- [ ] **Step 2: Implement with capability-aware defaults**

In `src/print/preprint_predictor.cpp`:

```cpp
std::map<int, int> PreprintPredictor::default_phase_durations() {
    return {
        {static_cast<int>(PrintStartPhase::HOMING), 20},
        {static_cast<int>(PrintStartPhase::BED_MESH), 90},
        {static_cast<int>(PrintStartPhase::QGL), 60},
        {static_cast<int>(PrintStartPhase::Z_TILT), 45},
        {static_cast<int>(PrintStartPhase::CLEANING), 15},
        {static_cast<int>(PrintStartPhase::PURGING), 10},
    };
}
```

- [ ] **Step 3: Use defaults in predicted_phases() when no entries**

In `predicted_phases()`, when `entries_.empty()`:

```cpp
std::map<int, int> PreprintPredictor::predicted_phases() const {
    if (entries_.empty()) {
        return default_phase_durations();
    }
    // ... existing weighted average logic ...
}
```

- [ ] **Step 4: Build and run tests**

Run: `make test && ./build/bin/helix-tests "[preprint_predictor]" -v`
Expected: All tests PASS.

- [ ] **Step 5: Commit**

```bash
git add include/preprint_predictor.h src/print/preprint_predictor.cpp
git commit -m "feat: smarter non-heating defaults for PreprintPredictor"
```

---

### Task 14: ETA rounding utility

Add a rounding function for ETA display: nearest 30s when > 2 min, nearest 10s when <= 2 min. Used by both the detail view estimate and the live progress ETA.

**Files:**
- Modify: `src/format_utils.cpp` (or wherever `duration_remaining` lives)
- Modify: `include/format_utils.h`

- [ ] **Step 1: Find the existing format function**

Check `include/format_utils.h` and `src/format_utils.cpp` for the `duration_remaining()` function. Understand its current signature and output format.

- [ ] **Step 2: Add rounding helper**

In `include/format_utils.h` (in the `helix::format` namespace):

```cpp
/// Round seconds for stable display: nearest 30s when > 120s, nearest 10s when <= 120s.
int round_eta_seconds(int seconds);
```

In `src/format_utils.cpp`:

```cpp
int helix::format::round_eta_seconds(int seconds) {
    if (seconds <= 0) return 0;
    if (seconds > 120) {
        return ((seconds + 15) / 30) * 30;
    }
    return ((seconds + 5) / 10) * 10;
}
```

- [ ] **Step 3: Apply rounding in collector ETA display**

In `update_eta_display()`, apply rounding before formatting:

```cpp
int display_remaining = helix::format::round_eta_seconds(remaining);
std::string text = "~" + helix::format::duration_remaining(display_remaining);
```

- [ ] **Step 4: Build and verify**

Run: `make -j`
Expected: Clean build.

- [ ] **Step 5: Commit**

```bash
git add include/format_utils.h src/format_utils.cpp src/print/print_start_collector.cpp
git commit -m "feat: ETA rounding for stable display (30s/10s buckets)"
```

---

### Task 15: Final integration testing and cleanup

Verify the full flow end-to-end: detail view estimate, live progress during print, finish time accuracy.

**Files:**
- Run all tests
- Manual testing with `--test` mode

- [ ] **Step 1: Run all unit tests**

Run: `make test-run`
Expected: All tests PASS. No regressions.

- [ ] **Step 2: Manual test — detail view estimate**

Run: `HELIX_HOT_RELOAD=1 ./build/bin/helix-screen --test -vv`

1. Navigate to file browser, select a file
2. Verify prep time estimate appears on detail view
3. Toggle bed mesh checkbox — estimate should change
4. Toggle QGL checkbox — estimate should change
5. Check log output for `ThermalRateManager` messages

- [ ] **Step 3: Manual test — live progress during simulated print**

1. Start a print in test mode
2. Watch the progress bar — should advance smoothly with 5s updates
3. During heating phases, progress should correlate with temperature rise
4. ETA display should show rounded values, not flicker
5. Check that finish time ("~X:XX PM") includes prep time at the start

- [ ] **Step 4: Verify config migration**

Create a test config with v10 format, run the application, verify:
1. Heat rates migrated to `/thermal/rates/`
2. Old `/calibration/pid_history/` heat_rate keys removed
3. Predictor entries have heating phases stripped
4. Oscillation duration preserved at old path

- [ ] **Step 5: Final commit if any cleanup needed**

```bash
git add -A
git commit -m "fix: integration testing cleanup for adaptive preprint timing"
```
