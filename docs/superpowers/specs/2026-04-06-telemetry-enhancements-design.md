# Telemetry Enhancements: Periodic Snapshots, Performance, Feature Adoption & Dashboard

**Date:** 2026-04-06
**Status:** Draft

## Problem

HelixScreen's current telemetry has three blind spots:

1. **UX friction is invisible.** `panel_usage` fires only at shutdown. Sessions run for days/weeks, so data is rare and lost entirely on crash. We can't answer "where do users spend time?" or "which panels are ignored?"

2. **Performance in the wild is unknown.** No frame-time or render-performance data is collected. We can't identify janky panels, detect regressions across versions, or understand platform-specific performance issues.

3. **Feature adoption is opaque.** Widget tap counts exist but lack context. We can't answer "which features have users *never* tried?" or "which settings do people actually change?"

Additionally, `connection_stability` (WebSocket lifecycle) suffers the same shutdown-only problem as `panel_usage`.

## Goals

- Answer: "Where do users spend time?", "What's ignored?", "Where is the UI janky?"
- Crash-resilient data collection via periodic disk persistence
- New dashboard views that surface actionable insights, not raw data dumps
- Zero new PII — maintain full GDPR compliance

## Non-Goals

- Real-time streaming telemetry
- Per-interaction event logging (tap-by-tap tracking)
- Dashboard redesign of existing views
- Server-side user identification or tracking

---

## Design

### 1. Periodic Snapshot System

#### 1.1 Snapshot Timer

A single 4-hour LVGL timer fires and emits three events as a correlated batch:

- `panel_usage` (existing, modified)
- `connection_stability` (existing, modified)
- `performance_snapshot` (new)

All three share the same `snapshot_seq` (incrementing integer per session) for correlation.

#### 1.2 Crash Resilience

Counters are persisted to `telemetry_snapshot.json` in the config directory at each snapshot interval. On startup, if a snapshot file exists without a corresponding `is_shutdown=true` event, the previous session crashed — load counters and emit a recovery snapshot before starting fresh.

#### 1.3 Modified: `panel_usage` Event

Existing fields unchanged. New fields:

```json
{
  "event": "panel_usage",
  "snapshot_seq": 3,
  "is_shutdown": false,
  "session_duration_sec": 43200,
  "panel_time_sec": { "status": 28800, "temperature": 3600, "settings": 600, ... },
  "panel_visits": { "status": 45, "temperature": 12, "settings": 3, ... },
  "widget_interactions": { "favorite_macro": 8, "temp_preset": 3, ... },
  "overlay_open_count": 15
}
```

- `snapshot_seq`: Incrementing counter per session (0, 1, 2, ...). Allows stitching snapshots and computing deltas.
- `is_shutdown`: `true` only on the final flush during `shutdown()`. Distinguishes clean shutdown from periodic snapshot.
- All counters remain **cumulative within session** — dashboard computes deltas between consecutive snapshots.

#### 1.4 Modified: `connection_stability` Event

Same treatment as `panel_usage`:

```json
{
  "event": "connection_stability",
  "snapshot_seq": 3,
  "is_shutdown": false,
  ...existing fields...
}
```

---

### 2. New Event: `performance_snapshot`

Emitted alongside `panel_usage` at each 4-hour snapshot.

```json
{
  "event": "performance_snapshot",
  "schema_version": 2,
  "device_id": "a3f8c1...",
  "timestamp": "2026-04-06T14:00:00Z",
  "app_version": "1.0.0",
  "app_platform": "pi4",
  "snapshot_seq": 3,
  "frame_time_p50_ms": 8,
  "frame_time_p95_ms": 16,
  "frame_time_p99_ms": 28,
  "dropped_frame_count": 42,
  "total_frame_count": 432000,
  "worst_panel": "temperature",
  "worst_panel_p95_ms": 31,
  "task_handler_max_ms": 45
}
```

#### 2.1 Frame Time Sampling

- Fixed-size ring buffer (1024 entries) in the LVGL render loop
- Each entry: `{frame_time_us, panel_name}` — one `steady_clock::now()` diff per frame
- Overhead: ~8 bytes per entry × 1024 = 8KB, one clock read per frame
- At snapshot time: sort buffer, compute percentiles by index, identify worst panel
- Buffer resets after each snapshot to keep data fresh

#### 2.2 Dropped Frame Threshold

A frame is "dropped" if render time exceeds 33ms (below 30fps). This threshold is appropriate for a touchscreen UI — not a game, but sluggish response is noticeable.

#### 2.3 Worst Panel Detection

Each ring buffer entry is tagged with the current panel name. At snapshot time, group by panel and compute per-panel p95. The panel with the highest p95 is reported as `worst_panel`.

---

### 3. New Event: `feature_adoption`

Emitted once per session, 5 minutes after startup (to allow time for user interaction).

