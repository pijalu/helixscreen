# Temperature Graph Dashboard Widget

**Date:** 2026-03-23
**Status:** Approved
**Issue:** N/A

## Summary

A configurable, multi-instance dashboard panel widget that displays temperature sensor data over time as a line chart. Users select which sensors to display (hotend, bed, chamber, auxiliary) with per-sensor color customization. The widget adapts its display features intelligently based on its grid size — from a minimal sparkline at 1x1 to a full chart with live readouts at larger sizes.

## Requirements

- Multi-instance (`temp_graph:1`, `temp_graph:2`, etc.) — users can have multiple graph widgets showing different sensor groups
- Default configuration: hotend + bed enabled, all other discovered sensors listed but disabled
- Per-sensor toggle (enable/disable) and color picker via edit mode modal
- Tap opens the existing `TempGraphOverlay` for full-screen view
- Edit button appears only in home panel edit/arrange mode (standard widget pattern)
- Adaptive display: features progressively revealed as widget grows
- Minimum size 1x1, default 2x2, no maximum

## Architecture

```
TempGraphWidget (PanelWidget subclass)
  ├── owns ui_temp_graph_t* (extended with display mode API)
  ├── per-instance config: {sensors: [{name, enabled, color}]}
  ├── multi-instance: temp_graph:1, temp_graph:2, etc.
  ├── std::vector<SeriesInfo> series_ (per-series state)
  │     └── SeriesInfo { name, series_id, color, ObserverGuard temp_obs,
  │                      ObserverGuard target_obs, SubjectLifetime lifetime,
  │                      bool is_dynamic }
  ├── std::shared_ptr<bool> alive_ (weak_ptr guard for async safety)
  ├── uint32_t generation_ (stale callback guard)
  ├── ObserverGuard connection_observer_ (printer connect/disconnect)
  ├── observes TemperatureHistoryManager for backfill on attach
  ├── observes live temp subjects via ObserverGuard per SeriesInfo
  └── on tap → opens existing TempGraphOverlay (GraphOnly mode)
```

### Widget Registration

In `panel_widget_registry.cpp`:
- `id: "temp_graph"`
- `display_name: "Temperature Graph"`
- `icon: "chart-line"`
- `colspan: 2, rowspan: 2` (default)
- `min_colspan: 1, min_rowspan: 1` (sparkline minimum)
- `max_colspan: 0, max_rowspan: 0` (no maximum)
- `multi_instance: true`

### XML Component

Minimal — a container `<view>` with click handler and connection state gating. The chart itself is created programmatically in C++ via `ui_temp_graph_create()` since LVGL charts cannot be fully configured through XML.

## Display Mode API Extension

New API added to `ui_temp_graph.h` to control which features are visible:

```cpp
enum ui_temp_graph_feature {
    TEMP_GRAPH_LINES        = (1 << 0),  // Always on
    TEMP_GRAPH_TARGET_LINES = (1 << 1),
    TEMP_GRAPH_LEGEND       = (1 << 2),
    TEMP_GRAPH_Y_AXIS       = (1 << 3),
    TEMP_GRAPH_X_AXIS       = (1 << 4),
    TEMP_GRAPH_GRADIENTS    = (1 << 5),
    TEMP_GRAPH_READOUTS     = (1 << 6),  // Live temp values in legend
};

void ui_temp_graph_set_features(ui_temp_graph_t* graph, uint32_t feature_flags);
```

Implementation: axis labels and legend are LVGL objects controlled via `LV_OBJ_FLAG_HIDDEN`. Gradient fill is already conditional via `gradient_opacity` (set to 0 when disabled). Point count adjusted proportionally to width for extended time windows at larger sizes via existing `ui_temp_graph_set_point_count()`.

## Adaptive Sizing

The widget maps its grid size to feature flags in `on_size_changed()`:

| Size | Features |
|------|----------|
| **1x1** | `LINES` only — sparklines fill the cell |
| **2x1** (wide) | + `TARGET_LINES`, `LEGEND`, `X_AXIS` |
| **1x2** (tall) | + `TARGET_LINES`, `LEGEND`, `Y_AXIS` |
| **2x2** (default) | + `TARGET_LINES`, `LEGEND`, `Y_AXIS`, `X_AXIS`, `GRADIENTS` |
| **3x2+** | All features + `READOUTS` (live temp in legend), extended time window |

