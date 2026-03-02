# Grid Edit Mode Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Android/Samsung-style in-panel grid editing for the home panel dashboard — long-press to enter, drag to reposition, resize via corner handles, add/remove widgets.

**Architecture:** A new `GridEditMode` class owned by `HomePanel` manages all edit state: grid dots overlay, selection chrome, drag/resize gestures, and the widget catalog. It intercepts LVGL events on the widget container during edit mode and modifies `PanelWidgetConfig` on changes. HomePanel delegates long-press events to it and queries `is_active()` to suppress normal widget interactions.

**Tech Stack:** C++17, LVGL 9.5 grid layout, existing `PanelWidgetConfig`/`PanelWidgetManager`/`GridLayout` infrastructure, `ObserverGuard` for cleanup, XML components for overlays.

**Design doc:** `docs/plans/2026-02-25-grid-edit-mode-design.md`

---

## Progress

| Task | Status | Commit |
|------|--------|--------|
| Phase 1: Grid Infrastructure | DONE | `14da6f31` |
| Phase 2: Widget Extraction (printer_image, print_status, tips) | DONE | `d51f0564` |
| Task 1: GridEditMode Skeleton | DONE | `1b80edb2` |
| Task 2: Navbar Done Button | DONE | `9e9d14a0` |
| Task 3: Grid Dots Overlay | DONE | `ae3a7d30` |
| Task 4: Widget Selection + Corner Brackets | DONE | `7f6e2a42` |
| Task 5: Drag-to-Reposition | DONE | `1648d68d` |
| Task 6: Drag-to-Resize | DONE | `49035091` |
| Task 7: Widget Catalog Overlay | DONE | `84664673` |
| Task 8: Double-Tap Migration | DONE | `36272c90` |
| Task 9: Code Review Fixes | DONE | `1601228b` |
| Dynamic bottom-right packing + config persistence | DONE | `4d50706e` |
| Phase 5: Polish & Edge Cases | DONE | various |

### Post-review architectural changes (session 2026-02-25)

- **Dynamic placement**: 1×1 widgets no longer get positions at config-build time. Positions are computed dynamically at each `populate_widgets()` call using bottom-right-first packing.
- **Config persistence**: Positions written back to config entries in-memory after each populate. Saved to disk on edit mode enter/exit only.
- **Segfault fix**: Added `UpdateQueue::drain()` after widget detach in populate to prevent use-after-free from deferred observer callbacks.
- **Default layout**: Only 3 anchor widgets (printer_image, print_status, tips) get fixed grid positions. All other widgets are auto-placed.

### Remaining work

- [x] Breakpoint adaptation (widget clamping/hiding on smaller screens)
- [x] Per-widget config UIs — macro picker (favorite_macro), mode toggle (fan_stack, temp_stack)
- [x] Animation/transition polish — selection pulse, resize snap animation
- [x] Debounce timer — CoalescedTimer 300ms in PanelWidgetManager
- [x] Visual polish: every widget at every breakpoint × aspect ratio
- [x] Widget sizing constraints — `on_size_changed()` virtual, min/max for all 23 widgets
- [ ] Settings entry "Customize Home Panel" (long-press gesture only, no menu entry yet)
- [x] New widget types: clock, print queue done; quick actions covered by macro widgets
- [ ] New widget types: camera, print stats

### Phase 6: Visual Polish — Every Widget × Every Breakpoint × Every Aspect Ratio

Iteratively test and fix each widget's visual appearance across all screen sizes.
One widget at a time, reviewed together before moving to the next.

**Test matrix:**

| Breakpoint | Resolution | Aspect | Grid | CLI flag |
|------------|-----------|--------|------|----------|
| TINY       | 480×320   | 3:2    | 6×4  | `-s 480x320`  |
| SMALL      | 480×400   | 6:5    | 6×4  | `-s 480x400`  |
| SMALL (UW) | 1920×440  | ~4:1   | 6×4  | `-s 1920x440` |
| MEDIUM     | 800×480   | 5:3    | 6×4  | `-s 800x480`  |
| LARGE      | 1024×600  | ~17:10 | 8×5  | `-s 1024x600` |
| XLARGE     | 1280×720  | 16:9   | 8×5  | `-s 1280x720` |