```json
{
  "event": "feature_adoption",
  "schema_version": 2,
  "device_id": "a3f8c1...",
  "timestamp": "2026-04-06T10:05:00Z",
  "app_version": "1.0.0",
  "app_platform": "pi4",
  "features": {
    "macros": true,
    "filament_management": false,
    "camera": true,
    "console_gcode": false,
    "bed_mesh": true,
    "input_shaper": false,
    "manual_probe": false,
    "spoolman": false,
    "led_control": false,
    "power_devices": false,
    "multi_printer": false,
    "theme_changed": true,
    "timelapse": false,
    "favorites": true,
    "pid_calibration": false,
    "firmware_retraction": false
  }
}
```

#### 3.1 Detection Strategy

Most features are detected from existing data — no new instrumentation needed:

| Feature | Detection |
|---------|-----------|
| `macros` | `panel_visits["macros"] > 0` or macro widget tapped |
| `filament_management` | Filament overlay opened or AMS widget tapped |
| `camera` | Camera panel visited |
| `console_gcode` | Console panel visited or gcode sent |
| `bed_mesh` | Bed mesh panel visited |
| `input_shaper` | Input shaper panel visited |
| `spoolman` | Spoolman overlay opened |
| `led_control` | LED widget tapped |
| `power_devices` | Power device widget tapped |
| `multi_printer` | Printer switcher used (>1 printer configured) |
| `theme_changed` | Settings snapshot shows non-default theme |
| `favorites` | Favorite macro widget exists and tapped |
| `pid_calibration` | PID panel visited |
| `timelapse` | Timelapse widget tapped |
| `firmware_retraction` | FW retraction panel visited |
| `manual_probe` | Manual probe overlay opened |

A few features may need explicit `notify_feature_used("feature_name")` calls where panel/widget signals don't exist.

#### 3.2 Dashboard Aggregation

The dashboard computes "never used" by aggregating across sessions per device:
- If a device has sent 10+ `feature_adoption` events and `camera` is always `false`, camera is "never used" for that device.
- Fleet-wide: "X% of devices have never used camera."

---

### 4. New Event: `settings_changes`

Emitted on settings change, debounced with a 30-second window.

```json
{
  "event": "settings_changes",
  "schema_version": 2,
  "device_id": "a3f8c1...",
  "timestamp": "2026-04-06T11:23:00Z",
  "app_version": "1.0.0",
  "app_platform": "pi4",
  "changes": [
    { "setting": "theme", "old_value": "dark", "new_value": "light" },
    { "setting": "brightness", "old_value": "80", "new_value": "60" },
    { "setting": "locale", "old_value": "en", "new_value": "de" }
  ]
}
```

#### 4.1 Scope

Only category-level values are recorded. No free-text, no file paths, no user-entered strings.

Tracked settings:
- `theme`, `brightness`, `screen_timeout`, `locale`, `time_format`
- `animations_enabled`, `sound_enabled`, `sound_volume`
- `temperature_unit`, `distance_unit`
- `widget_placement` (layout preset name)
- `show_eta`, `show_elapsed`, `show_filename`

#### 4.2 Debouncing

When a setting changes, start a 30-second timer. If more settings change within that window, batch them into one event. This prevents spamming during initial setup or settings exploration.

#### 4.3 Relationship to `settings_snapshot`

`settings_snapshot` (existing, once at startup) provides the baseline. `settings_changes` captures transitions. Together they answer: "What's the default? How often do people change it? What do they change it to?"

---

### 5. Worker API Changes

#### 5.1 New Endpoints

| Endpoint | Source Events | Purpose |
|----------|--------------|---------|
| `GET /v1/dashboard/performance?{filters}` | `performance_snapshot` | Frame times, drop rates, worst panels |
| `GET /v1/dashboard/features?{filters}` | `feature_adoption` | Per-feature adoption %, trends |
| `GET /v1/dashboard/ux?{filters}` | `panel_usage` + `settings_changes` | Panel time, settings changes |

All support existing filter params: `range`, `platform`, `version`, `model`.

#### 5.2 Analytics Engine Indexing

New event types indexed into the existing `helixscreen_telemetry` dataset:

**`performance_snapshot`:**
- Blobs: `platform`, `version`, `worst_panel`
- Doubles: `frame_p50`, `frame_p95`, `frame_p99`, `dropped_count`, `total_count`, `task_handler_max`

**`feature_adoption`:**
- Blobs: `platform`, `version`
- Doubles: one per feature flag (1.0 / 0.0)

**`settings_changes`:**
- Blobs: `setting_name`, `old_value`, `new_value`, `platform`, `version`
- Doubles: `change_count` (number of changes in batch)

#### 5.3 Snapshot Delta Computation

For periodic `panel_usage` and `connection_stability` snapshots:
- Worker stores latest `snapshot_seq` per device per session in R2
- Dashboard queries compute deltas between consecutive snapshots for rate-based metrics
- `is_shutdown=true` marks session end — no delta computed after it

---

### 6. Dashboard — New Views

#### 6.1 Performance View (`/performance`)

