# Preset-Aware Wizard Mode

**Date**: 2026-03-02
**Status**: Approved

## Problem

Non-generic builds targeting known hardware (AD5M, CC1, etc.) force users through the full 13-step wizard, including hardware discovery steps where the answers are already known. This wastes time and creates a poor first-boot experience.

## Solution

The wizard gains a **preset mode** that detects pre-populated configs from known-hardware builds and shows only the steps that require user input: touch calibration (if needed), language, connection (if validation fails), and telemetry opt-in.

## Preset File Format

Presets live at `config/presets/<PLATFORM_TARGET>.json` (convention-based naming matching the Makefile's `PLATFORM_TARGET`).

Existing preset format gains one field:

```json
{
  "preset": "ad5m",
  "wizard_completed": false,
  "input": { "calibration": { ... } },
  "printer": {
    "moonraker_host": "127.0.0.1",
    "moonraker_port": 7125,
    "heaters": { ... },
    "fans": { ... },
    "leds": { ... },
    "filament_sensors": { ... },
    "hardware": { ... },
    "default_macros": { ... }
  }
}
```

- `"preset"` — identifies this config as preset-based; triggers abbreviated wizard mode; persists permanently (useful for platform-specific runtime behavior)
- `"wizard_completed": false` — ensures the abbreviated wizard runs on first boot
- All hardware sections pre-populated as today

## Wizard Flow in Preset Mode

When `Config::has_preset()` is true and `wizard_completed` is false:

| Step | Internal # | Show? | Logic |
|------|-----------|-------|-------|
| Touch Calibration | 0 | If needed | Skip if preset has `input.calibration.valid = true` or platform is Android/DRM |
| Language | 1 | Always | User preference |
| WiFi | 2 | Skip | Known hardware = direct-attached, WiFi managed by host OS |
| Connection | 3 | Auto-validate | Try preset's `moonraker_host:port`. Connected → skip. Failed → show |
| Printer Identify | 4 | Skip | Preset has hardware config |
| Heater Select | 5 | Skip | Preset has heater mappings |
| Fan Select | 6 | Skip | Preset has fan mappings |
| AMS Identify | 7 | Skip | Preset has AMS config (or none) |
| LED Select | 8 | Skip | Preset has LED config (or none) |
| Filament Sensors | 9 | Skip | Preset has sensor config |
| Probe Sensor | 10 | Skip | Preset has probe config |
| Input Shaper | 11 | Skip | Preset has shaper config |
| **Telemetry** | **NEW** | **Always** | Dedicated opt-in step (preset mode only) |
| Summary | 12→13 | Skip | Not needed — hardware is known |

Normal (non-preset) wizard is unchanged. The telemetry step only exists in preset mode (normal wizard keeps telemetry on the summary page).

## Connection Auto-Validation

In preset mode, before showing the connection step, the wizard attempts to connect to the preset's Moonraker host/port:

1. Issue a one-shot HTTP GET to `http://<host>:<port>/server/info` with a short timeout (~3s)
2. If successful → skip connection step entirely
3. If failed → show connection step so user can correct the address

This handles the common case (on-device install, localhost works) while gracefully falling back when the network isn't ready.

## New Telemetry Wizard Step

A dedicated step for telemetry opt-in, replacing the telemetry section currently embedded in the summary page (only in preset mode).

**Files:**
- `include/ui_wizard_telemetry.h`
- `src/ui/ui_wizard_telemetry.cpp`
- `ui_xml/wizard_telemetry.xml`

**UI:** Full-page treatment with:
- Telemetry toggle (bound to `settings_telemetry_enabled` subject)
- Description: "Anonymous stats — no personal data, ever."
- "Learn More" button (opens telemetry info overlay)
- Same content as current summary page telemetry section, given dedicated space

**Step interface:** Implements `WizardStep` with `should_skip()` returning `false` (always shown in preset mode).

## Packaging Changes

**`scripts/package.sh`** — Replace hardcoded if/elif with convention-based lookup:

```bash
preset_file="${PROJECT_DIR}/config/presets/${platform}.json"
if [ -f "$preset_file" ]; then
    cp "$preset_file" "$pkg_dir/config/helixconfig.json"
    log_info "  Using ${platform} preset as default config"
else
    cp "${PROJECT_DIR}/config/helixconfig.json.template" \
       "$pkg_dir/config/helixconfig.json.template" 2>/dev/null || true
fi
```

No installer changes needed — backup/restore on upgrade already works.

## Config System Changes

Add helper methods to `Config`:

```cpp
bool Config::has_preset() const;        // true if "preset" field exists and is non-empty
std::string Config::get_preset() const; // returns preset name or ""
```

## Wizard Implementation Changes

**`ui_wizard.cpp`:**
- Add `static bool preset_mode_` flag, set during wizard init from `Config::has_preset()`
- `ui_wizard_precalculate_skips()`: In preset mode, force-skip WiFi (2), hardware steps (4-11), and summary (12/13)
- Connection step (3): Add auto-validation logic before showing
- Touch cal step (0): Check preset calibration validity
- Register new telemetry step; activate only in preset mode

**Step display count:** Already handled by `WizardSkipFlags` + `wizard_calculate_display_step()` — no changes needed; step counter auto-adjusts.

## Existing Preset Updates

| File | Change |
|------|--------|
| `config/presets/ad5m.json` | Add `"preset": "ad5m"`, change `wizard_completed` to `false` |
| `config/presets/ad5x.json` | Add `"preset": "ad5x"`, ensure `wizard_completed` is `false` |
| `config/presets/cc1.json` | Add `"preset": "cc1"`, keep `wizard_completed: false` |
| `config/presets/voron-v2-afc.json` | Reference config, no changes needed |

## Testing

- Unit test: `WizardSkipFlags` in preset mode skips correct steps
- Unit test: `Config::has_preset()` / `Config::get_preset()`
- Manual test on AD5M: First boot shows only language → telemetry → done (touch cal has valid preset, connection auto-validates to localhost)
- Manual test: Remove Moonraker → connection step should appear
- Manual test: Normal wizard (no preset) unchanged