The key insight: wide widgets get the time axis (horizontal space), tall widgets get the temperature axis (vertical space). Both axes appear at 2x2+.

## Sensor Configuration & Persistence

Per-instance config stored via `save_widget_config()`:

```json
{
  "sensors": [
    {"name": "extruder", "enabled": true, "color": "#FF4444"},
    {"name": "heater_bed", "enabled": true, "color": "#88C0D0"},
    {"name": "temperature_sensor chamber", "enabled": false, "color": "#A3BE8C"}
  ]
}
```

### Default behavior
When no config exists: hotend + bed enabled with default palette colors. All other discovered sensors appended as disabled.

### Sensor discovery
On `attach()`, the widget queries `PrinterTemperatureState` for extruders/bed and `TemperatureSensorManager` for auxiliary sensors. New sensors not in saved config are appended as disabled. Sensors in config that no longer exist are silently skipped.

### Color assignment
Default colors auto-assigned from the existing 8-color palette (same as `ui_overlay_temp_graph`). Users can override per-sensor via the edit modal color picker.

### Edit mode modal
Uses the `Modal` subclass pattern (not imperative context menu). Subclass `Modal`, implement `get_name()` + `component_name()`, override `on_ok()` / `on_cancel()`. Edit button visible only in panel edit/arrange mode. Modal contains:
- Scrollable list of all discovered sensors
- Each row: **color swatch** (tappable, opens palette picker) | **sensor display name** | **on/off toggle**
- `modal_button_row` with OK/Cancel

## Data Flow & Observer Lifecycle

### Per-series state

Each enabled sensor gets a `SeriesInfo` entry:

```cpp
struct SeriesInfo {
    std::string name;           // e.g. "extruder", "heater_bed", "temperature_sensor chamber"
    int series_id;              // ui_temp_graph series handle
    lv_color_t color;
    ObserverGuard temp_obs;     // live temperature observer
    ObserverGuard target_obs;   // target temperature observer (heaters only)
    SubjectLifetime lifetime;   // for dynamic subjects (multi-extruder, aux sensors)
    bool is_dynamic = false;    // true if subject requires lifetime token
};
```

The widget owns `std::vector<SeriesInfo> series_` — this is the primary storage, not separate observer vectors.

### Subject classification

| Source | Static? | Lifetime needed? |
|--------|---------|------------------|
| `get_active_extruder_temp_subject()` | Yes | No |
| `get_bed_temp_subject()` / `get_bed_target_subject()` | Yes | No |
| `get_extruder_temp_subject(name, lt)` (multi-extruder) | **No** | **Yes** |
| `get_extruder_target_subject(name, lt)` (multi-extruder) | **No** | **Yes** |
| `TemperatureSensorManager::get_temp_subject(name, lt)` | **No** | **Yes** |

For single-extruder setups, the widget uses the static `get_active_extruder_temp/target_subject()`. For multi-extruder, it uses the per-name dynamic variants with `SubjectLifetime` stored in `SeriesInfo::lifetime`.

### Async safety

The widget owns `std::shared_ptr<bool> alive_`. All observer lambdas capture `std::weak_ptr<bool> weak_alive = alive_` and check `if (weak.expired()) return;` before accessing widget state. This prevents callbacks from reaching a destroyed widget.

Additionally, `uint32_t generation_` is bumped before any rebuild (config change). Observer lambdas capture the current generation and check `if (gen != self->generation_) return;` to discard stale callbacks from a previous configuration.

### On `attach()`
1. Allocate a new `alive_ = std::make_shared<bool>(true)` (invalidates any weak_ptrs from previous attach cycle)
2. Create `ui_temp_graph_t` via `ui_temp_graph_create()` inside the XML container
3. Set features based on current size
4. For each enabled sensor, create a `SeriesInfo` and add a series via `ui_temp_graph_add_series()`
4. Backfill from `TemperatureHistoryManager::get_samples(heater_name)` via `ui_temp_graph_set_series_data()`
5. Set up live observers per `SeriesInfo`:
   - Observer lambdas capture `weak_alive` + `generation_` for safety
   - Each temp observer pushes one point via `ui_temp_graph_update_series()`
   - Target observers (heaters only) update cursors via `ui_temp_graph_set_series_target()`
   - Dynamic subjects pass their `SeriesInfo::lifetime` token to the observer factory

