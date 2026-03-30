# Output Pin Fan Support + fan_feedback Integration

**Date:** 2026-03-30
**Status:** Draft
**Scope:** MAJOR — touches discovery, fan state, API control, fan overlay, settings UI

## Problem

Creality printers (K1, K1C, K2) use `output_pin` objects for fan control instead of standard Klipper `fan`/`fan_generic` sections. These fans are invisible to HelixScreen because:

1. **Discovery** ignores `output_pin` objects as fans (`printer_discovery.h` only recognizes `fan`, `heater_fan`, `fan_generic`, `controller_fan`, `temperature_fan`)
2. **Status updates** don't parse `output_pin` keys (which report `{"value": 0.0-1.0}` instead of `{"speed": 0.0-1.0}`)
3. **Fan control** sends `SET_FAN_SPEED` which doesn't work for `output_pin` objects (need `M106 P<index>` or `SET_PIN`)
4. **fan_feedback** RPM data is completely unused

Additionally, users have no way to rename fans to meaningful names.

## K1C Fan Inventory (from Moonraker)

| Moonraker Object | Type | Data Format | Role |
|---|---|---|---|
| `heater_fan hotend_fan` | Standard | `{"speed": 1.0}` | Hotend cooling |
| `temperature_fan chamber_fan` | Standard | `{"speed": 0.0, "temperature": 36.6}` | Exhaust fan |
| `output_pin fan0` | Output pin | `{"value": 0.0}` | Part cooling (nozzle) |
| `output_pin fan1` | Output pin | `{"value": 0.0}` | Aux/circulation |
| `output_pin fan2` | Output pin | `{"value": 0.0}` | Aux (used during printing) |
| `fan_feedback` | Custom module | `{"fan0_speed": 16000, ...}` | RPM tachometer |

## Design

### 1. Discovery & Classification

**`printer_discovery.h`**: Add `output_pin fan*` recognition. Heuristic: any `output_pin` whose short name starts with `fan` and has `pwm: true` with `scale >= 100` (LEDs use `scale: 1.0`). This aligns with `moonraker_discovery_sequence.cpp` which already does this.

Also discover `fan_feedback` as a capability flag (not a fan itself).

**New `FanType::OUTPUT_PIN_FAN`**: Signals that:
- Control uses `M106 P<index>` (or `SET_PIN` fallback)
- Status comes from `{"value": ...}` not `{"speed": ...}`

**`classify_fan_type()`**:
```
"output_pin fan*" → FanType::OUTPUT_PIN_FAN (controllable = true)
```

### 2. Macro Analysis (new: `MacroFanAnalyzer`)

Small utility that runs once at discovery time. Parses gcode text from Moonraker's `configfile.settings` for these macros:

- **`gcode_macro m106`**: Extract `SET_PIN PIN=fan{N}` patterns to confirm fan index mappings
- **`gcode_macro m107`**: Same extraction for off commands
- **`gcode_macro m141`**: References to `fanN` suggest chamber/circulation role for that fan
- **`gcode_macro printer_param`**: Extract `fans` count, `fan0_min`/`fan2_min` thresholds

Results:
- Map of `output_pin fanN` → M106 index (usually identity: fan0=P0, fan1=P1, fan2=P2)
- Role suggestions: e.g., fan referenced in `m141` → "Chamber Circulation" hint
- Fan count from `PRINTER_PARAM`

These results write default names to `fans/names/` in settings.json (only if no name already exists for that fan — never overwrites user customizations).

### 3. Status Updates

**`update_from_status()`**: Add branch for `output_pin *` keys:

```cpp
if (key.rfind("output_pin ", 0) == 0) {
    if (value.is_object() && value.contains("value") && value["value"].is_number()) {
        double speed = value["value"].get<double>();
        // output_pin scale varies — normalize if needed
        update_fan_speed(key, speed);
    }
}
```

Note: `output_pin` values are already 0.0-1.0 in Moonraker status (the `scale: 255` in config is for gcode `SET_PIN` commands, not status reporting).

**`fan_feedback` integration**: When `fan_feedback` is present in status updates:

```cpp
if (status.contains("fan_feedback")) {
    const auto& fb = status["fan_feedback"];
    for (int i = 0; i < 5; i++) {
        std::string key = "fan" + std::to_string(i) + "_speed";
        if (fb.contains(key) && fb[key].is_number()) {
            int rpm = fb[key].get<int>();
            update_fan_rpm("output_pin fan" + std::to_string(i), rpm);
        }
    }
}
```

**`FanInfo` struct** gains a new field:
```cpp
std::optional<int> rpm;  // from fan_feedback or standard Klipper rpm field
```

Standard Klipper fans that report `rpm` (non-null) also populate this field.

### 4. Fan Control

**`set_fan_speed()` in `moonraker_api_controls.cpp`**: Add third path:

