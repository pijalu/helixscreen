# AMS Per-Unit Environment Display & Dryer Control

**Date:** 2026-03-25
**Status:** Design approved, pending implementation plan

## Overview

Replace the existing dryer info bar and dryer modal with a unified per-unit environment display system. Every AMS unit that has temperature/humidity sensors shows a compact inline indicator on its unit card. Tapping the indicator opens a full environment overlay with detailed readings, material comfort ranges, and dryer controls (when heaters are available).

## Architecture

Three layers:

1. **Compact Indicator** — small inline row on the unit card
2. **Environment Overlay** — full panel takeover with details + controls
3. **Backend Abstraction** — capability-based environment/dryer support per backend

Data flow: Backend → `AmsUnit.environment` / `SlotInfo.environment` → AmsState per-unit subjects → XML bindings (compact indicator) → overlay (detailed view + controls)

## Layer 1: Compact Indicator

**Component:** `ams_environment_indicator.xml` (new, reusable)

**Location:** Below the slot bars on the unit card. Used in both:
- `ams_unit_detail` (single-unit detail view in `ams_panel.xml`)
- `ams_unit_card` (multi-unit overview cards in `ams_overview_panel.xml`)

**Layout — passive/idle:**
```
[thermometer-icon] 24°C  |  [water-icon] 46%
```

**Layout — drying active:**
```
[heat-wave-icon] 47°C → 55°C  [timer-icon] 2:30 left
```

**States:**
- **Normal:** Static temp + humidity, text color-coded by material thresholds
- **Drying active:** Shows current → target temp + countdown, warning color
- **No sensors:** Row hidden entirely (flex collapses)
- **Error/offline:** Shows `--` for values

**Color thresholds** (based on loaded material, green/yellow/red):
- Green: humidity below material's "good" threshold
- Yellow: humidity in "marginal" range
- Red: humidity above material's "danger" threshold
- Temperature: flag extremes only (< 15°C or > 40°C ambient)

**Interaction:** Entire row is clickable → opens environment overlay. All children have `clickable="false" event_bubble="true"`.

## Layer 2: Environment Overlay

**Trigger:** Tap compact indicator → `ui_nav_push_overlay()`

### Passive System (no heater, e.g. CFS)

```
┌─────────────────────────────────────┐
│ ← Environment    CFS Unit 1        │  Header with back button
├─────────────────────────────────────┤
│                                     │
│   🌡 24°C          💧 46%          │  Large readout, color-coded
│   Ambient Temp     Humidity         │
│                                     │
│   ┌─────────────────────────────┐   │
│   │  Material Comfort Ranges    │   │  Card with thresholds based
│   │  PLA:  ✅ OK (< 50%)       │   │  on loaded materials
│   │  PETG: ⚠️ Marginal (< 40%) │   │
│   │  Nylon: ❌ Too humid (<20%) │   │
│   └─────────────────────────────┘   │
│                                     │
│   No dryer available.               │  Gentle note
│   Store filament in dry box for     │
│   best results.                     │
└─────────────────────────────────────┘
```

### Active Dryer System

```
┌─────────────────────────────────────┐
│ ← Environment    AMS Unit 1        │
├─────────────────────────────────────┤
│                                     │
│   🌡 47°C → 55°C    💧 28%        │  Current → Target, humidity
│   Drying: 2:30 left  ████████░░    │  Progress bar
│                                     │
│   ┌──────────────────────────────┐  │
│   │ 💡 Loaded: PLA (all slots)   │  │  Smart preset detection
│   │ Recommended: 55°C / 4h       │  │
│   │                              │  │
│   │ Preset: [PLA 55°C/4h    ▾]  │  │  Dropdown, auto-selects
│   │                              │  │
│   │ Temperature: [ 55 ]°C       │  │  Editable, populated from
│   │ Duration:    [  4 ]h         │  │  preset selection
│   │                              │  │
│   │ [  Start Drying  ]          │  │  Primary action
│   └──────────────────────────────┘  │
└─────────────────────────────────────┘
```

**Preset dropdown:** `PLA 55°C/4h`, `PETG 65°C/6h`, `ABS 80°C/8h`, `PA/Nylon 70°C/8h`, `TPU 55°C/4h`, `Custom...`

Selecting a preset fills temperature + duration fields. User can manually adjust regardless. Selecting "Custom..." clears the fields for manual entry.

When drying is active: fields become read-only, button changes to `[Stop Drying]`, progress bar + countdown shown.

**Smart presets:** Reads loaded materials from slot data. If all slots have the same material, recommends that preset and pre-selects it. If mixed materials, shows "Mixed — use lowest safe temperature" with the most conservative preset highlighted.

**Per-slot breakdown:** When backend reports `EnvironmentGranularity::SLOT`, shows a mini table with slot letter, material, temp, humidity per row. Falls back to per-unit display for `UNIT` granularity.

## Layer 3: Backend Abstraction

### New capability methods on AmsBackend

```cpp
virtual bool has_environment_sensors() const { return false; }
virtual bool has_dryer() const { return false; }
virtual EnvironmentGranularity environment_granularity() const {
    return EnvironmentGranularity::NONE;
}

// Dryer operations (no-op defaults)
virtual AmsError start_drying(int unit, int target_temp_c, int duration_min) {
    return AmsErrorHelper::not_supported("No dryer");
}
virtual AmsError stop_drying(int unit) {
    return AmsErrorHelper::not_supported("No dryer");
}
```

### New types

```cpp
enum class EnvironmentGranularity { NONE, UNIT, SLOT };

struct MaterialComfortRange {
    std::string material;
    float max_humidity_good;   // Green threshold
    float max_humidity_warn;   // Yellow threshold (above = red)
};
```

`MaterialComfortRange` stored as a small static lookup table (~10 common materials).

### CFS backend overrides
- `has_environment_sensors()` → `true`
- `has_dryer()` → `false`
- `environment_granularity()` → `EnvironmentGranularity::UNIT`

## File Changes

### New files
- `ui_xml/components/ams_environment_indicator.xml` — compact temp/humidity row
- `ui_xml/ams_environment_overlay.xml` — full overlay layout
- `src/ui/ui_ams_environment_overlay.cpp` / `.h` — overlay logic, smart presets, dryer control

### Modified files
- `ui_xml/components/ams_unit_detail.xml` — add environment indicator
- `ui_xml/components/ams_unit_card.xml` — add indicator on overview cards
- `ui_xml/ams_panel.xml` — remove `<ams_dryer_info_bar>`
- `include/ams_backend.h` — add environment capability virtuals + dryer ops
- `include/ams_backend_cfs.h` / `.cpp` — override environment capabilities
- `src/printer/ams_state.cpp` — remove dryer-specific subject wiring
- `include/ams_types.h` — add `EnvironmentGranularity`, `MaterialComfortRange`

### Deleted files
- `ui_xml/components/ams_dryer_info_bar.xml`
- `src/ui/ui_ams_dryer_card.cpp` / `.h`
- Dryer presets modal

### Not touched
- Per-unit subject infrastructure (`unit_temp_`, `unit_humidity_`) — already built
- `EnvironmentData` struct — already exists on `AmsUnit` and `SlotInfo`