**Widgets to polish (19 total):**

1. `printer_image` — 3D printer visualization (2×2, resizable 1-4 × 1-3)
2. `print_status` — Print progress card (2×2, resizable 2-4 × 1-3)
3. `tips` — Rotating tips (4×1 anchor, resizable 2-6 × 1)
4. `temperature` — Nozzle temp (1×1)
5. `fan_stack` — Fan speeds (1×1)
6. `notifications` — Pending alerts (1×1)
7. `ams` — Multi-material spool status (1×1)
8. `power` — Moonraker power controls (1×1, hardware-gated)
9. `led` — LED light toggle (1×1, hardware-gated)
10. `humidity` — Humidity sensor (1×1, hardware-gated)
11. `width_sensor` — Filament width sensor (1×1, hardware-gated)
12. `probe` — Z probe status (1×1, hardware-gated)
13. `filament` — Filament sensor (1×1, hardware-gated)
14. `network` — Wi-Fi/ethernet status (1×1, default disabled)
15. `firmware_restart` — Klipper restart (1×1, default disabled)
16. `temp_stack` — Multi-temp stacked (1×1, default disabled)
17. `thermistor` — Custom temp sensor (1×1, default disabled)
18. `favorite_macro_1` — Macro button (1×1, default disabled)
19. `favorite_macro_2` — Macro button (1×1, default disabled)

**Process per widget:**
1. Launch app at each resolution: `./build/bin/helix-screen --test -vv -s WxH`
2. Screenshot or visually inspect the widget
3. Check: text truncation, icon alignment, padding, touch target size, overflow
4. Fix XML/theme issues (applying [L040]: inline attrs override bind_style)
5. Re-verify at all resolutions
6. Move to next widget

---

## Task 1: GridEditMode Skeleton — State Machine + Enter/Exit

Create the `GridEditMode` class with basic state management. No visuals yet — just the state transitions and integration with HomePanel.

**Files:**
- Create: `include/grid_edit_mode.h`
- Create: `src/ui/grid_edit_mode.cpp`
- Modify: `include/ui_panel_home.h` — add `GridEditMode` member
- Modify: `src/ui/ui_panel_home.cpp` — wire long-press to enter edit mode
- Test: `tests/unit/test_grid_edit_mode.cpp`

**Step 1: Write failing tests**

```cpp
// tests/unit/test_grid_edit_mode.cpp
#include <catch2/catch_test_macros.hpp>
#include "grid_edit_mode.h"
#include "panel_widget_config.h"
#include "config.h"

using namespace helix;

TEST_CASE("GridEditMode: starts inactive", "[grid_edit][edit_mode]") {
    GridEditMode em;
    REQUIRE_FALSE(em.is_active());
}

TEST_CASE("GridEditMode: enter/exit toggles state", "[grid_edit][edit_mode]") {
    GridEditMode em;
    em.enter(nullptr, nullptr);  // null container/config OK for state test
    REQUIRE(em.is_active());
    em.exit();
    REQUIRE_FALSE(em.is_active());
}

TEST_CASE("GridEditMode: exit when not active is no-op", "[grid_edit][edit_mode]") {
    GridEditMode em;
    em.exit();  // Should not crash
    REQUIRE_FALSE(em.is_active());
}

TEST_CASE("GridEditMode: double enter is no-op", "[grid_edit][edit_mode]") {
    GridEditMode em;
    em.enter(nullptr, nullptr);
    em.enter(nullptr, nullptr);  // Second enter should be ignored
    REQUIRE(em.is_active());
    em.exit();
    REQUIRE_FALSE(em.is_active());
}
```

**Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[grid_edit]"`
Expected: Compilation error — `grid_edit_mode.h` does not exist.

**Step 3: Write minimal implementation**

```cpp
// include/grid_edit_mode.h
#pragma once

#include "lvgl/lvgl.h"

#include <functional>
#include <string>

namespace helix {

class PanelWidgetConfig;

/// Manages in-panel grid editing for the home dashboard.
/// Handles: edit mode state, grid dot overlay, widget selection,
/// drag-to-reposition, drag-to-resize, widget removal, widget catalog.
class GridEditMode {
  public:
    using SaveCallback = std::function<void()>;

    GridEditMode() = default;
    ~GridEditMode();

    // Non-copyable, non-movable (owns LVGL resources)
    GridEditMode(const GridEditMode&) = delete;
    GridEditMode& operator=(const GridEditMode&) = delete;

    /// Enter edit mode on the given grid container
    void enter(lv_obj_t* container, PanelWidgetConfig* config);

    /// Exit edit mode, save config, trigger rebuild
    void exit();

    bool is_active() const { return active_; }

    /// Set callback for when config changes need to be saved + rebuilt
    void set_save_callback(SaveCallback cb) { save_cb_ = std::move(cb); }

  private:
    bool active_ = false;
    lv_obj_t* container_ = nullptr;
    PanelWidgetConfig* config_ = nullptr;
    SaveCallback save_cb_;
};

} // namespace helix
```

```cpp
// src/ui/grid_edit_mode.cpp
#include "grid_edit_mode.h"
#include "panel_widget_config.h"

#include <spdlog/spdlog.h>

namespace helix {

GridEditMode::~GridEditMode() {
    if (active_) {
        exit();
    }
}

void GridEditMode::enter(lv_obj_t* container, PanelWidgetConfig* config) {
    if (active_) {
        spdlog::debug("[GridEditMode] Already active, ignoring enter()");
        return;
    }
    active_ = true;
    container_ = container;
    config_ = config;
    spdlog::info("[GridEditMode] Entered edit mode");
}

void GridEditMode::exit() {
    if (!active_) {
        return;
    }
    active_ = false;

    if (config_) {
        config_->save();
    }
    if (save_cb_) {
        save_cb_();
    }

    container_ = nullptr;
    config_ = nullptr;
    spdlog::info("[GridEditMode] Exited edit mode");
}

} // namespace helix
```

Add to Makefile: `src/ui/grid_edit_mode.cpp` in sources, `tests/unit/test_grid_edit_mode.cpp` in test sources.

**Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[grid_edit]"`
Expected: All 4 tests PASS.

**Step 5: Wire into HomePanel**

Add `#include "grid_edit_mode.h"` to `ui_panel_home.h`, add member `helix::GridEditMode grid_edit_mode_;`.

In `ui_panel_home.cpp`, add a long-press event handler on `widget_container` that calls `grid_edit_mode_.enter(container, &widget_config)`. This is the entry point — the visuals come in later tasks.

**Step 6: Commit**

```bash
git add include/grid_edit_mode.h src/ui/grid_edit_mode.cpp tests/unit/test_grid_edit_mode.cpp
git add include/ui_panel_home.h src/ui/ui_panel_home.cpp Makefile
git commit -m "feat(grid): add GridEditMode skeleton with enter/exit state machine"
```

---

## Task 2: Navbar Done Button

Add a checkmark button to the top of the navbar that appears only during edit mode, controlled by a subject.

**Files:**
- Modify: `ui_xml/navigation_bar.xml` — add edit mode done button
- Modify: `src/ui/grid_edit_mode.cpp` — set subject on enter/exit
- Modify: `src/ui/ui_panel_home.cpp` — register callback for done button

**Step 1: Create the `home_edit_mode` subject**

In `grid_edit_mode.cpp`, in `enter()`: create/set an int subject `home_edit_mode` to 1.
In `exit()`: set it to 0.

The subject should be initialized in HomePanel's `init_subjects()` as a static int subject with value 0.

