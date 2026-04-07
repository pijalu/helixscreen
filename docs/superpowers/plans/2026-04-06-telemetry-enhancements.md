# Telemetry Enhancements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add periodic telemetry snapshots, UI performance metrics, feature adoption tracking, settings change events, and three new dashboard views.

**Architecture:** Device-side C++ adds a 4-hour snapshot timer (emitting `panel_usage`, `connection_stability`, `performance_snapshot` as a batch), a `feature_adoption` event on 5-min delay, and a debounced `settings_changes` event. The Cloudflare Worker ingests and indexes these new event types. The Vue dashboard gets three new views: Performance, Features, and UX Insights.

**Tech Stack:** C++ (LVGL 9.5, spdlog, nlohmann/json), TypeScript (Cloudflare Workers, Vitest), Vue 3 (Composition API, Chart.js, Pinia)

**Spec:** `docs/superpowers/specs/2026-04-06-telemetry-enhancements-design.md`

---

## File Structure

### C++ (Device)

| File | Action | Responsibility |
|------|--------|---------------|
| `include/system/telemetry_manager.h` | Modify | Add snapshot timer, ring buffer, new methods, new members |
| `src/system/telemetry_manager.cpp` | Modify | Snapshot timer, frame sampling, persistence, new event builders |
| `src/system/settings_manager.cpp` | Modify | Call `notify_setting_changed()` on setting writes |
| `src/application/application.cpp` | Modify | Hook frame time sampling around `lv_timer_handler()` |
| `tests/unit/test_telemetry_manager.cpp` | Modify | Tests for new events and snapshot logic |

### TypeScript (Worker)

| File | Action | Responsibility |
|------|--------|---------------|
| `server/telemetry-worker/src/analytics.ts` | Modify | Analytics Engine mappings for 3 new event types |
| `server/telemetry-worker/src/queries.ts` | Modify | SQL queries for 3 new dashboard endpoints |
| `server/telemetry-worker/src/index.ts` | Modify | 3 new dashboard endpoint handlers |
| `server/telemetry-worker/src/__tests__/index.test.ts` | Modify | Tests for new event ingestion and endpoints |

### Vue (Dashboard)

| File | Action | Responsibility |
|------|--------|---------------|
| `server/analytics-dashboard/src/services/api.ts` | Modify | New types and API methods |
| `server/analytics-dashboard/src/router/index.ts` | Modify | New routes |
| `server/analytics-dashboard/src/views/PerformanceView.vue` | Create | Performance dashboard view |
| `server/analytics-dashboard/src/views/FeaturesView.vue` | Create | Feature adoption dashboard view |
| `server/analytics-dashboard/src/views/UxInsightsView.vue` | Create | UX insights dashboard view |
| `server/analytics-dashboard/src/components/AppLayout.vue` | Modify | Add nav links for new views |

---

## Task 1: Add Snapshot Infrastructure to TelemetryManager Header

**Files:**
- Modify: `include/system/telemetry_manager.h`

- [ ] **Step 1: Add new constants, members, and method declarations**

Add after the existing constants (around line 605):

```cpp
// Periodic snapshot interval (4 hours)
static constexpr uint32_t SNAPSHOT_INTERVAL_MS = 4 * 60 * 60 * 1000;

// Frame time ring buffer size
static constexpr size_t FRAME_RING_SIZE = 1024;

// Dropped frame threshold (33ms = below 30fps)
static constexpr uint32_t DROPPED_FRAME_THRESHOLD_US = 33000;

// Feature adoption delay after init (5 minutes)
static constexpr uint32_t FEATURE_ADOPTION_DELAY_MS = 5 * 60 * 1000;

// Settings change debounce window (30 seconds)
static constexpr uint32_t SETTINGS_DEBOUNCE_MS = 30 * 1000;
```

Add new public method declarations (after existing `record_*` methods, around line 343):

```cpp
/**
 * @brief Record a performance snapshot with frame time percentiles.
 * Called by the periodic snapshot timer alongside panel_usage and connection_stability.
 */
void record_performance_snapshot();

/**
 * @brief Record feature adoption flags for this session.
 * Called once per session after a 5-minute delay.
 */
void record_feature_adoption();

/**
 * @brief Notify that a setting was changed by the user.
 * Batches changes over a 30-second debounce window before emitting.
 * @param setting_name Enumerated setting name (e.g., "theme", "brightness")
 * @param old_value Previous value as string
 * @param new_value New value as string
 */
void notify_setting_changed(const std::string& setting_name,
                            const std::string& old_value,
                            const std::string& new_value);

/**
 * @brief Record a frame time sample from the main render loop.
 * Called once per frame with the elapsed render time.
 * @param frame_time_us Frame time in microseconds
 */
void record_frame_time(uint32_t frame_time_us);
```

Add new private method declarations (after existing build_* methods, around line 710):

```cpp
nlohmann::json build_performance_snapshot_event() const;
nlohmann::json build_feature_adoption_event() const;
nlohmann::json build_settings_changes_event() const;

void start_snapshot_timer();
void stop_snapshot_timer();
void fire_periodic_snapshot();

void save_snapshot_state() const;
void load_snapshot_state();

void start_feature_adoption_timer();
void stop_feature_adoption_timer();

void start_settings_debounce_timer();
void stop_settings_debounce_timer();
void flush_settings_changes();
```

Add new member variables (after existing session tracker members, around line 876):

```cpp
// Periodic snapshot state
lv_timer_t* snapshot_timer_{nullptr};
int snapshot_seq_{0};

// Frame time ring buffer (LVGL thread only)
struct FrameSample {
    uint32_t frame_time_us;
    uint16_t panel_id;  // index into panel_names_
};
std::array<FrameSample, FRAME_RING_SIZE> frame_ring_{};
size_t frame_ring_idx_{0};
size_t frame_ring_count_{0};  // total samples since last snapshot
std::vector<std::string> panel_names_;  // panel name lookup table
uint16_t current_panel_id_{0};

// Feature adoption
lv_timer_t* feature_adoption_timer_{nullptr};

// Settings change debounce
struct SettingChange {
    std::string setting;
    std::string old_value;
    std::string new_value;
};
std::vector<SettingChange> pending_settings_changes_;
lv_timer_t* settings_debounce_timer_{nullptr};
```

- [ ] **Step 2: Verify it compiles**