### On `detach()`
1. Set `*alive_ = false` (invalidates in-flight weak_ptrs)
2. `auto freeze = UpdateQueue::instance().scoped_freeze();` — freeze queue to prevent new callbacks between drain and destroy
3. `UpdateQueue::instance().drain();` — process any pending deferred callbacks
4. `ObserverGuard::reset()` on all series and connection observer (subjects are alive during detach)
5. Clear `series_` vector
6. `ui_temp_graph_destroy()` the chart
7. Null all LVGL pointers

### On `on_size_changed()`
1. Map colspan/rowspan to feature flags
2. Call `ui_temp_graph_set_features()`
3. Adjust point count: default 300 at 2x2, scale to `width_px / 2` capped at 1200 for larger sizes

### On `on_activate()` / `on_deactivate()`
When the owning panel is hidden (`on_deactivate()`), set a `paused_` flag. Observer callbacks check this flag and skip chart updates while offscreen. `on_activate()` clears the flag and does a full backfill to catch up on missed samples.

### On config change (edit modal OK)
1. Bump `generation_`
2. `detach()` + `attach()` cycle to rebuild series list cleanly

### On printer disconnect/reconnect
The widget observes the printer connection state subject. On disconnect, it clears the chart. On reconnect, it re-discovers sensors (some may have appeared/disappeared), rebuilds series via `detach()` + `attach()`, and backfills from history.

### Threading
All observer callbacks are deferred to the UI thread by `observe_int_sync`. No direct `lv_subject_set_*()` calls from background threads.

## Y-Axis Auto-Scaling

The widget uses simplified auto-range based on enabled sensor types:
- **Heaters only** (extruder/bed): range 0–300°C (covers typical printing temps)
- **Sensors only** (MCU, chamber, ambient): range 0–100°C
- **Mixed**: range 0–300°C
- Auto-expands if any value exceeds the ceiling (same threshold logic as `TempGraphOverlay`: expand at 90% of range, shrink at 50%)

At 1x1 sparkline size, auto-scaling is especially important — a flat line is useless.

## Files to Create/Modify

### New files
- `src/ui/panel_widgets/temp_graph_widget.h` — widget class declaration
- `src/ui/panel_widgets/temp_graph_widget.cpp` — widget implementation, factory registration, edit modal
- `ui_xml/components/panel_widget_temp_graph.xml` — minimal XML container
- `tests/unit/test_panel_widget_temp_graph.cpp` — unit tests

### Modified files
- `src/ui/panel_widget_registry.cpp` — add `temp_graph` definition and call `register_temp_graph_widget()`
- `include/ui_temp_graph.h` — add `ui_temp_graph_feature` enum and `ui_temp_graph_set_features()`
- `src/ui/ui_temp_graph.cpp` — implement feature flag visibility controls
- `src/main.cpp` — add `lv_xml_component_register_from_file()` for `panel_widget_temp_graph`

## Testing

### Unit tests (`test_panel_widget_temp_graph.cpp`)
- Widget creation with default config (hotend + bed enabled)
- Widget creation with custom config (specific sensors, custom colors)
- Feature flag mapping: each size produces the correct feature set
- Config persistence: save/load round-trip
- Sensor discovery: new sensors appended as disabled, missing sensors skipped
- Detach/reattach: observers cleaned up, no leaks
- Generation counter: stale callbacks after rebuild are no-ops
- Dynamic subject teardown: sensor disappears while widget attached (subject deinited, no crash)
- Multi-extruder: per-extruder subjects use lifetime tokens
- Pause/resume: observer callbacks skip updates while deactivated
- Reconnect: sensor list re-discovered after disconnect/reconnect cycle

### Test infrastructure
- `LvglUiTestFixture` for LVGL init/deinit
- `MockPrinterState` for temperature subjects
- `UpdateQueue::drain_queue_for_testing()` before assertions

### Manual verification
- Visual verification of adaptive sizing at each tier
- Edit modal sensor toggling and color picking
- Tap → overlay transition