**Step 2: Add done button to navbar XML**

Insert before the Home Button in `navigation_bar.xml`:

```xml
<!-- Edit Mode Done Button - only visible during home panel edit mode -->
<ui_button name="nav_btn_edit_done"
           width="100%" style_min_height="48" variant="ghost" flex_flow="column"
           style_flex_main_place="center" style_flex_cross_place="center"
           style_flex_track_place="center" style_border_width="0" hidden="true">
  <bind_flag_if_eq subject="home_edit_mode" flag="hidden" ref_value="0"/>
  <icon src="check" size="#icon_size" variant="success"/>
  <event_cb trigger="clicked" callback="on_edit_done_clicked"/>
</ui_button>
```

Applying [L039]: Use unique callback name `on_edit_done_clicked` to avoid collisions.

**Step 3: Register callback in HomePanel**

In `ui_panel_home.cpp` `setup()`, register:
```cpp
lv_xml_register_event_cb("on_edit_done_clicked", [](lv_event_t* e) {
    get_global_home_panel().grid_edit_mode_.exit();
});
```

**Step 4: Visual test**

Run: `./build/bin/helix-screen --test -vv`
- Long-press a widget → checkmark appears at top of navbar
- Tap checkmark → edit mode exits, checkmark disappears

**Step 5: Commit**

```bash
git commit -m "feat(grid): add navbar done button for edit mode exit"
```

---

## Task 3: Grid Dots Overlay

Draw small dots at cell intersection points when edit mode is active.

**Files:**
- Modify: `src/ui/grid_edit_mode.cpp` — create/destroy dots overlay
- Modify: `include/grid_edit_mode.h` — add dots overlay member

**Step 1: Implement dots overlay**

In `enter()`, create a transparent overlay `lv_obj_t*` the same size as the container, with `LV_OBJ_FLAG_FLOATING` so it doesn't affect grid layout. Set `clickable="false"` so events pass through.

Draw dots using LVGL canvas or by creating small `lv_obj_t` circles at each grid intersection. Given the grid has `cols+1` × `rows+1` intersection points:

```cpp
void GridEditMode::create_dots_overlay() {
    dots_overlay_ = lv_obj_create(container_);
    lv_obj_set_size(dots_overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(dots_overlay_, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(dots_overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(dots_overlay_, LV_OPA_TRANSP, 0);

    // Get grid dimensions
    lv_area_t area;
    lv_obj_get_content_coords(container_, &area);
    int w = area.x2 - area.x1;
    int h = area.y2 - area.y1;

    int ncols = GridLayout::get_cols(breakpoint_);
    int nrows = /* actual rows used from grid descriptors */;

    // Create dot at each intersection (including edges)
    for (int r = 0; r <= nrows; ++r) {
        for (int c = 0; c <= ncols; ++c) {
            lv_obj_t* dot = lv_obj_create(dots_overlay_);
            lv_obj_set_size(dot, 4, 4);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, ui_theme_get_color("text_secondary"), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_30, 0);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            // Position at intersection
            int x = c * (w / ncols) - 2;  // Center the 4px dot
            int y = r * (h / nrows) - 2;
            lv_obj_set_pos(dot, x, y);
        }
    }
}
```

In `exit()`, destroy the dots overlay: `lv_obj_delete(dots_overlay_); dots_overlay_ = nullptr;`

**Step 2: Fade animation**

Wrap dots creation with opacity animation: start at `LV_OPA_TRANSP`, animate to `LV_OPA_30` over 200ms on enter. Reverse on exit (animate to 0, then delete in completion callback).

**Step 3: Visual test**

Run: `./build/bin/helix-screen --test -vv`
- Long-press → dots fade in at grid intersections
- Tap done → dots fade out

**Step 4: Commit**

```bash
git commit -m "feat(grid): add grid dots overlay for edit mode"
```

---

## Task 4: Widget Selection — Tap to Select + Corner Brackets