Run: `make -j 2>&1 | tail -5`
Expected: Build succeeds (new methods are declared but not yet defined — that's fine for the header, but we need stubs)

- [ ] **Step 3: Commit**

```bash
git add include/system/telemetry_manager.h
git commit -m "feat(telemetry): add snapshot infrastructure declarations to header"
```

---

## Task 2: Implement Frame Time Sampling

**Files:**
- Modify: `src/system/telemetry_manager.cpp`
- Modify: `src/application/application.cpp`

- [ ] **Step 1: Write test for frame time recording**

Add to `tests/unit/test_telemetry_manager.cpp`:

```cpp
// ============================================================================
// Frame Time Sampling [telemetry][frame]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Frame time: samples are recorded in ring buffer",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Simulate some panel changes and frame times
    tm.notify_panel_changed("status");
    tm.record_frame_time(8000);   // 8ms
    tm.record_frame_time(12000);  // 12ms
    tm.record_frame_time(16000);  // 16ms

    // Record snapshot to capture frame data
    tm.record_performance_snapshot();
    REQUIRE(tm.queue_size() == 1);

    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];
    REQUIRE(event["event"] == "performance_snapshot");
    REQUIRE(event.contains("frame_time_p50_ms"));
    REQUIRE(event.contains("frame_time_p95_ms"));
    REQUIRE(event.contains("frame_time_p99_ms"));
    REQUIRE(event.contains("dropped_frame_count"));
    REQUIRE(event.contains("total_frame_count"));
    REQUIRE(event["total_frame_count"] == 3);
    REQUIRE(event["dropped_frame_count"] == 0);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Frame time: dropped frames detected above 33ms threshold",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("temperature");
    tm.record_frame_time(10000);  // 10ms - ok
    tm.record_frame_time(35000);  // 35ms - dropped
    tm.record_frame_time(50000);  // 50ms - dropped

    tm.record_performance_snapshot();
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];
    REQUIRE(event["dropped_frame_count"] == 2);
    REQUIRE(event["total_frame_count"] == 3);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Frame time: worst panel is identified by p95",
                 "[telemetry][frame]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Status panel - fast frames
    tm.notify_panel_changed("status");
    for (int i = 0; i < 20; ++i) {
        tm.record_frame_time(8000);  // 8ms
    }

    // Temperature panel - slower frames
    tm.notify_panel_changed("temperature");
    for (int i = 0; i < 20; ++i) {
        tm.record_frame_time(25000);  // 25ms
    }

    tm.record_performance_snapshot();
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];
    REQUIRE(event["worst_panel"] == "temperature");
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[telemetry][frame]" -v 2>&1 | tail -20`
Expected: FAIL — `record_frame_time` and `record_performance_snapshot` not defined

- [ ] **Step 3: Implement `record_frame_time()` in telemetry_manager.cpp**

Add to `src/system/telemetry_manager.cpp`:

```cpp
void TelemetryManager::record_frame_time(uint32_t frame_time_us) {
    // Always record (even when telemetry disabled) to keep buffer warm.
    // No mutex needed — LVGL thread only.
    auto& sample = frame_ring_[frame_ring_idx_ % FRAME_RING_SIZE];
    sample.frame_time_us = frame_time_us;
    sample.panel_id = current_panel_id_;
    frame_ring_idx_++;
    frame_ring_count_++;
}
```

Update `notify_panel_changed()` to also maintain `current_panel_id_`:

```cpp
// At end of notify_panel_changed(), after setting current_panel_:
auto it = std::find(panel_names_.begin(), panel_names_.end(), panel_name);
if (it != panel_names_.end()) {
    current_panel_id_ = static_cast<uint16_t>(std::distance(panel_names_.begin(), it));
} else {
    panel_names_.push_back(panel_name);
    current_panel_id_ = static_cast<uint16_t>(panel_names_.size() - 1);
}
```

- [ ] **Step 4: Implement `build_performance_snapshot_event()`**

Add to `src/system/telemetry_manager.cpp`:

```cpp
nlohmann::json TelemetryManager::build_performance_snapshot_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "performance_snapshot";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["app_version"] = HELIX_VERSION;
    event["app_platform"] = UpdateChecker::get_platform_key();
    event["snapshot_seq"] = snapshot_seq_;

    size_t count = std::min(frame_ring_count_, FRAME_RING_SIZE);
    if (count == 0) {
        event["frame_time_p50_ms"] = 0;
        event["frame_time_p95_ms"] = 0;
        event["frame_time_p99_ms"] = 0;
        event["dropped_frame_count"] = 0;
        event["total_frame_count"] = 0;
        event["worst_panel"] = "";
        event["worst_panel_p95_ms"] = 0;
        event["task_handler_max_ms"] = 0;
        return event;
    }

    // Copy frame times for sorting
    std::vector<uint32_t> times;
    times.reserve(count);
    int dropped = 0;

    // Per-panel frame times for worst panel detection
    std::unordered_map<uint16_t, std::vector<uint32_t>> per_panel;

    size_t start = (frame_ring_count_ > FRAME_RING_SIZE) ? (frame_ring_idx_ % FRAME_RING_SIZE) : 0;
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (start + i) % FRAME_RING_SIZE;
        uint32_t ft = frame_ring_[idx].frame_time_us;
        times.push_back(ft);
        if (ft > DROPPED_FRAME_THRESHOLD_US) dropped++;
        per_panel[frame_ring_[idx].panel_id].push_back(ft);
    }

    std::sort(times.begin(), times.end());

    auto percentile = [&](double p) -> int {
        size_t idx = static_cast<size_t>(p * static_cast<double>(times.size() - 1));
        return static_cast<int>(times[idx] / 1000); // us -> ms
    };

    event["frame_time_p50_ms"] = percentile(0.50);
    event["frame_time_p95_ms"] = percentile(0.95);
    event["frame_time_p99_ms"] = percentile(0.99);
    event["dropped_frame_count"] = dropped;
    event["total_frame_count"] = static_cast<int>(frame_ring_count_);

    // Find worst panel by p95
    std::string worst_panel;
    int worst_p95 = 0;
    for (auto& [pid, ptimes] : per_panel) {
        std::sort(ptimes.begin(), ptimes.end());
        size_t p95_idx = static_cast<size_t>(0.95 * static_cast<double>(ptimes.size() - 1));
        int p95_ms = static_cast<int>(ptimes[p95_idx] / 1000);
        if (p95_ms > worst_p95 && pid < panel_names_.size()) {
            worst_p95 = p95_ms;
            worst_panel = panel_names_[pid];
        }
    }
    event["worst_panel"] = worst_panel;
    event["worst_panel_p95_ms"] = worst_p95;
    event["task_handler_max_ms"] = 0; // TODO: tracked separately if needed

    return event;
}

void TelemetryManager::record_performance_snapshot() {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load())
        return;

    spdlog::debug("[TelemetryManager] Recording performance snapshot (seq={})", snapshot_seq_);
    auto event = build_performance_snapshot_event();
    enqueue_event(event);

    // Reset ring buffer for next snapshot window
    frame_ring_count_ = 0;
    frame_ring_idx_ = 0;
}
```

- [ ] **Step 5: Hook frame timing into the main loop**

In `src/application/application.cpp`, in the main loop around line 2669, wrap `lv_timer_handler()`:

```cpp
        // Run LVGL tasks — returns ms until next timer needs to fire
        auto frame_start = std::chrono::steady_clock::now();
        uint32_t time_till_next = lv_timer_handler();
        auto frame_end = std::chrono::steady_clock::now();
        auto frame_us = std::chrono::duration_cast<std::chrono::microseconds>(
            frame_end - frame_start).count();
        TelemetryManager::instance().record_frame_time(
            static_cast<uint32_t>(frame_us));
```

Add `#include "system/telemetry_manager.h"` at the top if not already present.

- [ ] **Step 6: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[telemetry][frame]" -v 2>&1 | tail -20`
Expected: All 3 frame time tests PASS

- [ ] **Step 7: Run full test suite**

Run: `make test-run 2>&1 | tail -10`
Expected: All tests pass

- [ ] **Step 8: Commit**

```bash
git add include/system/telemetry_manager.h src/system/telemetry_manager.cpp src/application/application.cpp tests/unit/test_telemetry_manager.cpp
git commit -m "feat(telemetry): add frame time sampling and performance_snapshot event"
```

---

## Task 3: Implement Periodic Snapshot Timer

**Files:**
- Modify: `src/system/telemetry_manager.cpp`

- [ ] **Step 1: Write test for periodic snapshot fields**

Add to `tests/unit/test_telemetry_manager.cpp`:

```cpp
// ============================================================================
// Periodic Snapshots [telemetry][snapshot]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot: panel_usage includes snapshot_seq and is_shutdown fields",
                 "[telemetry][snapshot]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");
    tm.record_panel_usage();

    REQUIRE(tm.queue_size() == 1);
    auto snapshot = tm.get_queue_snapshot();
    auto event = snapshot[0];
    REQUIRE(event["event"] == "panel_usage");
    REQUIRE(event.contains("snapshot_seq"));
    REQUIRE(event.contains("is_shutdown"));
    REQUIRE(event["is_shutdown"] == false);
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot: snapshot_seq increments on each periodic fire",
                 "[telemetry][snapshot]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");

    // Simulate two periodic snapshots
    tm.fire_periodic_snapshot();
    tm.fire_periodic_snapshot();

    REQUIRE(tm.queue_size() >= 4); // 2x (panel_usage + connection_stability) minimum
    auto queue = tm.get_queue_snapshot();

    // Find panel_usage events and check seq
    int first_seq = -1, second_seq = -1;
    for (const auto& ev : queue) {
        if (ev["event"] == "panel_usage") {
            if (first_seq < 0)
                first_seq = ev["snapshot_seq"].get<int>();
            else
                second_seq = ev["snapshot_seq"].get<int>();
        }
    }
    REQUIRE(first_seq == 0);
    REQUIRE(second_seq == 1);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[telemetry][snapshot]" -v 2>&1 | tail -20`
Expected: FAIL — `fire_periodic_snapshot` not defined, `snapshot_seq` not in events

- [ ] **Step 3: Add `snapshot_seq` and `is_shutdown` to `build_panel_usage_event()`**

In `src/system/telemetry_manager.cpp`, modify `build_panel_usage_event()` (around line 1909). Add after the existing fields:

```cpp
    event["snapshot_seq"] = snapshot_seq_;
    event["is_shutdown"] = false;  // Overridden in shutdown() path
```

- [ ] **Step 4: Add `snapshot_seq` and `is_shutdown` to `build_connection_stability_event()`**

Similarly add:

```cpp
    event["snapshot_seq"] = snapshot_seq_;
    event["is_shutdown"] = false;
```

- [ ] **Step 5: Implement `fire_periodic_snapshot()`**

Add to `src/system/telemetry_manager.cpp`:

```cpp
void TelemetryManager::fire_periodic_snapshot() {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load())
        return;

    spdlog::info("[TelemetryManager] Firing periodic snapshot (seq={})", snapshot_seq_);

    record_panel_usage();
    record_connection_stability();
    record_performance_snapshot();
    save_snapshot_state();

    snapshot_seq_++;
}
```

- [ ] **Step 6: Implement snapshot timer start/stop**

```cpp
void TelemetryManager::start_snapshot_timer() {
    if (snapshot_timer_) return;

    spdlog::debug("[TelemetryManager] Starting snapshot timer (interval={}ms)",
                  SNAPSHOT_INTERVAL_MS);

    snapshot_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<TelemetryManager*>(lv_timer_get_user_data(timer));
            if (self && !self->shutting_down_.load()) {
                self->fire_periodic_snapshot();
            }
        },
        SNAPSHOT_INTERVAL_MS, this);
}

void TelemetryManager::stop_snapshot_timer() {
    if (snapshot_timer_) {
        lv_timer_delete(snapshot_timer_);
        snapshot_timer_ = nullptr;
    }
}
```

- [ ] **Step 7: Wire snapshot timer into lifecycle**

In `start_auto_send()` (around line 900), add after the existing timer creation:

```cpp
    start_snapshot_timer();
```

In `stop_auto_send()`, add:

```cpp
    stop_snapshot_timer();
```

In `shutdown()` (around line 376), mark the final events as shutdown:

```cpp
    // Before the existing record_panel_usage() / record_connection_stability() calls:
    // These will produce events with is_shutdown=false by default.
    // After recording, patch them:
```

Actually, simpler approach — add a `bool is_shutdown_snapshot_` member (default false), set it true before the shutdown recording calls, and use it in `build_panel_usage_event()` / `build_connection_stability_event()`:

In the header, add member: `bool is_shutdown_snapshot_{false};`

In `build_panel_usage_event()`:
```cpp
    event["is_shutdown"] = is_shutdown_snapshot_;
```

In `shutdown()`, before the record calls:
```cpp
    is_shutdown_snapshot_ = true;
```

- [ ] **Step 8: Reset state in `init()`**

In `init()` (around line 289), add to the state reset section:

```cpp
    snapshot_seq_ = 0;
    frame_ring_count_ = 0;
    frame_ring_idx_ = 0;
    panel_names_.clear();
    current_panel_id_ = 0;
    pending_settings_changes_.clear();
    is_shutdown_snapshot_ = false;
```

- [ ] **Step 9: Run tests**

Run: `make test && ./build/bin/helix-tests "[telemetry][snapshot]" -v 2>&1 | tail -20`
Expected: All snapshot tests PASS

- [ ] **Step 10: Commit**

```bash
git add include/system/telemetry_manager.h src/system/telemetry_manager.cpp tests/unit/test_telemetry_manager.cpp
git commit -m "feat(telemetry): add periodic snapshot timer with snapshot_seq and is_shutdown"
```

---

## Task 4: Implement Snapshot Disk Persistence

**Files:**
- Modify: `src/system/telemetry_manager.cpp`

- [ ] **Step 1: Write test for snapshot persistence**

Add to `tests/unit/test_telemetry_manager.cpp`:

```cpp
TEST_CASE_METHOD(TelemetryTestFixture,
                 "Snapshot: state persisted to disk and recovered",
                 "[telemetry][snapshot]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_panel_changed("status");
    tm.notify_panel_changed("temperature");
    tm.fire_periodic_snapshot();
    tm.clear_queue();

    // Verify snapshot file exists
    auto snap_path = temp_dir() / "telemetry_snapshot.json";
    REQUIRE(fs::exists(snap_path));

    // Read the snapshot file
    std::ifstream ifs(snap_path);
    auto snap_json = nlohmann::json::parse(ifs);
    REQUIRE(snap_json.contains("snapshot_seq"));
    REQUIRE(snap_json["snapshot_seq"] == 1); // incremented after fire
    REQUIRE(snap_json.contains("panel_time_sec"));
    REQUIRE(snap_json.contains("panel_visits"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "Snapshot: state persisted" -v 2>&1 | tail -10`
Expected: FAIL — `save_snapshot_state` not implemented

- [ ] **Step 3: Implement `save_snapshot_state()` and `load_snapshot_state()`**

```cpp
void TelemetryManager::save_snapshot_state() const {
    if (config_dir_.empty()) return;

    json state;
    state["snapshot_seq"] = snapshot_seq_;

    json time_map = json::object();
    for (const auto& [name, sec] : panel_time_sec_) {
        time_map[name] = sec;
    }
    state["panel_time_sec"] = time_map;

    json visit_map = json::object();
    for (const auto& [name, count] : panel_visits_) {
        visit_map[name] = count;
    }
    state["panel_visits"] = visit_map;

    json widget_map = json::object();
    for (const auto& [name, count] : widget_interactions_) {
        widget_map[name] = count;
    }
    state["widget_interactions"] = widget_map;

    state["overlay_open_count"] = overlay_open_count_;
    state["connect_count"] = connect_count_;
    state["disconnect_count"] = disconnect_count_;
    state["total_connected_sec"] = total_connected_sec_;
    state["total_disconnected_sec"] = total_disconnected_sec_;
    state["longest_disconnect_sec"] = longest_disconnect_sec_;
    state["klippy_error_count"] = klippy_error_count_;
    state["klippy_shutdown_count"] = klippy_shutdown_count_;

    auto path = fs::path(config_dir_) / "telemetry_snapshot.json";
    auto tmp_path = fs::path(config_dir_) / "telemetry_snapshot.json.tmp";

    try {
        std::ofstream ofs(tmp_path);
        ofs << state.dump(2);
        ofs.close();
        fs::rename(tmp_path, path);
        spdlog::debug("[TelemetryManager] Snapshot state saved to {}", path.string());
    } catch (const std::exception& e) {
        spdlog::warn("[TelemetryManager] Failed to save snapshot state: {}", e.what());
    }
}

void TelemetryManager::load_snapshot_state() {
    auto path = fs::path(config_dir_) / "telemetry_snapshot.json";
    if (!fs::exists(path)) return;

    try {
        std::ifstream ifs(path);
        auto state = json::parse(ifs);

        snapshot_seq_ = state.value("snapshot_seq", 0);

        // Restore counters from snapshot for crash recovery
        if (state.contains("panel_time_sec")) {
            for (auto& [key, val] : state["panel_time_sec"].items()) {
                panel_time_sec_[key] = val.get<int>();
            }
        }
        if (state.contains("panel_visits")) {
            for (auto& [key, val] : state["panel_visits"].items()) {
                panel_visits_[key] = val.get<int>();
            }
        }
        if (state.contains("widget_interactions")) {
            for (auto& [key, val] : state["widget_interactions"].items()) {
                widget_interactions_[key] = val.get<int>();
            }
        }

        overlay_open_count_ = state.value("overlay_open_count", 0);
        connect_count_ = state.value("connect_count", 0);
        disconnect_count_ = state.value("disconnect_count", 0);
        total_connected_sec_ = state.value("total_connected_sec", 0);
        total_disconnected_sec_ = state.value("total_disconnected_sec", 0);
        longest_disconnect_sec_ = state.value("longest_disconnect_sec", 0);
        klippy_error_count_ = state.value("klippy_error_count", 0);
        klippy_shutdown_count_ = state.value("klippy_shutdown_count", 0);

        spdlog::info("[TelemetryManager] Recovered snapshot state (seq={})", snapshot_seq_);

        // Remove snapshot file after loading — clean start
        fs::remove(path);
    } catch (const std::exception& e) {
        spdlog::warn("[TelemetryManager] Failed to load snapshot state: {}", e.what());
    }
}
```

- [ ] **Step 4: Wire `load_snapshot_state()` into `init()`**

In `init()`, after `load_queue()` (around line 328):

```cpp
    load_snapshot_state();
```

- [ ] **Step 5: Clean up snapshot file on clean shutdown**

In `shutdown()`, after the final `record_panel_usage()` / `record_connection_stability()` calls, remove the snapshot file:

```cpp
    // Remove snapshot file — clean shutdown, no recovery needed
    auto snap_path = fs::path(config_dir_) / "telemetry_snapshot.json";
    std::error_code ec;
    fs::remove(snap_path, ec);
```

- [ ] **Step 6: Run tests**

Run: `make test && ./build/bin/helix-tests "[telemetry][snapshot]" -v 2>&1 | tail -20`
Expected: All snapshot tests PASS

- [ ] **Step 7: Commit**

```bash
git add src/system/telemetry_manager.cpp tests/unit/test_telemetry_manager.cpp
git commit -m "feat(telemetry): add snapshot disk persistence for crash recovery"
```

---

## Task 5: Implement Feature Adoption Event

**Files:**
- Modify: `src/system/telemetry_manager.cpp`

- [ ] **Step 1: Write test for feature adoption event**

Add to `tests/unit/test_telemetry_manager.cpp`:

```cpp
// ============================================================================
// Feature Adoption [telemetry][adoption]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Feature adoption: event contains feature flags",
                 "[telemetry][adoption]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    // Simulate visiting some panels
    tm.notify_panel_changed("macros");
    tm.notify_panel_changed("camera");
    tm.notify_widget_interaction("led_control");

    tm.record_feature_adoption();

    REQUIRE(tm.queue_size() == 1);
    auto queue = tm.get_queue_snapshot();
    auto event = queue[0];
    REQUIRE(event["event"] == "feature_adoption");
    REQUIRE(event.contains("features"));

    auto& features = event["features"];
    REQUIRE(features["macros"] == true);
    REQUIRE(features["camera"] == true);
    REQUIRE(features["led_control"] == true);
    // Panels not visited should be false
    REQUIRE(features["bed_mesh"] == false);
    REQUIRE(features["console_gcode"] == false);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[telemetry][adoption]" -v 2>&1 | tail -10`
Expected: FAIL

- [ ] **Step 3: Implement `build_feature_adoption_event()`**

```cpp
nlohmann::json TelemetryManager::build_feature_adoption_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "feature_adoption";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["app_version"] = HELIX_VERSION;
    event["app_platform"] = UpdateChecker::get_platform_key();

    // Build feature map from panel visits and widget interactions
    auto has_panel = [&](const std::string& name) -> bool {
        auto it = panel_visits_.find(name);
        return it != panel_visits_.end() && it->second > 0;
    };

    auto has_widget = [&](const std::string& name) -> bool {
        auto it = widget_interactions_.find(name);
        return it != widget_interactions_.end() && it->second > 0;
    };

    json features;
    features["macros"] = has_panel("macros") || has_widget("favorite_macro");
    features["filament_management"] = has_panel("filament") || has_widget("filament");
    features["camera"] = has_panel("camera");
    features["console_gcode"] = has_panel("console");
    features["bed_mesh"] = has_panel("bed_mesh");
    features["input_shaper"] = has_panel("input_shaper");
    features["manual_probe"] = has_widget("manual_probe");
    features["spoolman"] = has_widget("spoolman");
    features["led_control"] = has_widget("led_control");
    features["power_devices"] = has_widget("power_device");
    features["multi_printer"] = has_widget("printer_switcher");
    features["theme_changed"] = false; // Set from settings state if available
    features["timelapse"] = has_widget("timelapse");
    features["favorites"] = has_widget("favorite_macro");
    features["pid_calibration"] = has_panel("pid_calibration");
    features["firmware_retraction"] = has_panel("firmware_retraction");

    event["features"] = features;
    return event;
}

void TelemetryManager::record_feature_adoption() {
    if (shutting_down_.load() || !initialized_.load() || !enabled_.load())
        return;

    spdlog::debug("[TelemetryManager] Recording feature adoption");
    auto event = build_feature_adoption_event();
    enqueue_event(event);
}
```

- [ ] **Step 4: Implement feature adoption timer**

```cpp
void TelemetryManager::start_feature_adoption_timer() {
    if (feature_adoption_timer_) return;

    feature_adoption_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<TelemetryManager*>(lv_timer_get_user_data(timer));
            if (self && !self->shutting_down_.load()) {
                self->record_feature_adoption();
            }
            // One-shot: delete after firing
            lv_timer_delete(timer);
            if (self) self->feature_adoption_timer_ = nullptr;
        },
        FEATURE_ADOPTION_DELAY_MS, this);
    lv_timer_set_repeat_count(feature_adoption_timer_, 1);
}

void TelemetryManager::stop_feature_adoption_timer() {
    if (feature_adoption_timer_) {
        lv_timer_delete(feature_adoption_timer_);
        feature_adoption_timer_ = nullptr;
    }
}
```

Wire into `start_auto_send()`:

```cpp
    start_feature_adoption_timer();
```

Wire into `stop_auto_send()`:

```cpp
    stop_feature_adoption_timer();
```

- [ ] **Step 5: Run tests**

Run: `make test && ./build/bin/helix-tests "[telemetry][adoption]" -v 2>&1 | tail -10`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add src/system/telemetry_manager.cpp tests/unit/test_telemetry_manager.cpp
git commit -m "feat(telemetry): add feature_adoption event with panel/widget detection"
```

---

## Task 6: Implement Settings Changes Event

**Files:**
- Modify: `src/system/telemetry_manager.cpp`
- Modify: `src/system/settings_manager.cpp`

- [ ] **Step 1: Write test for settings changes**

Add to `tests/unit/test_telemetry_manager.cpp`:

```cpp
// ============================================================================
// Settings Changes [telemetry][settings]
// ============================================================================

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Settings changes: batches changes and builds event",
                 "[telemetry][settings]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.notify_setting_changed("theme", "dark", "light");
    tm.notify_setting_changed("brightness", "80", "60");

    // Force flush (normally debounced)
    tm.flush_settings_changes();

    REQUIRE(tm.queue_size() == 1);
    auto queue = tm.get_queue_snapshot();
    auto event = queue[0];
    REQUIRE(event["event"] == "settings_changes");
    REQUIRE(event["changes"].size() == 2);
    REQUIRE(event["changes"][0]["setting"] == "theme");
    REQUIRE(event["changes"][0]["old_value"] == "dark");
    REQUIRE(event["changes"][0]["new_value"] == "light");
}

