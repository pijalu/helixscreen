# Tool Changer Improvements — Design Spec

**Issue**: prestonbrown/helixscreen#493
**Date**: 2026-03-23
**Status**: Draft

## Overview

Three features to improve the multi-tool printer experience: preheat all tools via combo button, quick tool switching from the home panel, and a dedicated multi-nozzle temperature display widget.

Target user: Tool changer operators (e.g., Voron Stealth Changer with 6 tools) who need fast access to tool switching and at-a-glance temperature monitoring for all extruders.

---

## Feature 1: Preheat Combo Button

### What Changes

Modify the existing `PreheatWidget` to support multi-tool preheat. The split button dropdown gains tool-targeting options.

### Behavior

- **Default tap** ("Preheat PLA"): Heats ALL extruders to the selected material's nozzle temp + bed to bed temp. This is the common case for tool changers preparing for a print.
- **Dropdown options** (existing material picker remains):
  - "All Tools (N)" — default, heats every extruder discovered via `ToolState::tools()`
  - Individual tools: "T0", "T1", ... "T5" — heats only the selected extruder + bed
- **Single-extruder printers**: No change in behavior. Dropdown does not show tool options when `ToolState::is_multi_tool()` is false.

### Implementation Approach

- Extend `PreheatWidget::handle_apply()` to iterate `ToolState::tools()` and call `MoonrakerAPI::set_temperature()` for each tool's `effective_heater()` when "All Tools" is selected. Skip tools where `effective_heater()` returns no valid heater. Multiple `set_temperature()` RPCs in sequence is acceptable — Moonraker queues them and they execute within a single Klipper tick.
- **Tool target is separate from material selection.** The material picker (PLA, PETG, ABS, TPU) stays in the existing split button dropdown. Add a secondary button or toggle adjacent to the split button for tool targeting. Default label: "All" (or "All (6)"). Tap to cycle through: All → T0 → T1 → ... → All. This avoids overloading the single split button with two dimensions. Alternative: a small dropdown/popover anchored to a tool icon next to the split button.
- Store the current tool target (all vs specific tool index) as widget state. Default: "all".
- Temp display on the right side: when "all" is selected, show bed temp only. When a specific tool is selected, show that tool's nozzle + bed.
- **Custom macro interaction**: If a custom preheat macro is configured (via `MaterialSettingsManager`), fire the macro once regardless of tool target — the macro is assumed to handle heating itself. Only the non-macro path iterates tools.

### Key Files

- `src/ui/panel_widgets/preheat_widget.cpp` — main logic changes
- `src/ui/panel_widgets/preheat_widget.h` — add tool target state
- `ui_xml/components/panel_widget_preheat.xml` — layout updates for tool target selector

---

## Feature 2: Adaptive Tool Switcher Widget

### What It Is

A new panel widget (`panel_widget_tool_switcher`) for the home panel dashboard. Provides quick tool switching without navigating to the filament panel.

### Adaptive Sizing

| Widget Size | Layout |
|-------------|--------|
| **1x1** | Shows current tool label (e.g., "T2") centered. Tap opens a popup/context menu with a grid of all tools. Active tool highlighted. |
| **1x2 / 2x1** | Inline pill buttons in a row (or column for 1x2). One pill per tool. Active tool uses primary color. Tap a pill to switch. |
| **2x2** | Pill buttons + additional info (could show tool name or temp alongside each pill). |

### Behavior

- Active tool highlighted with primary accent color.
- Tap an inactive tool → calls `ToolState::request_tool_change(index, api, on_success, on_error)`.
- Tool count auto-discovered from `ToolState::tools()`.
- Widget auto-hides when `ToolState::is_multi_tool()` is false (single-extruder printers).
- Observes `active_tool_` and `tool_count_` subjects for reactive updates.

### Context Menu (1x1 mode)

- Follow the `FanStackWidget` picker pattern (`fan_stack_widget.cpp` `picker_backdrop_`): create a backdrop overlay on the active screen, position a container near the widget, populate with tool buttons, dismiss on selection or backdrop tap.
- Grid layout: 3 columns for 6 tools, 2 columns for 4, etc.
- Dismiss on selection or tap outside.

### Safety

- **During active print**: Tool changing mid-print is dangerous (collisions, failed prints). Check `PrinterState::get_print_state()` before allowing tool changes. If printing, show a confirmation dialog via `modal_show_confirmation()` with severity warning: "Changing tools during a print may cause issues. Continue?"

### Key Files (new)

- `src/ui/panel_widgets/tool_switcher_widget.cpp`
- `src/ui/panel_widgets/tool_switcher_widget.h`
- `ui_xml/components/panel_widget_tool_switcher.xml`
- Register in `main.cpp` (`lv_xml_component_register_from_file()`)
- Register in panel widget factory