Tap a widget in edit mode to select it. Show L-shaped corner brackets.

**Files:**
- Modify: `include/grid_edit_mode.h` — selected widget tracking, bracket objects
- Modify: `src/ui/grid_edit_mode.cpp` — tap handler, bracket creation/positioning

**Step 1: Write failing test**

```cpp
TEST_CASE("GridEditMode: select/deselect widget tracking", "[grid_edit][selection]") {
    GridEditMode em;
    em.enter(nullptr, nullptr);

    REQUIRE(em.selected_widget() == nullptr);

    // Simulate selection (direct API, no LVGL objects needed)
    lv_obj_t fake_widget;  // Stack-allocated placeholder
    em.set_selected(&fake_widget);
    REQUIRE(em.selected_widget() == &fake_widget);

    em.set_selected(nullptr);
    REQUIRE(em.selected_widget() == nullptr);

    // Selection clears on exit
    em.set_selected(&fake_widget);
    em.exit();
    REQUIRE(em.selected_widget() == nullptr);
}
```

**Step 2: Implement selection tracking**

Add to `GridEditMode`:
```cpp
lv_obj_t* selected_widget() const { return selected_; }
void set_selected(lv_obj_t* widget);
```

`set_selected()` stores the pointer, creates/repositions 4 corner bracket objects (small L-shapes using `lv_obj_t` with border styling), and creates an (X) button.

**Step 3: Corner bracket visuals**

Each bracket is a small `lv_obj_t` (e.g., 12×12px) with two-sided borders in the accent color, positioned at each corner of the selected widget's bounds. Use `lv_obj_get_coords()` to get widget screen coordinates, position brackets relative to the overlay.

For resizable widgets (where `PanelWidgetDef::is_scalable()` returns true), make brackets larger (16×16px) and add a filled accent background to indicate they're draggable handles.

**Step 4: Tap event wiring**

In `enter()`, add `LV_EVENT_CLICKED` callback to the container. On click:
1. Get pointer position via `lv_indev_get_point()`
2. Hit-test against child widgets using `lv_hit_test()`
3. If hit a widget, call `set_selected(widget)`. If hit empty area, call `set_selected(nullptr)`.

**Step 5: (X) button on selected widget**

When a widget is selected, create a small X button in the top-right corner of the selection bounds. On tap:
1. Get widget ID from config (match by grid position)
2. Call `config_->set_enabled(index, false)` to disable
3. Call `set_selected(nullptr)` to deselect
4. Trigger rebuild

**Step 6: Commit**

```bash
git commit -m "feat(grid): widget selection with corner brackets and (X) removal"
```

---

## Task 5: Drag-to-Reposition

Long-press a selected widget and drag to move it to a new grid cell.

**Files:**
- Modify: `include/grid_edit_mode.h` — drag state members
- Modify: `src/ui/grid_edit_mode.cpp` — drag start/move/end handlers

**Step 1: Write failing test for grid cell hit-testing**

```cpp
TEST_CASE("GridEditMode: screen_to_grid_cell maps coordinates correctly",
          "[grid_edit][grid_cell]") {
    // 6-column grid, container at (100, 0) with width 600, height 400, 4 rows
    // Cell size: 100x100
    auto cell = GridEditMode::screen_to_grid_cell(
        150, 50,   // point (relative to container: 50, 50)
        100, 0,    // container origin
        600, 400,  // container size
        6, 4       // cols, rows
    );
    REQUIRE(cell.first == 0);   // col 0 (50/100)
    REQUIRE(cell.second == 0);  // row 0 (50/100)

    // Bottom-right corner
    auto cell2 = GridEditMode::screen_to_grid_cell(
        690, 390,
        100, 0,
        600, 400,
        6, 4
    );
    REQUIRE(cell2.first == 5);  // col 5
    REQUIRE(cell2.second == 3); // row 3
}
```

**Step 2: Implement `screen_to_grid_cell()` static helper**