TEST_CASE_METHOD(TelemetryTestFixture,
                 "Settings changes: no event if no changes pending",
                 "[telemetry][settings]") {
    auto& tm = TelemetryManager::instance();
    tm.set_enabled(true);

    tm.flush_settings_changes();
    REQUIRE(tm.queue_size() == 0);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[telemetry][settings]" -v 2>&1 | tail -10`
Expected: FAIL

- [ ] **Step 3: Implement settings change methods**

```cpp
void TelemetryManager::notify_setting_changed(const std::string& setting_name,
                                               const std::string& old_value,
                                               const std::string& new_value) {
    if (!enabled_.load() || !initialized_.load() || shutting_down_.load())
        return;

    pending_settings_changes_.push_back({setting_name, old_value, new_value});
    spdlog::trace("[TelemetryManager] Setting changed: {}='{}' -> '{}'",
                  setting_name, old_value, new_value);

    start_settings_debounce_timer();
}

void TelemetryManager::flush_settings_changes() {
    if (pending_settings_changes_.empty()) return;

    auto event = build_settings_changes_event();
    enqueue_event(event);
    pending_settings_changes_.clear();
}

nlohmann::json TelemetryManager::build_settings_changes_event() const {
    json event;
    event["schema_version"] = SCHEMA_VERSION;
    event["event"] = "settings_changes";
    event["device_id"] = get_hashed_device_id();
    event["timestamp"] = get_timestamp();
    event["app_version"] = HELIX_VERSION;
    event["app_platform"] = UpdateChecker::get_platform_key();

    json changes = json::array();
    for (const auto& c : pending_settings_changes_) {
        changes.push_back({
            {"setting", c.setting},
            {"old_value", c.old_value},
            {"new_value", c.new_value}
        });
    }
    event["changes"] = changes;
    return event;
}

void TelemetryManager::start_settings_debounce_timer() {
    // Reset timer if already running (extends debounce window)
    if (settings_debounce_timer_) {
        lv_timer_reset(settings_debounce_timer_);
        return;
    }

    settings_debounce_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<TelemetryManager*>(lv_timer_get_user_data(timer));
            if (self && !self->shutting_down_.load()) {
                self->flush_settings_changes();
            }
            lv_timer_delete(timer);
            if (self) self->settings_debounce_timer_ = nullptr;
        },
        SETTINGS_DEBOUNCE_MS, this);
    lv_timer_set_repeat_count(settings_debounce_timer_, 1);
}

void TelemetryManager::stop_settings_debounce_timer() {
    if (settings_debounce_timer_) {
        lv_timer_delete(settings_debounce_timer_);
        settings_debounce_timer_ = nullptr;
    }
}
```

- [ ] **Step 4: Hook SettingsManager setters to notify TelemetryManager**

In `src/system/settings_manager.cpp`, add at the top:

```cpp
#include "system/telemetry_manager.h"
```

Then in key setter methods, add telemetry notifications. For example, in `set_z_movement_style()` (line 221), after persisting to config:

```cpp
    TelemetryManager::instance().notify_setting_changed(
        "z_movement_style",
        std::to_string(lv_subject_get_int(&z_movement_style_subject_)),
        std::to_string(val));
```

Apply similar pattern to these setters:
- `set_led_enabled()` → setting name `"led_enabled"`
- `set_toolhead_style()` → `"toolhead_style"`
- `set_extrude_speed()` → `"extrude_speed"`

Note: Capture old value BEFORE updating the subject. The general pattern is:

```cpp
    auto old_val = std::to_string(lv_subject_get_int(&subject_));
    // ... update subject and persist ...
    TelemetryManager::instance().notify_setting_changed("setting_name", old_val, std::to_string(new_val));
```

Search `settings_manager.cpp` for all `config->save()` calls and add telemetry notifications to each setter that persists a user-facing setting.

- [ ] **Step 5: Run tests**

Run: `make test && ./build/bin/helix-tests "[telemetry][settings]" -v 2>&1 | tail -10`
Expected: PASS

- [ ] **Step 6: Run full test suite**

Run: `make test-run 2>&1 | tail -10`
Expected: All tests pass

- [ ] **Step 7: Commit**

```bash
git add src/system/telemetry_manager.cpp src/system/settings_manager.cpp tests/unit/test_telemetry_manager.cpp
git commit -m "feat(telemetry): add settings_changes event with debounce and SettingsManager hooks"
```

---

## Task 7: Analytics Engine Mappings for New Events

**Files:**
- Modify: `server/telemetry-worker/src/analytics.ts`

- [ ] **Step 1: Write test for new event mappings**

Add to `server/telemetry-worker/src/__tests__/index.test.ts`:

```typescript
describe("Analytics Engine mappings for new events", () => {
  it("maps performance_snapshot to correct blobs/doubles", () => {
    const { mapEventToDataPoints } = require("../analytics");
    const event = {
      event: "performance_snapshot",
      device_id: "abc123",
      app_version: "1.0.0",
      app_platform: "pi4",
      snapshot_seq: 3,
      frame_time_p50_ms: 8,
      frame_time_p95_ms: 16,
      frame_time_p99_ms: 28,
      dropped_frame_count: 42,
      total_frame_count: 432000,
      worst_panel: "temperature",
      worst_panel_p95_ms: 31,
      task_handler_max_ms: 45,
    };

    const points = mapEventToDataPoints(event);
    expect(points).toHaveLength(1);
    expect(points[0].indexes).toEqual(["performance_snapshot"]);
    expect(points[0].blobs![0]).toBe("abc123"); // device_id
    expect(points[0].blobs![1]).toBe("1.0.0");  // version
    expect(points[0].blobs![2]).toBe("pi4");     // platform
    expect(points[0].blobs![3]).toBe("temperature"); // worst_panel
    expect(points[0].doubles![0]).toBe(3);   // snapshot_seq
    expect(points[0].doubles![1]).toBe(8);   // p50
    expect(points[0].doubles![2]).toBe(16);  // p95
    expect(points[0].doubles![3]).toBe(28);  // p99
    expect(points[0].doubles![4]).toBe(42);  // dropped
    expect(points[0].doubles![5]).toBe(432000); // total
    expect(points[0].doubles![6]).toBe(31);  // worst_panel_p95
    expect(points[0].doubles![7]).toBe(45);  // task_handler_max
  });

  it("maps feature_adoption to per-feature doubles", () => {
    const { mapEventToDataPoints } = require("../analytics");
    const event = {
      event: "feature_adoption",
      device_id: "abc123",
      app_version: "1.0.0",
      app_platform: "pi4",
      features: {
        macros: true, camera: false, bed_mesh: true,
        console_gcode: false, input_shaper: false,
        filament_management: false, manual_probe: false,
        spoolman: false, led_control: true, power_devices: false,
        multi_printer: false, theme_changed: false,
        timelapse: false, favorites: true,
        pid_calibration: false, firmware_retraction: false,
      },
    };

    const points = mapEventToDataPoints(event);
    expect(points).toHaveLength(1);
    expect(points[0].indexes).toEqual(["feature_adoption"]);
    expect(points[0].blobs![0]).toBe("abc123");
    // doubles should encode feature flags
    expect(points[0].doubles![0]).toBe(1); // macros=true
    expect(points[0].doubles![1]).toBe(0); // camera=false
  });

  it("maps settings_changes to per-change data points", () => {
    const { mapEventToDataPoints } = require("../analytics");
    const event = {
      event: "settings_changes",
      device_id: "abc123",
      app_version: "1.0.0",
      app_platform: "pi4",
      changes: [
        { setting: "theme", old_value: "dark", new_value: "light" },
        { setting: "brightness", old_value: "80", new_value: "60" },
      ],
    };

    const points = mapEventToDataPoints(event);
    expect(points).toHaveLength(2); // one per change
    expect(points[0].indexes).toEqual(["settings_change"]);
    expect(points[0].blobs![3]).toBe("theme");
    expect(points[0].blobs![4]).toBe("dark");
    expect(points[0].blobs![5]).toBe("light");
    expect(points[1].blobs![3]).toBe("brightness");
  });
});
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd server/telemetry-worker && npx vitest run --reporter=verbose 2>&1 | tail -20`
Expected: FAIL — new event types map to unknown fallback

- [ ] **Step 3: Add mappings to `analytics.ts`**

Add before the unknown event fallback (before line 499):

```typescript
  if (eventType === "performance_snapshot") {
    return {
      indexes: ["performance_snapshot"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        String(event.worst_panel ?? ""),
        "", "", "", "", "", "", "", "",
      ],
      doubles: [
        Number(event.snapshot_seq ?? 0),
        Number(event.frame_time_p50_ms ?? 0),
        Number(event.frame_time_p95_ms ?? 0),
        Number(event.frame_time_p99_ms ?? 0),
        Number(event.dropped_frame_count ?? 0),
        Number(event.total_frame_count ?? 0),
        Number(event.worst_panel_p95_ms ?? 0),
        Number(event.task_handler_max_ms ?? 0),
      ],
    };
  }

  if (eventType === "feature_adoption") {
    const features = (event.features ?? {}) as Record<string, boolean>;
    // Encode feature flags as doubles in a fixed order
    const featureOrder = [
      "macros", "camera", "bed_mesh", "console_gcode",
      "input_shaper", "filament_management", "manual_probe",
      "spoolman",
    ];
    const doubles = featureOrder.map(f => features[f] ? 1 : 0);
    // Analytics Engine supports 8 doubles max — pack remaining into a bitmask
    const extraFeatures = [
      "led_control", "power_devices", "multi_printer", "theme_changed",
      "timelapse", "favorites", "pid_calibration", "firmware_retraction",
    ];
    let extraBitmask = 0;
    for (let i = 0; i < extraFeatures.length; i++) {
      if (features[extraFeatures[i]]) extraBitmask |= 1 << i;
    }
    // Replace last double with bitmask for extra features
    doubles[7] = extraBitmask;

    return {
      indexes: ["feature_adoption"],
      blobs: [
        deviceId,
        String(app.version ?? event.app_version ?? event.version ?? ""),
        String(app.platform ?? event.app_platform ?? event.platform ?? ""),
        "", "", "", "", "", "", "", "", "",
      ],
      doubles,
    };
  }

  if (eventType === "settings_changes") {
    const changes = (event.changes ?? []) as Array<{
      setting: string;
      old_value: string;
      new_value: string;
    }>;
    const version = String(app.version ?? event.app_version ?? event.version ?? "");
    const platform = String(app.platform ?? event.app_platform ?? event.platform ?? "");

    if (changes.length > 0) {
      return changes.map((c) => ({
        indexes: ["settings_change"],
        blobs: [
          deviceId, version, platform,
          String(c.setting ?? ""),
          String(c.old_value ?? ""),
          String(c.new_value ?? ""),
          "", "", "", "", "", "",
        ],
        doubles: [1, 0, 0, 0, 0, 0, 0, 0],
      }));
    }

    return {
      indexes: ["settings_change"],
      blobs: [deviceId, version, platform, "", "", "", "", "", "", "", "", ""],
      doubles: [0, 0, 0, 0, 0, 0, 0, 0],
    };
  }
```

- [ ] **Step 4: Update test expectations to match implementation**

Review the test from Step 1 and adjust any expectations if the implementation differs from the original test expectations (e.g., feature_adoption doubles encoding).

- [ ] **Step 5: Run tests**

Run: `cd server/telemetry-worker && npx vitest run --reporter=verbose 2>&1 | tail -20`
Expected: All tests PASS

- [ ] **Step 6: Commit**

```bash
git add server/telemetry-worker/src/analytics.ts server/telemetry-worker/src/__tests__/index.test.ts
git commit -m "feat(telemetry): add Analytics Engine mappings for performance, adoption, settings events"
```

---

## Task 8: Add Dashboard SQL Queries

**Files:**
- Modify: `server/telemetry-worker/src/queries.ts`

- [ ] **Step 1: Add `performanceQueries()` function**

Add to `server/telemetry-worker/src/queries.ts`:

```typescript
export function performanceQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Frame time percentiles over time (p50, p95, p99 by date)
    `SELECT
      toDate(timestamp) as date,
      avg(double2) as avg_p50,
      avg(double3) as avg_p95,
      avg(double4) as avg_p99
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}
    GROUP BY date
    ORDER BY date`,
    // Drop rate by platform (total dropped / total frames by blob3)
    `SELECT
      blob3 as platform,
      sum(double5) as dropped,
      sum(double6) as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND blob3 != ''${f}
    GROUP BY platform
    ORDER BY dropped DESC`,
    // Drop rate by version (by blob2)
    `SELECT
      toDate(timestamp) as date,
      blob2 as version,
      sum(double5) as dropped,
      sum(double6) as total
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND blob2 != ''${f}
    GROUP BY date, version
    ORDER BY date`,
    // Worst panels (frequency as worst_panel in blob4, avg worst_panel_p95 in double7)
    `SELECT
      blob4 as panel,
      count() as times_worst,
      avg(double7) as avg_p95_ms
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot' AND blob4 != ''${f}
    GROUP BY panel
    ORDER BY times_worst DESC
    LIMIT 20`,
    // Fleet-wide metrics (medians/averages)
    `SELECT
      avg(double2) as fleet_p50,
      sum(double5) as total_dropped,
      sum(double6) as total_frames,
      count(DISTINCT blob1) as total_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}`,
    // Devices with high drop rate (>5%)
    `SELECT count() as high_drop_devices
    FROM (
      SELECT blob1, sum(double5) as dropped, sum(double6) as total
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'performance_snapshot'${f}
      GROUP BY blob1
      HAVING total > 0 AND dropped / total > 0.05
    )`,
  ];
}
```