---

## Feature 3: Multi-Nozzle Temperature List Widget

### What It Is

A new panel widget (`panel_widget_nozzle_temps`) showing raw temperature values for all extruders in a readable vertical stack. Preferred size: 1x2 (tall).

### Layout

Each extruder gets a row:
```
[T0]  215°          → 210°  [===progress===]
[T1]  208°          → 210°  [==progress====]
[T2]  210°          → 210°  [===progress===]
[T3]  198°          → 210°  [=progress=====]
[T4]  205°          → 210°  [==progress====]
[T5]   22°            off   [              ]
───────────────────────────────────────────
[bed]  58°          →  60°  [==progress====]
```

- **Tool label**: Left-aligned, primary color, bold (T0, T1, ...)
- **Current temp**: Large, readable font weight
- **Target temp**: Smaller, muted — or "off" when target is 0
- **Progress bar**: Thin bar (3px) under each row. Color-coded:
  - Green: at temp (within ~2°C of target)
  - Yellow/amber: heating (below target)
  - Gray: off (no target set)
  - Orange/red: bed (different accent)
- **Bed separator**: Visual divider before bed row
- **Bed row**: Same format, different accent color

### Dynamic Behavior

- Rows ordered by iterating `ToolState::tools()` (a `vector<ToolInfo>` in tool index order), then looking up temp subjects by each tool's `extruder_name`. This gives deterministic T0, T1, T2... ordering. Do NOT iterate `PrinterTemperatureState::extruders()` directly (unordered map, non-deterministic order).
- Each row observes per-extruder subjects via `get_extruder_temp_subject(name, lifetime)` and `get_extruder_target_subject(name, lifetime)`.
- **Must use `SubjectLifetime` tokens** for all per-extruder subject observers (dynamic subjects can be destroyed/recreated on reconnection).
- **Observe `extruder_version` subject** (`PrinterTemperatureState::get_extruder_version_subject()`) to trigger row rebuilds when extruder list changes (reconnection, rediscovery). Same pattern as `FanStackWidget` version observer.
- Bed row observes `get_bed_temp_subject()` and `get_bed_target_subject()` (static subjects, no lifetime token needed).
- Widget scrolls if tool count exceeds visible area (unlikely in 1x2 for 6 tools, but handles edge cases).
- Auto-hides when only one extruder detected (single-tool printers don't need this).
- **Destructor/detach**: Use `ScopedFreeze` around drain+destroy since the widget holds 2 observers per extruder (potentially 12+ for a 6-tool setup) on dynamic subjects.

### Tap Interaction

- Tap a tool row → opens temperature control for that specific extruder (navigate to temp panel or open a quick-set modal). This is a stretch goal — initial version can be display-only.

### Key Files (new)

- `src/ui/panel_widgets/nozzle_temps_widget.cpp`
- `src/ui/panel_widgets/nozzle_temps_widget.h`
- `ui_xml/components/panel_widget_nozzle_temps.xml`
- Register in `main.cpp` and panel widget factory

---

## Architecture Notes

### Threading Safety

All temperature and tool state updates arrive via WebSocket on a background thread. Per project rules:
- Subject updates go through `ui_queue_update()` (already handled by `PrinterState` internals).
- Widget observers use `observe_int_sync` / `observe_string` which defer callbacks via `ui_queue_update()`.
- No direct `lv_subject_set_*()` calls from widget code.

### Observer Cleanup

- All observers managed via `ObserverGuard` (RAII).
- Dynamic subjects (per-extruder) require `SubjectLifetime` tokens passed to both `get_*_subject()` and `observe_*()` calls.
- Widget destructor: freeze update queue → drain → clean children (per `ScopedFreeze` pattern).

### Registration

New widgets must be:
1. XML component registered via `lv_xml_component_register_from_file()` in `main.cpp`
2. Registered in the panel widget factory so users can add them to their dashboard

### Responsive Design

All sizing uses design tokens (`#space_md`, `#button_height`, etc.). No hardcoded pixel values. Widgets adapt to breakpoint via the standard panel widget size callback system.

---

## Out of Scope

- Custom preheat macros (separate feature, spec already exists)
- Tool offset editing from the home panel
- Filament color display per tool (AMS integration is separate)
- Chamber temperature in the nozzle temps widget (already in temp_stack_widget)

---

## Test Plan

- Unit tests for preheat-all-tools logic (mock ToolState with N tools, verify N API calls)
- Unit tests for tool switcher tool change requests
- Visual testing: run with `--test -vv` using mock multi-tool printer config
- Verify single-extruder printers: new widgets auto-hide, preheat behaves as before
- Verify observer cleanup: no crashes on panel switch / reconnection