```cpp
static std::pair<int, int> screen_to_grid_cell(
    int screen_x, int screen_y,
    int container_x, int container_y,
    int container_w, int container_h,
    int ncols, int nrows);
```

Maps screen coordinates to grid (col, row). Clamps to valid range.

**Step 3: Implement drag handlers**

Adapt the pattern from `ui_settings_panel_widgets.cpp`:

- **`handle_drag_start()`**: Called on `LV_EVENT_LONG_PRESSED` while a widget is selected.
  - Record original grid position from config
  - Add `LV_OBJ_FLAG_FLOATING` to widget
  - Apply elevation (shadow + slight scale)
  - Create ghost outline at original position (dashed border)
  - Record drag offset (pointer pos - widget pos)

- **`handle_drag_move()`**: Called on `LV_EVENT_PRESSING`.
  - Move widget to follow pointer (minus offset)
  - Compute target grid cell from pointer position
  - Show/hide snap preview at target cell
  - Validate placement: use `GridLayout::can_place()` for empty cells, or check same-size swap

- **`handle_drag_end()`**: Called on `LV_EVENT_RELEASED`.
  - If valid target: update config entries (swap or move), animate widget to target cell
  - If invalid: animate widget back to original position
  - Remove floating flag, ghost outline, snap preview
  - Save config + trigger rebuild

**Step 4: Same-size swap logic**

When dragging onto an occupied cell:
1. Look up the occupying widget's span from config
2. If same colspan AND same rowspan as dragged widget → swap their grid positions in config
3. Otherwise → reject (snap back)

**Step 5: Visual test**

Run: `./build/bin/helix-screen --test -vv`
- Enter edit mode, select a 1×1 widget
- Long-press and drag to an empty cell → widget moves
- Drag onto another 1×1 widget → they swap
- Drag onto a 2×2 widget → rejected, snaps back

**Step 6: Commit**

```bash
git commit -m "feat(grid): drag-to-reposition with same-size swap"
```

---

## Task 6: Drag-to-Resize

Drag corner handles on resizable widgets to change their colspan/rowspan.

**Files:**
- Modify: `include/grid_edit_mode.h` — resize state
- Modify: `src/ui/grid_edit_mode.cpp` — corner handle drag detection + resize logic

**Step 1: Write failing test**

```cpp
TEST_CASE("GridEditMode: clamp_span respects min/max from registry",
          "[grid_edit][resize]") {
    // printer_image: min 1×1, max 4×3, default 2×2
    auto [c, r] = GridEditMode::clamp_span("printer_image", 5, 4);
    REQUIRE(c == 4);  // clamped to max_colspan
    REQUIRE(r == 3);  // clamped to max_rowspan

    auto [c2, r2] = GridEditMode::clamp_span("printer_image", 0, 0);
    REQUIRE(c2 == 1);  // clamped to min_colspan
    REQUIRE(r2 == 1);  // clamped to min_rowspan

    // power: not scalable (min == max == 1×1)
    auto [c3, r3] = GridEditMode::clamp_span("power", 3, 3);
    REQUIRE(c3 == 1);
    REQUIRE(r3 == 1);
}
```

**Step 2: Implement `clamp_span()` static helper**

```cpp
static std::pair<int, int> clamp_span(const std::string& widget_id,
                                       int desired_colspan, int desired_rowspan);
```

Looks up `PanelWidgetDef` from registry, clamps to `effective_min/max_colspan/rowspan()`.

**Step 3: Implement corner handle drag**

When the user drags a corner bracket on a resizable widget:
1. Detect which corner is being dragged (top-left, top-right, bottom-left, bottom-right)
2. Map pointer position to grid cell
3. Compute new colspan/rowspan based on which corner moved
4. Clamp via `clamp_span()`
5. Validate with `GridLayout::can_place()` (new span must not overlap other widgets)
6. Update selection outline in real-time to show new size
7. On release: update config entry's colspan/rowspan, save + rebuild