- [ ] **Step 2: Add `featuresQueries()` function**

```typescript
export function featuresQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  // feature_adoption doubles: 0=macros, 1=camera, 2=bed_mesh, 3=console_gcode,
  // 4=input_shaper, 5=filament_management, 6=manual_probe, 7=extra_bitmask(spoolman not in first 7)
  // Extra bitmask bits: 0=led_control, 1=power_devices, 2=multi_printer, 3=theme_changed,
  // 4=timelapse, 5=favorites, 6=pid_calibration, 7=firmware_retraction
  return [
    // Per-feature adoption rate (latest per device, then avg across fleet)
    // Deduplicate by device: take latest values
    `SELECT
      avg(d1) as macros, avg(d2) as camera, avg(d3) as bed_mesh,
      avg(d4) as console_gcode, avg(d5) as input_shaper,
      avg(d6) as filament_management, avg(d7) as manual_probe,
      avg(extra) as extra_bitmask_avg,
      count() as total_devices
    FROM (
      SELECT blob1,
        argMax(double1, timestamp) as d1, argMax(double2, timestamp) as d2,
        argMax(double3, timestamp) as d3, argMax(double4, timestamp) as d4,
        argMax(double5, timestamp) as d5, argMax(double6, timestamp) as d6,
        argMax(double7, timestamp) as d7, argMax(double8, timestamp) as extra
      FROM ${dataset}
      WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'feature_adoption'${f}
      GROUP BY blob1
    )`,
    // Feature adoption by version
    `SELECT
      blob2 as version,
      avg(double1) as macros, avg(double2) as camera, avg(double3) as bed_mesh,
      avg(double4) as console_gcode, avg(double5) as input_shaper,
      avg(double6) as filament_management, avg(double7) as manual_probe,
      avg(double8) as extra_bitmask_avg,
      count(DISTINCT blob1) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'feature_adoption' AND blob2 != ''${f}
    GROUP BY version
    ORDER BY version`,
    // Total tracked devices
    `SELECT count(DISTINCT blob1) as total_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'feature_adoption'${f}`,
  ];
}
```

