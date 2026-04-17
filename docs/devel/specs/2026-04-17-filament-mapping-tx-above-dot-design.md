# Filament Mapping Pills — Tx Above Gcode Dot

**Date**: 2026-04-17
**Status**: Design approved
**Scope**: Small UI polish — single function modified

## Problem

On the Print File Detail overlay (tap a file → detail view → FILAMENT MAPPING card),
4-tool multi-color prints wrap onto a second row at medium breakpoint (800×480).
Each pill currently lays out horizontally as `[Tx] [gcode_dot] → [slot_dot]`, and
the inline `Tx` label plus its gap costs enough horizontal space that the row
can't hold four pills on the options column (~4/9 of the content area).

## Goal

Fit four filament-mapping pills on one row at 800×480 by moving the `Tx` tool
label above the gcode color dot (column), trading vertical space (which is
available on the card) for horizontal space.

## Changes

### Where

`src/ui/ui_filament_mapping_card.cpp::rebuild_compact_view()` only. No XML
changes. No public API changes.

### New per-pill structure (multi-tool files, `tool_info_.size() > 1`)

```
pair (flex row, cross=center, gap=inner_gap)
  ├── tool_col (flex column, cross=center, gap=2)
  │     ├── Tx label (font_xs, text_muted)
  │     └── gcode_dot (circle, space_md × space_md)
  ├── arrow "→"           (vertically centered against pair height)
  └── slot_dot            (vertically centered against pair height)
```

Cross-axis centering on the pair (`LV_FLEX_ALIGN_CENTER`) already exists and
gives the arrow and slot dot vertical centering against the taller `tool_col`
child automatically. No extra work needed.

### Single-tool files (no change)

When `tool_info_.size() == 1`, render the pill unchanged:
`[gcode_dot] → [slot_dot]`. No `Tx` label, no column wrapper. This preserves
the existing visual for single-tool prints and keeps the diff minimal.

### Styling details

- `tool_col`: `lv_obj_create(pair)`, `remove_style_all`, `SIZE_CONTENT` ×
  `SIZE_CONTENT`, `flex_flow = COLUMN`, `flex_cross_place = CENTER`,
  `pad_gap = 2` (hardcoded small value — not worth a token for one spot),
  `pad_all = 0`, `bg_opa = 0`, `remove_flag(SCROLLABLE | CLICKABLE)`,
  `add_flag(EVENT_BUBBLE)`.
- `Tx` label: same font/color as before (`font_xs`, `text_muted`). The label
  becomes a child of `tool_col` instead of `pair`.
- `gcode_dot`: unchanged styling, but parented to `tool_col` instead of `pair`.

### What stays the same

- `pair_gap`, `pill_pad_h`, `pill_pad_v`, `pill_radius`, pill background color.
- Swatch size (`space_md`), border color/opa, circle radius.
- Arrow glyph, font, color.
- Slot dot styling including empty-slot warning variant.
- All event handling (no callbacks on the chips themselves — card-level click
  opens the mapping modal).

## Non-goals

- No changes to the FilamentMappingModal (the fullscreen edit UI).
- No changes to `COLORS REQUIRED` swatches (`update_color_swatches` —
  different layout, single-line-of-dots).
- No design-token changes (no shrinking `space_md`, no new tokens).
- No tightening of pill padding or pair gap. If four pills still wrap after
  the layout change, that becomes a follow-up.

## Verification

Visual — at `--test -vv -p print_file_detail` on the medium breakpoint,
load a 4-tool mock file and confirm:
1. Four pills render on a single row.
2. `Tx` sits directly above its gcode dot, horizontally centered with the dot.
3. The `→` arrow and slot dot sit at the vertical midpoint of the pair,
   aligned with the center of the `[Tx] + gcode_dot` column.
4. Single-tool files still render as `[gcode] → [slot]` with no `Tx`.
5. No regression at small/large breakpoints.

## Risk

Low. One function changed. No state, no threading, no cleanup paths affected.
Worst case is a visual misalignment fixed by tweaking `tool_col` gap or
`flex_cross_place`.
