# First-Run Tour Design

**Date:** 2026-04-15
**Status:** Design approved, implementation pending

## Problem

Users who don't read documentation land on the home panel after setup wizards and have no guided introduction to HelixScreen's features. The existing setup wizards (connection, input shaper, spool) handle initial configuration but don't teach the user how to navigate the running UI — particularly the customizable home-screen widget grid and the six navbar destinations.

## Goal

A short, skippable, replayable coach-marked walkthrough that:

- Auto-runs **once** the first time the user reaches the home panel after setup wizards complete.
- Highlights the home grid's purpose and teaches that long-pressing a tile enters edit mode.
- Visits each of the six main navbar destinations with a one-sentence explanation of what lives there.
- Can be skipped at any step (single tap, no confirmation).
- Can be replayed from Settings → Help at any time.

## Non-Goals

- Not demonstrating edit mode programmatically (describe only).
- Not highlighting individual widgets inside non-home panels — the tour calls out the **navbar button** only. Destination panels can evolve freely without breaking the tour.
- No "ask me later" option — Skip is final (Replay is the escape hatch).
- No video, no animation beyond the coach-mark's position shift between steps.

## User-Facing Flow

Eight steps, counter shown as "N / 8" on every step. "Skip" visible on every step; "Next" advances; "Done" replaces "Next" on step 8.

| # | Panel shown | Highlight target | Message |
|---|-------------|------------------|---------|
| 1 | Home | *(none — centered welcome card)* | Welcome to HelixScreen. Let's take a quick tour. |
| 2 | Home | home grid; sequential sub-spotlight of nozzle widget → fan widget → AMS widget (if present) | These tiles show your printer at a glance. |
| 3 | Home | carousel host (whole grid) | Long-press any tile to customize: rearrange, remove, add widgets, add pages. |
| 4 | Home (still) | `nav_btn_print_select` | Browse and start prints from SD, uploads, or network shares. |
| 5 | Home (still) | `nav_btn_controls` | Move axes, home, level. Adjust temps and fans. |
| 6 | Home (still) | `nav_btn_filament` | Load, unload, and swap spools. AMS/AFC lives here. |
| 7 | Home (still) | `nav_btn_advanced` | Macros, console, calibration, updates. |
| 8 | Home (still) | `nav_btn_settings` | Network, display, and **replay this tour here**. |

The tour never navigates away from the home panel — it only highlights navbar buttons in place. This keeps implementation simple and panel-refactor-proof.

### Hardware gating

Step 2's AMS sub-spotlight is skipped when no AMS/AFC/IFS/CFS backend is detected (`AmsState::has_any_backend()` or equivalent). The step itself and the counter math are unchanged — only one sub-spotlight inside step 2 drops.

## Architecture

### Components

```
src/ui/tour/
├── first_run_tour.{h,cpp}   State machine, trigger gate, settings read/write
├── tour_overlay.{h,cpp}     Overlay widget: dim + highlight + tooltip
└── tour_steps.{h,cpp}       Static step table + hardware gating

ui_xml/
└── tour_overlay.xml         Tooltip card layout (title, body, counter, Skip, Next)
```

### `FirstRunTour`

Singleton (`FirstRunTour::instance()`). Responsibilities:

- `maybe_start()` — called from `HomePanel::on_activate()`. Checks gate:
  - `tour.completed` settings flag is false
  - Setup wizards are complete (gate key TBD during implementation — grep existing code for the post-wizard flag; if none, add `setup.wizards_complete` and set it when the last wizard finishes)
  - No tour currently running
  - All pass → `lv_async_call(start_tour_impl, this)` so `on_activate` finishes first
- `start()` — explicit replay entrypoint (bypasses the gate). Called from Settings → Help.
- `advance()` / `skip()` / `finish()` — state transitions.
- Owns the `TourOverlay` instance; destroys it on skip/finish.

**Subject note [L056]:** `FirstRunTour` owns no `lv_subject_t` directly; all UI state is driven by method calls on `TourOverlay`. If a subject is needed later (e.g., to drive a "skip" badge in the navbar), it must be initialized in-place and never copied.

### `TourOverlay` (extends `OverlayBase`)

Built on `lv_layer_top()` so it is above the navbar and all panels. Owns `lifetime_` for async safety (inherited from `OverlayBase`).

```
TourOverlay
├── dim_layer     full-screen, bg_opa=55%, click-blocking (absorbs all touches)
├── highlight     transparent rect positioned over target with outline + glow
└── tooltip       XML-defined card: title, body, counter, Skip, Next
```

**Highlight.** No masking. `style_bg_opa=0`, `style_outline_width=3`, `style_outline_color=ui_theme_get_color("accent")`, `style_outline_pad=6`, soft glow via `style_shadow_*`. Sized to the target's global coords. For steps with no target (Welcome, Done), highlight is hidden and tooltip is screen-centered.

**Tooltip placement.** For each step:
1. Resolve target with `lv_obj_find_by_name(scope_root, step.target_name)`. `scope_root` is the home panel root (since navbar is a child of the app root, lookup starts there).
2. Read target rect via `lv_obj_get_coords()`.
3. Pick above / below / left / right based on available space, honoring `step.anchor_hint` when possible, falling back otherwise.
4. Clamp to screen bounds.
5. Width cap: `min(280, screen_w - 2 × #space_lg)`. Text wraps.