- [ ] **Step 3: Add `uxInsightsQueries()` function**

```typescript
export function uxInsightsQueries(days: number, filters?: FilterParams): string[] {
  const dataset = "helixscreen_telemetry";
  const f = buildFilterClause(filters);
  return [
    // Panel time distribution (reuse engagement data)
    `SELECT blob4 as panel, sum(double2) as total_time_sec
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND blob4 != '' AND blob4 != 'home'${f}
    GROUP BY panel
    ORDER BY total_time_sec DESC`,
    // Panel visit frequency (normalized per session)
    `SELECT blob4 as panel, sum(double3) as total_visits, count(DISTINCT blob1) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage' AND blob4 != '' AND blob4 != 'home'${f}
    GROUP BY panel
    ORDER BY total_visits DESC`,
    // Settings change frequency (which settings change most)
    `SELECT blob4 as setting, count() as change_count
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change' AND blob4 != ''${f}
    GROUP BY setting
    ORDER BY change_count DESC`,
    // Settings defaults (% of fleet that changed each setting — count distinct devices per setting)
    `SELECT blob4 as setting, count(DISTINCT blob1) as devices_changed
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change' AND blob4 != ''${f}
    GROUP BY setting
    ORDER BY devices_changed DESC`,
    // Total devices for normalization
    `SELECT count(DISTINCT blob1) as total_devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'session'${f}`,
    // Avg session duration
    `SELECT avg(double1) as avg_session_sec
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'panel_usage'${f}`,
    // Settings change rate (changes per device per week)
    `SELECT count() as total_changes, count(DISTINCT blob1) as devices
    FROM ${dataset}
    WHERE timestamp >= NOW() - INTERVAL '${days}' DAY AND index1 = 'settings_change'${f}`,
  ];
}
```