**Step 4: Visual test**

Run: `./build/bin/helix-screen --test -vv`
- Enter edit mode, select printer_image (2×2, scalable to 4×3)
- Drag bottom-right corner to expand → widget grows
- Try to expand beyond max → clamped
- Drag a 1×1 non-scalable widget's corner → nothing happens (brackets are decorative)

**Step 5: Commit**

```bash
git commit -m "feat(grid): drag-to-resize via corner handles"
```

---

## Task 7: Widget Catalog Overlay

Half-width overlay slides in from the right. Lists available widgets for placement.

**Files:**
- Create: `ui_xml/widget_catalog_overlay.xml`
- Create: `src/ui/ui_widget_catalog_overlay.cpp`
- Create: `include/ui_widget_catalog_overlay.h`
- Modify: `src/ui/grid_edit_mode.cpp` — long-press empty area opens catalog

**Step 1: Create XML layout**

```xml
<!-- ui_xml/widget_catalog_overlay.xml -->
<component>
  <view name="widget_catalog_overlay"
        extends="overlay_panel" width="50%" title="Add Widget"
        title_tag="Add Widget">
    <lv_obj name="catalog_scroll" width="100%" flex_grow="1"
            scrollable="true" flex_flow="column" style_pad_gap="#space_sm">
      <!-- Rows populated dynamically by C++ -->
    </lv_obj>
  </view>
</component>
```

Applying [L014]: Register `widget_catalog_overlay` in `main.cpp` via `lv_xml_component_register_from_file()`.

**Step 2: Implement catalog overlay class**

`WidgetCatalogOverlay` extends `OverlayBase`. On `create()`:
1. Iterate `get_all_widget_defs()`
2. For each widget, create a row with: icon + name + size badge ("2×2")
3. Already-placed widgets: dimmed with "Placed" label
4. Hardware-gated widgets with gate value 0: hidden entirely
5. On row tap: record selected widget ID, close overlay, call placement callback

**Step 3: Placement callback**

`GridEditMode` stores the grid cell where the long-press occurred. When the catalog returns a widget ID:
1. Try to place at the long-press cell using `GridLayout::can_place()`
2. If it fits: add to config at that position, save + rebuild
3. If not: use `GridLayout::find_available()` for nearest available spot
4. If grid is full: show toast "Grid is full"

**Step 4: Wire long-press on empty area**

In the edit mode container event handler, when a long-press occurs and no widget is hit:
1. Record the grid cell from the touch point
2. Create and push the `WidgetCatalogOverlay`

**Step 5: Visual test**

Run: `./build/bin/helix-screen --test -vv`
- Enter edit mode, long-press empty area → catalog slides in from right
- Shows available widgets with icons and size badges
- Already-placed widgets are dimmed
- Tap a widget → catalog slides out, widget appears on grid

**Step 6: Commit**

```bash
git commit -m "feat(grid): widget catalog overlay for adding widgets"
```

---

## Task 8: Double-Tap Migration for Fan/Temp/LED Widgets

Migrate long-press "more" functionality to double-tap on affected widgets.

**Files:**
- Modify: `src/ui/panel_widgets/fan_stack_widget.cpp` — change long-press to double-tap
- Modify: `src/ui/panel_widgets/temp_stack_widget.cpp` — change long-press to double-tap
- Modify: `src/ui/panel_widgets/led_widget.cpp` — change long-press to double-tap
- Modify: `src/ui/ui_panel_home.cpp` — change `light_long_press_cb` to double-click handler

**Step 1: For each widget**

Replace `LV_EVENT_LONG_PRESSED` registrations with `LV_EVENT_DOUBLE_CLICKED`.

For HomePanel callbacks (`light_long_press_cb`, etc.):
- Rename to `light_double_click_cb` (or similar)
- Change event registration from `LV_EVENT_LONG_PRESSED` to `LV_EVENT_DOUBLE_CLICKED`
- Keep the same handler logic (push overlay)

