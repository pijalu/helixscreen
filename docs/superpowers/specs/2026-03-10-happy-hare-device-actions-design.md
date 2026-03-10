# Happy Hare Device Actions & Configuration Parity

**Date:** 2026-03-10
**Status:** Approved

## Problem

The Happy Hare backend's device actions show hardcoded defaults instead of live values. Sliders always display fallback values (e.g., gear_load_speed=150) regardless of actual configuration. Changes sent via G-code work but don't persist across Klipper restarts. The AFC backend recently received full live-value population, config persistence, and dynamic action generation — Happy Hare needs parity.

Additionally, both AFC and HH device action buttons render full-panel-width, wasting vertical space.

## Design

### Data Flow & Value Lifecycle

Three value sources, prioritized:

1. **SettingsManager** (user's last UI change) — highest priority
2. **Moonraker status** (live state for LED/eSpooler/flowguard) — real-time authoritative
3. **configfile.settings.mmu** (HH config file defaults) — startup fallback

```
Startup:
  configfile.settings.mmu.gear_from_spool_speed  →  populate slider default
  printer.mmu.flowguard.encoder_mode             →  populate clog dropdown

User changes slider:
  UI slider release  →  execute_device_action("gear_load_speed", 200.0)
                     →  G-code: MMU_TEST_CONFIG GEAR_FROM_SPOOL_SPEED=200
                     →  SettingsManager: persist {"hh.gear_load_speed": 200.0}
                     →  Update in-memory cache for get_device_actions()

Reconnect after restart:
  on_started()  →  read SettingsManager saved values
               →  re-apply via MMU_TEST_CONFIG commands (batched)
               →  query configfile.settings.mmu for fresh defaults
               →  detect config file edits (drop stale overrides)
```

### Config Edit Detection

SettingsManager stores both the user's value and the config default at time of change:

```
hh.<action_id>.value          = 200.0    // user's slider value
hh.<action_id>.config_default = 150.0    // configfile.settings.mmu value when saved
```

On reconnect, if `config_default` no longer matches what `configfile.settings.mmu` reports, the user edited the config file manually — drop the override and adopt the new default.

### Action Mapping

#### Setup Section

| Action | Type | Read From | G-code | Topology |
|--------|------|-----------|--------|----------|
| Calibrate Bowden | BUTTON | — | `MMU_CALIBRATE_BOWDEN` | Both |
| Calibrate Encoder | BUTTON | — | `MMU_CALIBRATE_ENCODER` | Type A only |
| Calibrate Gear | BUTTON | — | `MMU_CALIBRATE_GEAR` | Both |
| Calibrate Gates | BUTTON | — | `MMU_CALIBRATE_GATES` | Both |
| Calibrate Servo | BUTTON | — | `MMU_SERVO ACTION=calibrate` | Type A only |
| LED Mode | DROPDOWN | `printer.mmu.leds.unit0.exit_effect` (hardcoded to unit0 for now; multi-unit LED is out of scope) | `MMU_LED EXIT_EFFECT={val}` | Both |

#### Speed Section

| Action | Type | Config Key | G-code Param | Range | Step | Topology |
|--------|------|-----------|--------------|-------|------|----------|
| Gear Load Speed | SLIDER | `gear_from_spool_speed` | `GEAR_FROM_SPOOL_SPEED` | 10–300 mm/s | 5 | Both |
| Gear Unload Speed | SLIDER | `gear_unload_speed` | `GEAR_UNLOAD_SPEED` | 10–300 mm/s | 5 | Both |
| Selector Speed | SLIDER | `selector_move_speed` | `SELECTOR_MOVE_SPEED` | 10–300 mm/s | 5 | Type A only |
| Extruder Load Speed | SLIDER | `extruder_load_speed` | `EXTRUDER_LOAD_SPEED` | 10–100 mm/s | 5 | Both |
| Extruder Unload Speed | SLIDER | `extruder_unload_speed` | `EXTRUDER_UNLOAD_SPEED` | 10–100 mm/s | 5 | Both |

All speeds: read initial from `configfile.settings.mmu`, persist in SettingsManager, apply via `MMU_TEST_CONFIG {PARAM}={val}`.

#### Toolhead Section (NEW)

| Action | Type | Config Key | G-code Param | Range | Step |
|--------|------|-----------|--------------|-------|------|
| Sensor to Nozzle | SLIDER | `toolhead_sensor_to_nozzle` | `TOOLHEAD_SENSOR_TO_NOZZLE` | 1–200 mm | 0.5 |
| Extruder to Nozzle | SLIDER | `toolhead_extruder_to_nozzle` | `TOOLHEAD_EXTRUDER_TO_NOZZLE` | 5–200 mm | 0.5 |
| Entry to Extruder | SLIDER | `toolhead_entry_to_extruder` | `TOOLHEAD_ENTRY_TO_EXTRUDER` | 0–200 mm | 0.5 |
| Ooze Reduction | SLIDER | `toolhead_ooze_reduction` | `TOOLHEAD_OOZE_REDUCTION` | -5–20 mm | 0.5 |

Same persistence pattern as speeds. Topology: Both.

#### Accessories Section

| Action | Type | Read From | G-code | Topology |
|--------|------|-----------|--------|----------|
| eSpooler Mode | DROPDOWN | `printer.mmu.espooler_active` | `MMU_ESPOOLER OPERATION={off\|rewind\|assist}` | Both (if has eSpooler) |
| Clog Detection | DROPDOWN | `printer.mmu.flowguard.encoder_mode` (v4) or `printer.mmu.clog_detection` (v3, deprecated) | `MMU_TEST_CONFIG CLOG_DETECTION={0\|1\|2}` (0=Off, 1=Manual, 2=Auto) | Type A only |
| Sync to Extruder | TOGGLE | configfile: `sync_to_extruder` | `MMU_TEST_CONFIG SYNC_TO_EXTRUDER={0\|1}` | Both |

#### Maintenance Section

| Action | Type | G-code | Topology |
|--------|------|--------|----------|
| Test Grip | BUTTON | `MMU_TEST_GRIP` | Both |
| Test Load | BUTTON | `MMU_TEST_LOAD` | Both |
| Test Move | BUTTON | `MMU_TEST_MOVE` | Both |
| Motors | TOGGLE | `MMU_MOTORS_OFF` (disable) / `MMU_HOME` (re-enable) | Both |
| Buzz Servo | BUTTON | `MMU_SERVO` | Type A only |

### Persistence Rules

| Category | Persist? | Reason |
|----------|----------|--------|
| Speeds, toolhead distances | Yes | Not in Moonraker status, lost on restart |
| Clog detection, sync_to_extruder | Yes | `MMU_TEST_CONFIG` overrides are volatile even though config file has defaults |
| LED mode, eSpooler mode | No | Per-session preference, reads from live status |
| Buttons (calibrate, test) | No | One-shot actions |
| Motor toggle | No | Transient state |

### Re-apply Strategy

On reconnect, batch all saved overrides into a single `MMU_TEST_CONFIG` command:

```
MMU_TEST_CONFIG GEAR_FROM_SPOOL_SPEED=200 GEAR_UNLOAD_SPEED=80 SELECTOR_MOVE_SPEED=250
```

### Dynamic Section Filtering

Sections and actions filtered by topology (already detected via `query_selector_type_from_config()`):

- **Type A (ERCF/Tradrack):** Show all sections. Hide eSpooler if not present.
- **Type B (EMU/Box Turtle):** Hide servo-related actions (Calibrate Servo, Buzz Servo). Hide Clog Detection (no encoder). Show eSpooler.

Actions hidden by setting `enabled=false` with `disable_reason` explaining why.

### Button Grid Layout (Shared UI Fix)

Change `AmsDeviceSectionDetailOverlay::create_action_control()` to group consecutive BUTTON actions into a 2-column flex-wrap container instead of one-per-row.

- Buttons: `width=48%` in a `flex_flow=ROW_WRAP` container
- Sliders, dropdowns, toggles: unchanged (full-width)
- Odd button count: last button alone on its row
- Applies to both AFC and HH — backend-agnostic fix

### Startup Query

Extend `on_started()` to query `configfile.settings.mmu` for all action-relevant keys. This uses the same mechanism as the existing `query_tip_method_from_config()` and `query_selector_type_from_config()` calls — a one-time `printer.objects.query` request.

Keys to query: `gear_from_spool_speed`, `gear_unload_speed`, `selector_move_speed`, `extruder_load_speed`, `extruder_unload_speed`, `toolhead_sensor_to_nozzle`, `toolhead_extruder_to_nozzle`, `toolhead_entry_to_extruder`, `toolhead_ooze_reduction`, `sync_to_extruder`, `clog_detection` (v3) / `flowguard_encoder_mode` (v4).

### Moonraker Status Subscriptions

Add to existing subscription list:
- `mmu.leds` — LED effect state
- `mmu.flowguard` — clog detection mode
- `mmu.espooler` — eSpooler per-gate modes

These are parsed in `handle_status_update()` and cached for `get_device_actions()`.

### Error Handling

- **G-code failures:** `execute_device_action()` returns `AmsError` on G-code failure. UI shows toast via existing error propagation.
- **Config query failures:** If `configfile.settings.mmu` query fails, fall back to hardcoded defaults from `hh_defaults.cpp`. Log warning.
- **Re-apply failures:** If batch `MMU_TEST_CONFIG` fails on reconnect, log error but don't block startup. Settings remain in SettingsManager for next attempt.

## Out of Scope

- Config file download/upload manager (HH doesn't need one — `MMU_TEST_CONFIG` handles runtime changes)
- Per-gate eSpooler mode selection (current UI is system-wide; per-gate is a future enhancement)
- Multi-unit action duplication (per-unit speed overrides — future work)

## Files Changed

- `src/printer/ams_backend_happy_hare.cpp` — live value population, execute_device_action, config queries, persistence
- `include/ams_backend_happy_hare.h` — new member variables for cached config values
- `src/printer/hh_defaults.cpp` — expanded sections and actions with new toolhead section
- `include/hh_defaults.h` — updated declarations
- `src/ui/ui_ams_device_section_detail_overlay.cpp` — button grid layout (shared fix)
- `tests/unit/test_ams_backend_happy_hare.cpp` — tests for live values, persistence, config edit detection
- `tests/unit/test_hh_defaults.cpp` — tests for new toolhead section actions (if exists, or add to existing test file)