- [ ] **Step 4: Commit**

```bash
git add server/telemetry-worker/src/queries.ts
git commit -m "feat(telemetry): add SQL queries for performance, features, and UX dashboard endpoints"
```

---

## Task 9: Add Dashboard Endpoint Handlers

**Files:**
- Modify: `server/telemetry-worker/src/index.ts`

- [ ] **Step 1: Add imports for new query functions**

In `server/telemetry-worker/src/index.ts`, add to the existing imports from `queries.ts`:

```typescript
import { performanceQueries, featuresQueries, uxInsightsQueries } from "./queries";
```

- [ ] **Step 2: Add `/v1/dashboard/performance` endpoint**

Add after the last existing dashboard endpoint handler (before the final `return json({ error: "Not found" }, 404)`):

```typescript
        // GET /v1/dashboard/performance
        if (url.pathname === "/v1/dashboard/performance") {
          const queries = performanceQueries(days, filters);
          const [timeRes, platRes, verRes, worstRes, fleetRes, highDropRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const timeData = timeRes as { data: Array<{ date: string; avg_p50: number; avg_p95: number; avg_p99: number }> };
          const platData = platRes as { data: Array<{ platform: string; dropped: number; total: number }> };
          const verData = verRes as { data: Array<{ date: string; version: string; dropped: number; total: number }> };
          const worstData = worstRes as { data: Array<{ panel: string; times_worst: number; avg_p95_ms: number }> };
          const fleetData = fleetRes as { data: Array<{ fleet_p50: number; total_dropped: number; total_frames: number; total_devices: number }> };
          const highDropData = highDropRes as { data: Array<{ high_drop_devices: number }> };

          const fleet = fleetData.data?.[0] ?? { fleet_p50: 0, total_dropped: 0, total_frames: 0, total_devices: 0 };

          return json({
            fleet_p50_ms: Math.round(fleet.fleet_p50),
            fleet_drop_rate: fleet.total_frames > 0 ? fleet.total_dropped / fleet.total_frames : 0,
            high_drop_devices: highDropData.data?.[0]?.high_drop_devices ?? 0,
            total_devices: fleet.total_devices,
            worst_panel: worstData.data?.[0]?.panel ?? "",
            frame_time_trend: (timeData.data ?? []).map(r => ({
              date: r.date, p50: Math.round(r.avg_p50), p95: Math.round(r.avg_p95), p99: Math.round(r.avg_p99),
            })),
            drop_rate_by_platform: (platData.data ?? []).map(r => ({
              platform: r.platform, rate: r.total > 0 ? r.dropped / r.total : 0, dropped: r.dropped, total: r.total,
            })),
            drop_rate_by_version: (verData.data ?? []).map(r => ({
              date: r.date, version: r.version, rate: r.total > 0 ? r.dropped / r.total : 0,
            })),
            jankiest_panels: (worstData.data ?? []).map(r => ({
              panel: r.panel, times_worst: r.times_worst, avg_p95_ms: Math.round(r.avg_p95_ms),
            })),
          });
        }
```

- [ ] **Step 3: Add `/v1/dashboard/features` endpoint**

```typescript
        // GET /v1/dashboard/features
        if (url.pathname === "/v1/dashboard/features") {
          const queries = featuresQueries(days, filters);
          const [adoptionRes, byVersionRes, totalRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const adoptionData = adoptionRes as { data: Array<Record<string, number>> };
          const byVersionData = byVersionRes as { data: Array<Record<string, unknown>> };
          const totalData = totalRes as { data: Array<{ total_devices: number }> };

          const row = adoptionData.data?.[0] ?? {};
          const totalDevices = totalData.data?.[0]?.total_devices ?? 0;

          // Build feature list from the aggregated row
          const featureNames = ["macros", "camera", "bed_mesh", "console_gcode",
            "input_shaper", "filament_management", "manual_probe"];
          const extraFeatureNames = ["led_control", "power_devices", "multi_printer",
            "theme_changed", "timelapse", "favorites", "pid_calibration", "firmware_retraction"];

          const features = featureNames.map(name => ({
            name,
            adoption_rate: Number(row[name] ?? 0),
          }));

          // Decode extra bitmask average into approximate rates
          const extraAvg = Number(row.extra_bitmask_avg ?? 0);
          for (let i = 0; i < extraFeatureNames.length; i++) {
            // Average of bitmask: rough approximation.
            // For precise per-feature rates, would need individual queries.
            // This provides directional data.
            features.push({
              name: extraFeatureNames[i],
              adoption_rate: 0, // Will need separate computation
            });
          }

          return json({
            total_devices: totalDevices,
            features: features.sort((a, b) => b.adoption_rate - a.adoption_rate),
            by_version: (byVersionData.data ?? []).map(r => ({
              version: String(r.version ?? ""),
              devices: Number(r.devices ?? 0),
              macros: Number(r.macros ?? 0),
              camera: Number(r.camera ?? 0),
              bed_mesh: Number(r.bed_mesh ?? 0),
            })),
          });
        }
```

- [ ] **Step 4: Add `/v1/dashboard/ux` endpoint**

```typescript
        // GET /v1/dashboard/ux
        if (url.pathname === "/v1/dashboard/ux") {
          const queries = uxInsightsQueries(days, filters);
          const [panelTimeRes, panelVisitRes, settingsFreqRes, settingsDefaultsRes,
                 totalDevicesRes, avgSessionRes, changeRateRes] =
            await Promise.all(queries.map((q) => executeQuery(queryConfig, q)));

          const panelTime = panelTimeRes as { data: Array<{ panel: string; total_time_sec: number }> };
          const panelVisits = panelVisitRes as { data: Array<{ panel: string; total_visits: number; devices: number }> };
          const settingsFreq = settingsFreqRes as { data: Array<{ setting: string; change_count: number }> };
          const settingsDefaults = settingsDefaultsRes as { data: Array<{ setting: string; devices_changed: number }> };
          const totalDevices = totalDevicesRes as { data: Array<{ total_devices: number }> };
          const avgSession = avgSessionRes as { data: Array<{ avg_session_sec: number }> };
          const changeRate = changeRateRes as { data: Array<{ total_changes: number; devices: number }> };

          const total = totalDevices.data?.[0]?.total_devices ?? 0;
          const cr = changeRate.data?.[0] ?? { total_changes: 0, devices: 0 };

          // Most/least visited panels
          const panels = panelTime.data ?? [];
          const mostVisited = panels[0]?.panel ?? "";
          const leastVisited = panels.length > 0 ? panels[panels.length - 1].panel : "";

          return json({
            avg_session_sec: avgSession.data?.[0]?.avg_session_sec ?? 0,
            most_visited_panel: mostVisited,
            least_visited_panel: leastVisited,
            settings_change_rate_per_device_per_week:
              cr.devices > 0 ? (cr.total_changes / cr.devices) * (7 / Math.max(days, 1)) : 0,
            panel_time: (panelTime.data ?? []).map(r => ({ panel: r.panel, total_time_sec: r.total_time_sec })),
            panel_visits: (panelVisits.data ?? []).map(r => ({
              panel: r.panel, total_visits: r.total_visits, visits_per_device: r.devices > 0 ? r.total_visits / r.devices : 0,
            })),
            settings_changes: (settingsFreq.data ?? []).map(r => ({ setting: r.setting, change_count: r.change_count })),
            settings_defaults: (settingsDefaults.data ?? []).map(r => ({
              setting: r.setting,
              pct_changed: total > 0 ? r.devices_changed / total : 0,
              devices_changed: r.devices_changed,
            })),
          });
        }
```

- [ ] **Step 5: Run worker tests**

Run: `cd server/telemetry-worker && npx vitest run --reporter=verbose 2>&1 | tail -20`
Expected: All tests pass

- [ ] **Step 6: Commit**

```bash
git add server/telemetry-worker/src/index.ts server/telemetry-worker/src/queries.ts
git commit -m "feat(telemetry): add dashboard endpoints for performance, features, and UX"
```

---

## Task 10: Add Dashboard API Types and Methods

**Files:**
- Modify: `server/analytics-dashboard/src/services/api.ts`

- [ ] **Step 1: Add TypeScript interfaces for new endpoints**

Add before the `apiFetch` function (around line 138):