For XML-registered callbacks: If any use `trigger="long_pressed"`, change to `trigger="double_clicked"`.

**Step 2: Suppress double-tap in edit mode**

In the event handler, check `grid_edit_mode_.is_active()` and suppress the double-tap action when in edit mode (edit mode owns all interactions).

**Step 3: Visual test**

Run: `./build/bin/helix-screen --test -vv`
- Double-tap LED widget → LED control overlay opens
- Double-tap fan stack → fan control overlay opens
- Long-press any widget → edit mode activates (not "more" overlay)
- In edit mode, double-tap does nothing (suppressed)

**Step 4: Commit**

```bash
git commit -m "feat(grid): migrate fan/temp/LED long-press to double-tap"
```

---

## Task 9: Code Review Checkpoint + Integration Polish

Review all changes, fix issues, run full test suite.

**Step 1: Run full test suite**

```bash
make test-run
```

All tests must pass.

**Step 2: Code review**

Use `claude-recall:review` skill. Key review areas:
- Observer/timer cleanup in `GridEditMode` destructor and `exit()`
- Applying [L059]: Use `lv_obj_safe_delete()` for LVGL cleanup
- Applying [L072]: No bare `this` capture in async callbacks
- Applying [L074]: Generation counter if deferred callbacks are used
- No dangling pointers to deleted LVGL objects
- Config persistence round-trip correctness
- Touch interaction edge cases (drag off-screen, rotate during edit mode)

**Step 3: Visual integration test**

Full workflow test:
1. Launch app → home panel renders normally
2. Long-press widget → edit mode: dots appear, widget selected with brackets
3. Drag widget to empty cell → widget moves
4. Drag widget onto same-size widget → swap
5. Select resizable widget → corner handles appear
6. Drag corner to resize → widget grows/shrinks
7. Tap (X) → widget removed
8. Long-press empty area → catalog opens
9. Add widget from catalog → placed at long-press location
10. Tap navbar checkmark → edit mode exits, layout persists
11. Kill and restart app → layout preserved

**Step 4: Commit**

```bash
git commit -m "feat(grid): grid edit mode integration polish"
```

---

## Task Order and Dependencies

```
Task 1 (skeleton) ─── required by all others
  ├── Task 2 (navbar done button)
  ├── Task 3 (grid dots)
  ├── Task 8 (double-tap migration) ── can be done early/parallel
  │
  ├── Task 4 (selection + X removal) ── requires Task 1
  │     └── Task 5 (drag-to-reposition) ── requires Task 4
  │           └── Task 6 (drag-to-resize) ── requires Task 5
  │
  └── Task 7 (widget catalog) ── requires Task 1, benefits from Task 4
        └── Task 9 (integration) ── requires all above
```

Tasks 2, 3, and 8 can be done in parallel after Task 1.
Tasks 5 and 6 are strictly sequential (each builds on the prior).

---

## Key References

| Topic | File | Lines |
|-------|------|-------|
| Existing drag-to-reorder | `src/ui/ui_settings_panel_widgets.cpp` | 307–705 |
| Grid layout + cell placement | `src/ui/panel_widget_manager.cpp` | 179–315 |
| GridLayout collision detection | `src/ui/grid_layout.cpp` | 100–148 |
| Widget registry (spans, scaling) | `include/panel_widget_registry.h` | 19–45 |
| PanelWidgetConfig persistence | `src/system/panel_widget_config.cpp` | 126–144 |
| Overlay push pattern | `src/ui/ui_panel_home.cpp` | 393–420 |
| Navbar XML | `ui_xml/navigation_bar.xml` | 1–103 |
| Home panel container | `ui_xml/home_panel.xml` | `widget_container` |
| LVGL double-click event | `lib/lvgl/src/misc/lv_event.h` | `LV_EVENT_DOUBLE_CLICKED` |
| Design doc | `docs/plans/2026-02-25-grid-edit-mode-design.md` | — |
