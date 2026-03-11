# HTLF Mixed Topology + Fixes (Issue #364)

**Date:** 2026-03-10
**Issue:** prestonbrown/helixscreen#364
**Status:** Approved

## Problem

User jimmyjon711 has an Infimech TX with 6 extruders, a Toolchanger unit (3 standalone toolheads), and an HTLF unit (4 lanes: 2 direct + 2 via hub to shared extruder). HelixScreen misrepresents this setup:

1. **Wrong topology**: HTLF classified as PARALLEL (4 independent nozzles) instead of mixed (2 direct + 2 converging through hub to shared nozzle)
2. **Wrong tool labels**: Filament path canvas uses slot index (T0,T1,T2,T3) instead of AFC `map` values (T0,T2,T1,T3)
3. **Badge leak**: Slot number badges from the Toolchanger unit appear in the HTLF detail view at random positions
4. **Confusing empty slots**: Empty slot placeholder (faint circle) doesn't clearly communicate "no spool"

### User's AFC Data

From `/printer/afc/status` endpoint, per-lane data includes a `hub` field:

```json
{
  "lane1": {"hub": "direct", "extruder": "extruder", "map": "T0", ...},
  "lane2": {"hub": "direct", "extruder": "extruder1", "map": "T2", ...},
  "lane3": {"hub": "HTLF_1", "extruder": "extruder2", "map": "T1", ...},
  "lane4": {"hub": "HTLF_1", "extruder": "extruder2", "map": "T3", ...}
}
```

6 physical extruders, 7 filament lanes. lane3+lane4 share extruder2 through the HTLF hub.

## Design

### 1. PathTopology::MIXED enum

**File:** `include/ams_types.h`

Add `MIXED = 3` to the `PathTopology` enum. Update `path_topology_to_string()` to return `"Mixed (Direct + Hub)"`.

### 2. AFC topology classification

**File:** `src/printer/ams_backend_afc.cpp`

The topology derivation at lines 1379-1394 currently classifies "hubs + multiple extruders" as PARALLEL. This is wrong for HTLF where some lanes are direct and others go through a hub.

**Data source:** The per-lane `hub` field is already available in the AFC `/printer/afc/status` response (parsed in `update_lane_data()`). Value is `"direct"` for lanes that go straight to an extruder, or a hub name (e.g., `"HTLF_1"`) for lanes that route through a hub.

**New classification logic (replaces extruder-count heuristic):**

```cpp
// Check per-lane hub field, NOT extruder count
bool has_direct = false, has_hub = false;
for (const auto& lane : unit_info.lanes) {
    if (lane_hub_routing_[lane] == "direct")
        has_direct = true;
    else
        has_hub = true;
}

if (has_direct && has_hub)
    unit_info.topology = PathTopology::MIXED;
else if (has_hub)
    unit_info.topology = PathTopology::HUB;
else if (has_direct && unit_info.extruders.size() > 1)
    unit_info.topology = PathTopology::PARALLEL;
else
    unit_info.topology = PathTopology::HUB; // default
```

**Storage:** Add `std::vector<bool> lane_is_hub_routed` to `AfcUnitInfo` (`include/ams_backend_afc.h`). Populated during unit object parsing from per-lane `hub` field.

**Propagation:** Add `std::vector<bool> lane_is_hub_routed` field to `AmsUnit` struct in `include/ams_types.h` (alongside existing `topology` field at line 678). Copy from `AfcUnitInfo` during `reorganize_slots()`.

### 3. Badge leak fix

**File:** `src/ui/ui_ams_detail.cpp`