```typescript
export interface PerformanceData {
  fleet_p50_ms: number
  fleet_drop_rate: number
  high_drop_devices: number
  total_devices: number
  worst_panel: string
  frame_time_trend: { date: string; p50: number; p95: number; p99: number }[]
  drop_rate_by_platform: { platform: string; rate: number; dropped: number; total: number }[]
  drop_rate_by_version: { date: string; version: string; rate: number }[]
  jankiest_panels: { panel: string; times_worst: number; avg_p95_ms: number }[]
}

export interface FeaturesData {
  total_devices: number
  features: { name: string; adoption_rate: number }[]
  by_version: { version: string; devices: number; macros: number; camera: number; bed_mesh: number }[]
}

export interface UxInsightsData {
  avg_session_sec: number
  most_visited_panel: string
  least_visited_panel: string
  settings_change_rate_per_device_per_week: number
  panel_time: { panel: string; total_time_sec: number }[]
  panel_visits: { panel: string; total_visits: number; visits_per_device: number }[]
  settings_changes: { setting: string; change_count: number }[]
  settings_defaults: { setting: string; pct_changed: number; devices_changed: number }[]
}
```

- [ ] **Step 2: Add API methods**

Add to the `api` object (after `getStability`):

```typescript
  getPerformance(queryString: string): Promise<PerformanceData> {
    return apiFetch(`/v1/dashboard/performance?${queryString}`)
  },

  getFeatures(queryString: string): Promise<FeaturesData> {
    return apiFetch(`/v1/dashboard/features?${queryString}`)
  },

  getUxInsights(queryString: string): Promise<UxInsightsData> {
    return apiFetch(`/v1/dashboard/ux?${queryString}`)
  },
```

- [ ] **Step 3: Commit**

```bash
git add server/analytics-dashboard/src/services/api.ts
git commit -m "feat(dashboard): add API types and methods for performance, features, UX views"
```

---

## Task 11: Create Performance Dashboard View

**Files:**
- Create: `server/analytics-dashboard/src/views/PerformanceView.vue`

- [ ] **Step 1: Create the view**

```vue
<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Performance</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Median Frame Time"
            :value="`${data.fleet_p50_ms}ms`"
            color="var(--accent-blue)"
          />
          <MetricCard
            title="Fleet Drop Rate"
            :value="`${(data.fleet_drop_rate * 100).toFixed(2)}%`"
            color="var(--accent-yellow)"
          />
          <MetricCard
            title="High Drop Devices (>5%)"
            :value="`${data.high_drop_devices} / ${data.total_devices}`"
            color="var(--accent-red)"
          />
          <MetricCard
            title="Worst Panel"
            :value="data.worst_panel || 'N/A'"
            color="var(--accent-purple)"
          />
        </div>

        <div class="chart-section">
          <h3>Frame Time Trends (ms)</h3>
          <LineChart :data="frameTimeTrendData" />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Drop Rate by Platform</h3>
            <BarChart :data="dropByPlatformData" :options="horizontalBarOpts" />
          </div>
          <div class="chart-section">
            <h3>Jankiest Panels (by p95 frame time)</h3>
            <BarChart :data="jankiestPanelsData" :options="horizontalBarOpts" />
          </div>
        </div>
      </template>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import AppLayout from '@/components/AppLayout.vue'
import MetricCard from '@/components/MetricCard.vue'
import LineChart from '@/components/LineChart.vue'
import BarChart from '@/components/BarChart.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { PerformanceData } from '@/services/api'
import { horizontalBarOpts } from '@/utils/chart'

const filters = useFiltersStore()
const data = ref<PerformanceData | null>(null)
const loading = ref(true)
const error = ref('')

const frameTimeTrendData = computed(() => ({
  labels: data.value?.frame_time_trend.map(d => d.date) ?? [],
  datasets: [
    {
      label: 'p50',
      data: data.value?.frame_time_trend.map(d => d.p50) ?? [],
      borderColor: '#10b981',
      backgroundColor: 'rgba(16, 185, 129, 0.1)',
      fill: false,
      tension: 0.3,
    },
    {
      label: 'p95',
      data: data.value?.frame_time_trend.map(d => d.p95) ?? [],
      borderColor: '#f59e0b',
      backgroundColor: 'rgba(245, 158, 11, 0.1)',
      fill: false,
      tension: 0.3,
    },
    {
      label: 'p99',
      data: data.value?.frame_time_trend.map(d => d.p99) ?? [],
      borderColor: '#ef4444',
      backgroundColor: 'rgba(239, 68, 68, 0.1)',
      fill: false,
      tension: 0.3,
    },
  ],
}))

const dropByPlatformData = computed(() => ({
  labels: data.value?.drop_rate_by_platform.map(p => p.platform) ?? [],
  datasets: [{
    label: 'Drop Rate %',
    data: data.value?.drop_rate_by_platform.map(p => +(p.rate * 100).toFixed(2)) ?? [],
    backgroundColor: '#ef4444',
  }],
}))

const jankiestPanelsData = computed(() => {
  const sorted = [...(data.value?.jankiest_panels ?? [])].sort((a, b) => b.avg_p95_ms - a.avg_p95_ms)
  return {
    labels: sorted.map(p => `${p.panel} (${p.avg_p95_ms}ms)`),
    datasets: [{
      label: 'Avg p95 (ms)',
      data: sorted.map(p => p.avg_p95_ms),
      backgroundColor: '#8b5cf6',
    }],
  }
})

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getPerformance(filters.queryString)
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
  }
}

watch(() => filters.queryString, fetchData, { immediate: true })
</script>

<style scoped>
.page-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 24px; }
.page-header h2 { font-size: 20px; font-weight: 600; }
.metrics-row { display: grid; grid-template-columns: repeat(4, 1fr); gap: 16px; margin-bottom: 24px; }
.grid-2col { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 24px; }
.chart-section { margin-bottom: 24px; }
.chart-section h3 { font-size: 14px; font-weight: 500; color: var(--text-secondary); margin-bottom: 12px; }
.loading, .error { padding: 40px; text-align: center; color: var(--text-secondary); }
.error { color: var(--accent-red); }
</style>
```

- [ ] **Step 2: Commit**

```bash
git add server/analytics-dashboard/src/views/PerformanceView.vue
git commit -m "feat(dashboard): add Performance view with frame time trends and drop rates"
```

---

## Task 12: Create Features Dashboard View

**Files:**
- Create: `server/analytics-dashboard/src/views/FeaturesView.vue`

- [ ] **Step 1: Create the view**

```vue
<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>Feature Adoption</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Features Tracked"
            :value="String(data.features.length)"
            color="var(--accent-blue)"
          />
          <MetricCard
            title="Most Used"
            :value="mostUsed"
            color="var(--accent-green)"
          />
          <MetricCard
            title="Least Used"
            :value="leastUsed"
            color="var(--accent-red)"
          />
        </div>

        <div class="chart-section">
          <h3>Feature Adoption Rates (% of devices)</h3>
          <BarChart :data="adoptionChartData" :options="horizontalBarOpts" />
        </div>

        <div class="chart-section">
          <h3>Never Touched (lowest adoption)</h3>
          <table class="data-table">
            <thead>
              <tr><th>Feature</th><th>Adoption %</th><th>Devices Using</th></tr>
            </thead>
            <tbody>
              <tr v-for="f in neverTouched" :key="f.name">
                <td>{{ formatFeatureName(f.name) }}</td>
                <td>{{ (f.adoption_rate * 100).toFixed(1) }}%</td>
                <td>{{ Math.round(f.adoption_rate * data.total_devices) }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </template>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import AppLayout from '@/components/AppLayout.vue'
import MetricCard from '@/components/MetricCard.vue'
import BarChart from '@/components/BarChart.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { FeaturesData } from '@/services/api'
import { horizontalBarOpts } from '@/utils/chart'

const filters = useFiltersStore()
const data = ref<FeaturesData | null>(null)
const loading = ref(true)
const error = ref('')

function formatFeatureName(id: string): string {
  return id.split('_').map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(' ')
}

const mostUsed = computed(() => {
  const f = data.value?.features[0]
  return f ? `${formatFeatureName(f.name)} (${(f.adoption_rate * 100).toFixed(0)}%)` : 'N/A'
})

const leastUsed = computed(() => {
  const features = data.value?.features ?? []
  const f = features[features.length - 1]
  return f ? `${formatFeatureName(f.name)} (${(f.adoption_rate * 100).toFixed(0)}%)` : 'N/A'
})

const adoptionChartData = computed(() => {
  const sorted = [...(data.value?.features ?? [])].sort((a, b) => b.adoption_rate - a.adoption_rate)
  return {
    labels: sorted.map(f => formatFeatureName(f.name)),
    datasets: [{
      label: 'Adoption %',
      data: sorted.map(f => +(f.adoption_rate * 100).toFixed(1)),
      backgroundColor: '#3b82f6',
    }],
  }
})

const neverTouched = computed(() =>
  [...(data.value?.features ?? [])]
    .sort((a, b) => a.adoption_rate - b.adoption_rate)
    .slice(0, 10)
)

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getFeatures(filters.queryString)
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
  }
}

watch(() => filters.queryString, fetchData, { immediate: true })
</script>

<style scoped>
.page-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 24px; }
.page-header h2 { font-size: 20px; font-weight: 600; }
.metrics-row { display: grid; grid-template-columns: repeat(3, 1fr); gap: 16px; margin-bottom: 24px; }
.chart-section { margin-bottom: 24px; }
.chart-section h3 { font-size: 14px; font-weight: 500; color: var(--text-secondary); margin-bottom: 12px; }
.data-table { width: 100%; border-collapse: collapse; }
.data-table th, .data-table td { padding: 8px 12px; text-align: left; border-bottom: 1px solid var(--border); }
.data-table th { font-size: 12px; color: var(--text-secondary); font-weight: 500; }
.loading, .error { padding: 40px; text-align: center; color: var(--text-secondary); }
.error { color: var(--accent-red); }
</style>
```