**Top row — 4 metric cards:**
- Median frame time (fleet-wide p50)
- Fleet drop rate (dropped / total frames %)
- Devices with >5% drop rate (count + %)
- Worst panel (most frequently appears as `worst_panel`)

**Charts:**
- **Frame time trends** — line chart: p50, p95, p99 over time, filterable by platform/version
- **Drop rate by platform** — bar chart comparing platforms
- **Drop rate by version** — line chart showing regressions/improvements across releases
- **Jankiest panels** — horizontal bar chart: panels ranked by fleet-aggregated p95 frame time

#### 6.2 Feature Adoption View (`/features`)

**Top row — 3 metric cards:**
- Total features tracked
- Least-used feature (name + % of devices)
- Most-used feature (name + % of devices)

**Charts:**
- **Feature adoption rates** — horizontal bar chart: each feature with % of devices that have ever used it
- **Adoption by version** — grouped/stacked bars showing how adoption changes across versions
- **"Never touched" table** — features ranked by % of devices with zero interaction, with sparkline trend

#### 6.3 UX Insights View (`/ux`)

**Top row — 4 metric cards:**
- Avg session duration
- Most visited panel
- Least visited panel
- Settings change rate (changes per device per week)

**Charts:**
- **Panel time distribution** — pie chart: where time is spent across panels
- **Panel visit frequency** — bar chart: visits per panel normalized per session
- **Settings changes** — bar chart: which settings get changed most often
- **Settings defaults** — table: what % of fleet has changed each setting from default

---

### 7. Privacy & GDPR

No new PII is introduced:

- **Performance data**: Frame times, panel names — no user content
- **Feature adoption**: Boolean flags for built-in features — no user-generated data
- **Settings changes**: Enumerated category values only — no free-text input
- **All events**: Use existing double-hashed device ID, same opt-in model

Existing exclusions remain: no filenames, paths, IP addresses, usernames, serial numbers, or any personally identifiable information.

---

### 8. Implementation Notes

#### 8.1 Snapshot Timer

- New LVGL timer alongside existing `auto_send_timer_`
- 4-hour interval: `SNAPSHOT_INTERVAL_MS = 4 * 60 * 60 * 1000`
- Fires: `record_panel_usage()`, `record_connection_stability()`, `record_performance_snapshot()`
- All three share `snapshot_seq_` counter (incremented per snapshot)

#### 8.2 Disk Persistence

- `telemetry_snapshot.json` in config dir, written at each snapshot
- Contains: `panel_time_sec`, `panel_visits`, `widget_interactions`, `connection_*` counters, `snapshot_seq`
- On startup: check for orphaned snapshot file → emit recovery event → clear file
- Performance ring buffer is NOT persisted (too large, and percentiles from partial data aren't useful)

#### 8.3 Frame Time Ring Buffer

```cpp
struct FrameSample {
    uint32_t frame_time_us;
    uint16_t panel_id;  // index into panel name table, saves memory
};

std::array<FrameSample, 1024> frame_ring_;
size_t frame_ring_idx_ = 0;
```

- One `steady_clock::now()` call per LVGL render cycle — negligible overhead
- Panel ID lookup avoids storing strings per sample
- At snapshot: copy buffer, sort, compute percentiles, reset

#### 8.4 Feature Adoption Timing

- 5-minute delay after init via LVGL timer
- Reads from existing `panel_visits_` and `widget_interactions_` maps
- No new per-interaction instrumentation for most features
- Explicit `notify_feature_used()` only for features without panel/widget signals

#### 8.5 Settings Change Debounce

- `SettingsManager` calls `TelemetryManager::notify_setting_changed(name, old, new)`
- TelemetryManager holds a `pending_changes_` vector and a 30-second LVGL timer
- Timer fires → emit `settings_changes` event → clear pending vector
- If telemetry is disabled, no-op

---

### 9. File Changes Summary

**Device side (C++):**

| File | Change |
|------|--------|
| `include/system/telemetry_manager.h` | New methods, ring buffer, snapshot fields |
| `src/system/telemetry_manager.cpp` | Snapshot timer, frame sampling, new event builders |
| `src/system/settings_manager.cpp` | Call `notify_setting_changed()` on setting writes |
| `src/application/application.cpp` | Hook frame time sampling into render loop |

**Worker (Cloudflare):**

| File | Change |
|------|--------|
| `server/telemetry-worker/src/index.ts` | Ingest new event types |
| `server/telemetry-worker/src/dashboard.ts` | New `/performance`, `/features`, `/ux` endpoints |

**Dashboard (Vue):**

| File | Change |
|------|--------|
| `server/analytics-dashboard/src/router/index.ts` | New routes |
| `server/analytics-dashboard/src/services/api.ts` | New API types and methods |
| `server/analytics-dashboard/src/views/PerformanceView.vue` | New view |
| `server/analytics-dashboard/src/views/FeaturesView.vue` | New view |
| `server/analytics-dashboard/src/views/UxInsightsView.vue` | New view |