**Root cause:** `ams_detail_update_badges()` (line 406) does not clean badge_layer before adding new badges. When switching units in the overview panel, old badges (reparented from previous unit's slots via `lv_obj_set_parent()`) remain as stale children of badge_layer.

**Fix:** Add `lv_obj_clean(w.badge_layer)` at line 410, matching how `ams_detail_update_labels()` (line 388) already cleans labels_layer.

### 4. Per-slot metadata in FilamentPathData

**File:** `src/ui/ui_filament_path_canvas.cpp`

Add to `FilamentPathData` struct (after `slot_has_prep_sensor` at line 154):

```cpp
int mapped_tool[MAX_SLOTS] = {};       // -1 = use slot index, >= 0 = actual tool number
bool slot_is_hub_routed[MAX_SLOTS] = {}; // true = lane routes through hub (for MIXED)
```

Initialize `mapped_tool` elements to -1 in the struct initializer.

**Setter functions** (declared in `src/ui/ui_filament_path_canvas.h`):

```cpp
void ui_filament_path_canvas_set_slot_mapped_tool(lv_obj_t* obj, int slot, int tool);
void ui_filament_path_canvas_set_slot_hub_routed(lv_obj_t* obj, int slot, bool is_hub);
```

Both silently return if `slot < 0 || slot >= MAX_SLOTS`. Invalidate widget after update.

**Plumbing:** In `ams_detail_setup_path_canvas()` (`src/ui/ui_ams_detail.cpp` line 424), after setting topology and slot count, iterate unit slots and call:

```cpp
SlotInfo slot = backend->get_slot_info(global_index);
ui_filament_path_canvas_set_slot_mapped_tool(canvas, i, slot.mapped_tool);
// For hub routing: check unit's lane_is_hub_routed vector
if (unit_index >= 0 && i < info.units[unit_index].lane_is_hub_routed.size()) {
    ui_filament_path_canvas_set_slot_hub_routed(canvas, i, info.units[unit_index].lane_is_hub_routed[i]);
}
```

### 5. Fix parallel topology tool labels

**File:** `src/ui/ui_filament_path_canvas.cpp`

Line 1529: change from:
```cpp
snprintf(tool_label, sizeof(tool_label), "T%d", i);
```
to:
```cpp
int tool = (data->mapped_tool[i] >= 0) ? data->mapped_tool[i] : i;
snprintf(tool_label, sizeof(tool_label), "T%d", tool);
```

This fixes tool labels for ALL parallel topology units, not just MIXED.

### 6. New draw_mixed_topology()

**File:** `src/ui/ui_filament_path_canvas.cpp`

New static function ~150-200 lines, drawing Option B layout:

**Entry points (top):** 4 spool entry points aligned with spool grid, same as `draw_parallel_topology()` using `get_slot_x()`.

**Sensor dots:** One per lane at `SENSOR_Y` ratio, same as parallel.

**Direct lanes** (`slot_is_hub_routed[i] == false`):
- Straight vertical line from sensor to individual nozzle at `TOOLHEAD_Y`
- Uses existing `draw_vertical_line()` / `draw_glow_line()` / `draw_hollow_vertical_line()`

**Hub lanes** (`slot_is_hub_routed[i] == true`):
- From sensor dot, draw S-curve (cubic bezier via `lv_draw_line`) converging to hub box
- Hub box positioned at: X = average of hub lane X positions, Y = `(SENSOR_Y + TOOLHEAD_Y) / 2`
- Hub box size: proportional to canvas (same as HUB topology `draw_hub_box()` but smaller, ~60% width)
- Hub box label: "HUB"
- Single output line from hub bottom to shared nozzle
- Shared nozzle X = hub box center X, Y = `TOOLHEAD_Y`

**Nozzles:** Uses existing `draw_nozzle_bambu()` / `draw_nozzle_faceted()` / `draw_nozzle_a4t()` (selected via `SettingsManager::get_effective_toolhead_style()`). Direct lane nozzles at full opacity if mounted, 40% if docked. Hub nozzle shows the shared extruder.

**Tool labels:** Below each nozzle using `mapped_tool[i]`. Hub lanes' shared nozzle shows the hub tool label (derived from the shared extruder number).

**Dispatch:** Add to `filament_path_draw_cb()` at line 1578:
```cpp
if (data->topology == static_cast<int>(PathTopology::MIXED)) {
    draw_mixed_topology(e, data);
    return;
}
```

### 7. System path canvas (overview) MIXED support

**Files:** `src/ui/ui_system_path_canvas.cpp`, `src/ui/ams_drawing_utils.cpp`

**`compute_system_tool_layout()`** (`ams_drawing_utils.cpp` line 346): For MIXED units, count unique mapped_tool values (not lane count) for `tool_count`. The HTLF has 4 lanes but mapped_tool values {0, 2, 1, 3} which map to 3 unique extruders → `tool_count = 3`.

**Route generation** (`ui_system_path_canvas.cpp` PASS 1, line 683): For MIXED units, generate routes like PARALLEL for direct lanes (independent route per tool) and merge hub lanes to a single shared route. Hub lanes with the same target extruder share a nozzle position — only one route from unit to that nozzle, drawn as a merge.

### 8. New mock mode: htlf_toolchanger_mode

**Files:** `src/printer/ams_backend_mock.cpp`, `include/ams_backend_mock.h`

Add `set_htlf_toolchanger_mode(bool)` / `is_htlf_toolchanger_mode() const` methods. Following existing pattern for mode flags (see `tool_changer_mode_`, `mixed_topology_mode_`).

**Setup:**
- **Unit 0 "Tools"**: 3 lanes, PARALLEL topology
  - extruder3→T4 (ASA Black, loaded), extruder4→T5 (empty), extruder5→T6 (empty)
- **Unit 1 "HTLF_1"**: 4 lanes, MIXED topology
  - lane1→extruder/T0 (direct, ABS White, loaded)
  - lane2→extruder1/T2 (direct, ABS Navy, loaded)
  - lane3→extruder2/T1 (hub, ASA Sparkle Navy, hub-loaded)
  - lane4→extruder2/T3 (hub, empty)
  - `lane_is_hub_routed = {false, false, true, true}`
- 6 total extruders, 7 total lanes
- Hub "HTLF_1" with 2 lanes (lane3, lane4)
- Buffers TN0-TN3

**Environment variable:** `HELIX_MOCK_AMS=htlf_toolchanger` (following existing pattern: `afc`, `toolchanger`, `mixed`, `vivid_mixed`, `ifs`).

**Documentation:** Add to `docs/devel/ENVIRONMENT_VARIABLES.md` in the Mock & Testing section alongside other modes.

### 9. Empty slot visualization

**File:** `src/ui/ui_ams_slot.cpp`

Replace current empty placeholder (lines 643-656, border-only circle at 40% opacity) with:

- **Dashed circle**: Same size as spool, `border_width=2`, `border_color=text_muted`, `border_opa=LV_OPA_60`. Dashed effect via `style_border_dash_width` and `style_border_dash_gap` (4px each).
- **Plus icon**: MDI plus icon (`ICON_PLUS` from `ui_icon_codepoints.h`), 16px, centered in circle, `text_color` at 40% opacity. Created as `lv_label` child of placeholder.
- **"Empty" label**: Set `material_label` text to `lv_tr("Empty")` when slot is unassigned (in `apply_slot_status()`, line 283-298). This reuses the existing label position above the spool.

## Testing

### Unit tests
- **Topology classification**: Parse sample AFC data with mixed hub routing → verify `get_unit_topology()` returns `PathTopology::MIXED`
- **Tool mapping**: Verify `get_slot_info()` returns correct `mapped_tool` values matching AFC `map` field

### Visual tests (mock mode)
1. Start with `HELIX_MOCK_AMS=htlf_toolchanger ./build/bin/helix-screen --test -vv`
2. **Overview**: Verify HTLF unit shows 3 nozzle positions, 2 direct routes + 2 merging routes
3. **HTLF detail**: Verify 4 spools, 3 nozzles, hub box between lanes 3+4, S-curve convergence
4. **Tool labels**: Verify badges show T0, T2, T1, T3 (not T0, T1, T2, T3)
5. **Badge leak**: Switch between Tools and HTLF_1 detail views — no stale badges
6. **Empty slot**: Verify lane4 and extruder4/5 show dashed circle + plus icon + "Empty"

### Regression
- Verify existing PARALLEL, HUB, LINEAR topologies still render correctly
- Run `make test-run` — no existing tests broken