**Dim absorbs all touches** — the user cannot interact with the real UI during the tour. All progression is via Skip/Next. This prevents accidental edit-mode entry, carousel swipes, and navbar taps.

**Teardown:** `safe_delete()` [L059] — normal synchronous cleanup, not inside a queue callback.

### `TourStep` data model

```cpp
struct TourStep {
    std::string target_name;           // lv_obj_find_by_name(); empty = centered
    std::string title_key;             // lv_tr() translation key
    std::string body_key;
    TooltipAnchor anchor_hint;         // PREFER_BELOW / PREFER_ABOVE / PREFER_RIGHT / CENTER
    std::function<bool()> visible_if;  // optional hardware gate
};

std::vector<TourStep> build_tour_steps();  // filters by hardware
```

### Panel switching

The tour does not switch panels. All highlights target the home panel root or navbar children. No `NavigationManager::switch_to_panel()` calls are needed in v1.

## Persistence

Settings keys (managed by `SettingsManager`):

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `tour.completed` | bool | false | Set true on finish **or** skip. Blocks auto-trigger. |
| `tour.version` | int | 1 | Current tour version (compiled in). |
| `tour.last_seen_version` | int | 0 | Highest version user has completed/skipped. |

If a future release materially changes the tour, bumping `tour.version` re-triggers it for existing users when `tour.last_seen_version < tour.version`. Not exposed in UI for v1; reserved in schema only.

## Replay

Add a row to Settings → Help: **"Replay welcome tour"**. Handler calls `FirstRunTour::instance().start()`, bypassing the auto-trigger gate. Does not clear `tour.completed`.

## Skip

Single tap on Skip on any step:
1. Set `tour.completed = true`
2. Set `tour.last_seen_version = tour.version`
3. `SettingsManager::save()`
4. `safe_delete(overlay)`

No confirmation dialog.

## Internationalization

All user-visible strings wrapped in `lv_tr()` with keys like `tour.step.welcome.title`, `tour.step.welcome.body`, `tour.step.home_grid.title`, etc. [L067]

**Not translated** [L070]: "HelixScreen", "Klipper", "AMS", "AFC", "SD" — sentences *containing* these names translate as sentence templates.

**Translation artifacts committed** [L064]:
- `src/generated/lv_i18n_translations.c`
- `src/generated/lv_i18n_translations.h`
- `ui_xml/translations/translations.xml`

Add `// i18n: do not translate` comment next to any string that is intentionally left unwrapped.

## XML callback naming [L039]

All event callbacks prefixed `on_tour_`:
- `on_tour_skip_clicked`
- `on_tour_next_clicked`
- `on_tour_done_clicked`

## Testing

Catch2 unit tests (main-thread only, no `[slow]` tag needed [L052]):

- `[tour] step list builds with AMS present includes AMS sub-spotlight`
- `[tour] step list builds without AMS skips AMS sub-spotlight`
- `[tour] counter total is always 8 regardless of AMS presence`
- `[tour] maybe_start respects tour.completed=true (no-op)`
- `[tour] maybe_start respects wizards-not-complete gate (no-op)`
- `[tour] start() bypasses gate (replay path)`
- `[tour] skip writes tour.completed=true and tour.last_seen_version`
- `[tour] finish writes tour.completed=true and tour.last_seen_version`

No overlay rendering tests — LVGL widget tests are brittle. Manual verification required on:

- Raspberry Pi (800×480)
- AD5M (480×320) — tightest tooltip fit; verify no clipping
- Sonic Pad (1024×600)

## Files

**Added:**
- `src/ui/tour/first_run_tour.{h,cpp}`
- `src/ui/tour/tour_overlay.{h,cpp}`
- `src/ui/tour/tour_steps.{h,cpp}`
- `ui_xml/tour_overlay.xml`
- `tests/test_first_run_tour.cpp`

**Modified:**
- `src/ui/ui_panel_home.cpp` — call `FirstRunTour::instance().maybe_start()` in `on_activate()`
- `src/ui/ui_overlay_settings_help.cpp` (or current Settings → Help location) — add "Replay welcome tour" row
- `src/system/settings_manager.cpp` — register `tour.*` keys with defaults
- `main.cpp` — `lv_xml_component_register_from_file("tour_overlay")` [L014]
- `assets/i18n/*.yml` + regenerated translation artifacts

## Open Items Deferred to Implementation

1. **Wizard-complete gate key.** Grep for an existing flag; fall back to adding `setup.wizards_complete`.
2. **Home widget names.** Confirm actual `name=` attributes for nozzle/fan/AMS widgets in the home grid XML. If a widget is not on the first home page, its sub-spotlight in step 2 is skipped (not page-switched).
3. **AMS detection accessor.** Confirm correct `AmsState` call for "any backend present".
4. **Settings → Help location.** Confirm file and insertion point.
5. **Tooltip fit on 480×320.** Prototype and iterate if text clips.