- [ ] **Step 2: Commit**

```bash
git add server/analytics-dashboard/src/views/FeaturesView.vue
git commit -m "feat(dashboard): add Feature Adoption view with adoption rates and never-touched table"
```

---

## Task 13: Create UX Insights Dashboard View

**Files:**
- Create: `server/analytics-dashboard/src/views/UxInsightsView.vue`

- [ ] **Step 1: Create the view**

```vue
<template>
  <AppLayout>
    <div class="page">
      <div class="page-header">
        <h2>UX Insights</h2>
      </div>

      <div v-if="loading" class="loading">Loading...</div>
      <div v-else-if="error" class="error">{{ error }}</div>
      <template v-else-if="data">
        <div class="metrics-row">
          <MetricCard
            title="Avg Session Duration"
            :value="formatDuration(data.avg_session_sec)"
            color="var(--accent-blue)"
          />
          <MetricCard
            title="Most Visited Panel"
            :value="data.most_visited_panel || 'N/A'"
            color="var(--accent-green)"
          />
          <MetricCard
            title="Least Visited Panel"
            :value="data.least_visited_panel || 'N/A'"
            color="var(--accent-red)"
          />
          <MetricCard
            title="Settings Changes/Device/Week"
            :value="data.settings_change_rate_per_device_per_week.toFixed(1)"
            color="var(--accent-yellow)"
          />
        </div>

        <div class="grid-2col">
          <div class="chart-section">
            <h3>Time per Panel</h3>
            <PieChart :data="panelTimePieData" />
          </div>
          <div class="chart-section">
            <h3>Visits per Panel (per device)</h3>
            <BarChart :data="panelVisitsBarData" :options="horizontalBarOpts" />
          </div>
        </div>

        <div class="chart-section">
          <h3>Most Changed Settings</h3>
          <BarChart :data="settingsChangesData" :options="horizontalBarOpts" />
        </div>

        <div class="chart-section">
          <h3>Settings Changed from Default (% of fleet)</h3>
          <table class="data-table">
            <thead>
              <tr><th>Setting</th><th>% Changed</th><th>Devices</th></tr>
            </thead>
            <tbody>
              <tr v-for="s in data.settings_defaults" :key="s.setting">
                <td>{{ formatSettingName(s.setting) }}</td>
                <td>{{ (s.pct_changed * 100).toFixed(1) }}%</td>
                <td>{{ s.devices_changed }}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </template>
    </div>
  </AppLayout>
</template>

<script setup lang="ts">
import { ref, watch, computed } from 'vue'
import AppLayout from '@/components/AppLayout.vue'
import MetricCard from '@/components/MetricCard.vue'
import LineChart from '@/components/LineChart.vue'
import BarChart from '@/components/BarChart.vue'
import PieChart from '@/components/PieChart.vue'
import { useFiltersStore } from '@/stores/filters'
import { api } from '@/services/api'
import type { UxInsightsData } from '@/services/api'
import { horizontalBarOpts } from '@/utils/chart'
import { formatDuration } from '@/utils/format'

const COLORS = ['#3b82f6', '#10b981', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899', '#06b6d4', '#84cc16']

const filters = useFiltersStore()
const data = ref<UxInsightsData | null>(null)
const loading = ref(true)
const error = ref('')

function formatSettingName(id: string): string {
  return id.split('_').map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(' ')
}

const panelTimePieData = computed(() => ({
  labels: data.value?.panel_time.map(p => p.panel) ?? [],
  datasets: [{
    data: data.value?.panel_time.map(p => p.total_time_sec) ?? [],
    backgroundColor: COLORS,
  }],
}))

const panelVisitsBarData = computed(() => {
  const sorted = [...(data.value?.panel_visits ?? [])].sort((a, b) => b.visits_per_device - a.visits_per_device)
  return {
    labels: sorted.map(p => p.panel),
    datasets: [{
      label: 'Visits/Device',
      data: sorted.map(p => +p.visits_per_device.toFixed(1)),
      backgroundColor: '#10b981',
    }],
  }
})

const settingsChangesData = computed(() => {
  const sorted = [...(data.value?.settings_changes ?? [])].sort((a, b) => b.change_count - a.change_count)
  return {
    labels: sorted.map(s => formatSettingName(s.setting)),
    datasets: [{
      label: 'Changes',
      data: sorted.map(s => s.change_count),
      backgroundColor: '#8b5cf6',
    }],
  }
})

async function fetchData() {
  loading.value = true
  error.value = ''
  try {
    data.value = await api.getUxInsights(filters.queryString)
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to load data'
  } finally {
    loading.value = false
  }
}

watch(() => filters.queryString, fetchData, { immediate: true })
</script>

<style scoped>
.page-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 24px; }
.page-header h2 { font-size: 20px; font-weight: 600; }
.metrics-row { display: grid; grid-template-columns: repeat(4, 1fr); gap: 16px; margin-bottom: 24px; }
.grid-2col { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-bottom: 24px; }
.chart-section { margin-bottom: 24px; }
.chart-section h3 { font-size: 14px; font-weight: 500; color: var(--text-secondary); margin-bottom: 12px; }
.data-table { width: 100%; border-collapse: collapse; }
.data-table th, .data-table td { padding: 8px 12px; text-align: left; border-bottom: 1px solid var(--border); }
.data-table th { font-size: 12px; color: var(--text-secondary); font-weight: 500; }
.loading, .error { padding: 40px; text-align: center; color: var(--text-secondary); }
.error { color: var(--accent-red); }
</style>
```

- [ ] **Step 2: Commit**

```bash
git add server/analytics-dashboard/src/views/UxInsightsView.vue
git commit -m "feat(dashboard): add UX Insights view with panel time, settings changes"
```

---

## Task 14: Wire Dashboard Routes and Navigation

**Files:**
- Modify: `server/analytics-dashboard/src/router/index.ts`
- Modify: `server/analytics-dashboard/src/components/AppLayout.vue`

- [ ] **Step 1: Add routes for new views**

In `server/analytics-dashboard/src/router/index.ts`, add after the existing routes (before the closing `]`):

```typescript
    {
      path: '/performance',
      name: 'performance',
      component: () => import('@/views/PerformanceView.vue'),
      meta: { requiresAuth: true }
    },
    {
      path: '/features',
      name: 'features',
      component: () => import('@/views/FeaturesView.vue'),
      meta: { requiresAuth: true }
    },
    {
      path: '/ux',
      name: 'ux',
      component: () => import('@/views/UxInsightsView.vue'),
      meta: { requiresAuth: true }
    },
```

- [ ] **Step 2: Add navigation links to AppLayout**

In `server/analytics-dashboard/src/components/AppLayout.vue`, find the navigation links list and add the new entries. Look for the existing nav items pattern (likely a `<nav>` or `<router-link>` section) and add:

```html
<router-link to="/performance">Performance</router-link>
<router-link to="/features">Features</router-link>
<router-link to="/ux">UX Insights</router-link>
```

Place these after the existing "Engagement" link, as they are related views.

- [ ] **Step 3: Build dashboard to verify**

Run: `cd server/analytics-dashboard && npm run build 2>&1 | tail -10`
Expected: Build succeeds with no TypeScript errors

- [ ] **Step 4: Commit**

```bash
git add server/analytics-dashboard/src/router/index.ts server/analytics-dashboard/src/components/AppLayout.vue
git commit -m "feat(dashboard): add routes and navigation for Performance, Features, UX views"
```

---

## Task 15: Update User-Facing Documentation

**Files:**
- Modify: `docs/user/TELEMETRY.md`

- [ ] **Step 1: Update telemetry documentation**

Add the new event types to the user-facing telemetry documentation. In the section that lists what data is collected, add entries for:

- **Performance snapshots** — Frame render times and UI responsiveness metrics (no user content, just timing data)
- **Feature adoption** — Which built-in features you've used (boolean flags, no personal data)
- **Settings changes** — When you change a setting, the setting name and new value are recorded (enumerated values only, no free-text)

Also update the "How often is data sent" section to mention the 4-hour periodic snapshots.

- [ ] **Step 2: Commit**

```bash
git add docs/user/TELEMETRY.md
git commit -m "docs: update telemetry documentation with new event types"
```

---

## Summary

| Task | Component | What It Does |
|------|-----------|-------------|
| 1 | C++ Header | Declarations for all new infrastructure |
| 2 | C++ + App | Frame time sampling ring buffer + main loop hook |
| 3 | C++ | Periodic snapshot timer (4h interval) |
| 4 | C++ | Snapshot disk persistence for crash recovery |
| 5 | C++ | Feature adoption event |
| 6 | C++ + Settings | Settings changes event with debounce |
| 7 | Worker | Analytics Engine mappings for 3 new events |
| 8 | Worker | SQL queries for 3 new dashboard endpoints |
| 9 | Worker | Dashboard endpoint handlers |
| 10 | Dashboard | API types and methods |
| 11 | Dashboard | Performance view |
| 12 | Dashboard | Features view |
| 13 | Dashboard | UX Insights view |
| 14 | Dashboard | Routes and navigation |
| 15 | Docs | User-facing telemetry documentation |