| Fan Type | Command |
|---|---|
| `"fan"` (canonical) | `M106 S<0-255>` |
| `"output_pin fanN"` | `M106 P<index> S<0-255>` |
| `"output_pin <other>"` | `SET_PIN PIN=<short_name> VALUE=<0.0-1.0>` |
| Everything else | `SET_FAN_SPEED FAN=<short_name> SPEED=<0.0-1.0>` |

Index extraction: parse trailing digits from the short name (e.g., `fan0` → 0, `fan2` → 2).

### 5. Display Names — Single Custom Name Field

One `custom_name` field per fan, stored in settings.json:

```json
{
  "fans": {
    "names": {
      "output_pin fan0": "Part Fan",
      "output_pin fan1": "Electronics Fan",
      "heater_fan hotend_fan": "Hotend Fan"
    }
  }
}
```

**Sources that write to this field** (each overwrites the previous, in order of when they run):
1. **Default** — `fan0` → "Part Fan" (assumed), `fanN` → "Fan N+1" (1-indexed)
2. **Macro analysis** — overwrites default if it finds role hints (e.g., fan1 in m141 → "Chamber Circulation")
3. **Printer database** — overwrites if it has explicit mappings for this printer model
4. **Wizard** — overwrites with user-selected role assignment
5. **User manual rename** — final authority

At runtime, `FanInfo.display_name` is simply read from `fans/names/<object_name>`. No priority chain — just one stored value.

### 6. Fan Overlay Changes

**RPM display**: Where `FanInfo.rpm` has a value, show it below the speed percentage on fan dials/cards: "75% · 3692 RPM". For fans without RPM data, just show the percentage.

**Long-press rename**: Long-press on fan name label in the overlay → keyboard modal → save to `fans/names/` → bump `fans_version_` to trigger rebuild.

### 7. Settings > Fans Page

New `FanSettingsOverlay` following the `SensorSettingsOverlay` pattern:

- Access: Settings > Printer > Fans
- Clear-and-repopulate on activate
- Two sections: "Controllable Fans" and "Auto Fans"
- Each row: fan icon + display name (tappable to rename via keyboard modal) + type badge + current speed + RPM if available
- XML row component: `fan_settings_row.xml`
- Tap name → keyboard modal → save to `fans/names/`

### 8. Moonraker Subscription

The status subscription list (built during discovery) must include:
- All `output_pin fan*` objects
- `fan_feedback` (when present)

### 9. Test Plan (TDD)

Tests written before implementation for each component:

- **MacroFanAnalyzer**: Parse M106/M107/M141 macro text, extract fan indices and role hints, handle missing/malformed macros gracefully
- **Discovery**: `output_pin fan*` recognized as fans, `output_pin led` excluded, `fan_feedback` detected as capability
- **classify_fan_type()**: `OUTPUT_PIN_FAN` for `output_pin fan*` objects
- **update_from_status()**: `output_pin` value format parsed correctly, `fan_feedback` RPM mapped to correct fans
- **set_fan_speed()**: Correct gcode generated for each fan type (`M106 P<N>`, `SET_PIN`, `SET_FAN_SPEED`)
- **Display names**: Settings persistence, default naming, overwrite behavior
- **FanInfo.rpm**: Populated from `fan_feedback` and standard `rpm` field

## Files Modified

| File | Change |
|---|---|
| `include/printer_discovery.h` | Add `output_pin fan*` to fans_, detect `fan_feedback` |
| `include/printer_fan_state.h` | Add `OUTPUT_PIN_FAN` to FanType, `rpm` to FanInfo, `update_fan_rpm()` |
| `src/printer/printer_fan_state.cpp` | Handle `output_pin` in `update_from_status()`, `classify_fan_type()`, `fan_feedback` parsing |
| `src/api/moonraker_api_controls.cpp` | `M106 P<index>` path in `set_fan_speed()` |
| `src/api/moonraker_discovery_sequence.cpp` | Subscribe to `fan_feedback` and `output_pin fan*` |
| `include/macro_fan_analyzer.h` | New — macro parsing utility |
| `src/printer/macro_fan_analyzer.cpp` | New — implementation |
| `src/ui/ui_fan_control_overlay.cpp` | RPM display, long-press rename |
| `include/ui_settings_fans.h` | New — fan settings overlay |
| `src/ui/ui_settings_fans.cpp` | New — implementation |
| `ui_xml/fan_settings_overlay.xml` | New — settings layout |
| `ui_xml/fan_settings_row.xml` | New — per-fan row component |
| `tests/test_macro_fan_analyzer.cpp` | New — macro parsing tests |
| `tests/test_printer_fan_state.cpp` | Extended — output_pin + fan_feedback tests |
| `tests/test_fan_control.cpp` | New or extended — set_fan_speed routing tests |
