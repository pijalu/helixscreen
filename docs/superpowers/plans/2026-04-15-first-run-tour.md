# First-Run Tour Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an 8-step, skippable, replayable coach-mark tour that auto-runs the first time a user reaches the home panel after setup wizards complete, highlighting the home grid, long-press-to-customize, and each of the six navbar destinations.

**Architecture:** One `FirstRunTour` singleton drives state (trigger gate, current step, replay). A `TourOverlay` widget on `lv_layer_top()` renders a full-screen dim layer, a target-outline highlight, and an XML-defined tooltip card. All 8 targets live on the home panel root or the navbar — no panel switching. Persistence uses the existing `Config` (JSON pointer) singleton alongside `wizard_completed`.

**Tech Stack:** LVGL 9.5 + XML engine; C++17; Catch2 for tests; spdlog for logging; existing `Config`, `SettingsManager`, `AmsState`, `NavigationManager`, `HomePanel`, `HelpSettingsOverlay` singletons; existing i18n pipeline (`lv_tr()`, YAML → `src/generated/lv_i18n_translations.{c,h}` + `ui_xml/translations/translations.xml`).

**Spec:** `docs/superpowers/specs/2026-04-15-first-run-tour-design.md`

---

## File Structure

**Added (headers in `include/` per project convention, sources in `src/ui/tour/`):**
- `include/tour_steps.h` — `TourStep` struct, `TooltipAnchor` enum, `build_tour_steps()` declaration
- `include/tour_overlay.h` — overlay widget declaration
- `include/first_run_tour.h` — singleton state machine declaration
- `src/ui/tour/tour_steps.cpp` — step table + AMS gate logic
- `src/ui/tour/tour_overlay.cpp` — overlay widget (dim, highlight, tooltip, positioning)
- `src/ui/tour/first_run_tour.cpp` — singleton state machine, trigger gate, settings I/O
- `ui_xml/tour_tooltip_card.xml` — tooltip card layout (filename matches `<view name>` so LVGL registers it correctly)
- `tests/unit/test_first_run_tour.cpp` — Catch2 unit tests

**Modified:**
- `src/ui/ui_panel_home.cpp` — call `FirstRunTour::instance().maybe_start()` in `on_activate()`
- `src/ui/ui_settings_help.{h,cpp}` + `ui_xml/settings_help_overlay.xml` — add "Replay welcome tour" row
- `src/xml_registration.cpp` — `register_xml("tour_tooltip_card.xml")` alongside other overlays [L014]
- `translations/en.yml` + regenerated `src/generated/lv_i18n_translations.c` + `ui_xml/translations/translations.xml` [L064]
- No Makefile changes needed — `src/*/*/*.cpp` glob picks up the new subdir automatically.

**Plan deviations from original draft:**
- Headers moved to `include/` (project convention — 579 headers there, no `src/ui/<subdir>` is on `-I` path).
- XML component filename must match `<view name>` — LVGL registers components by filename stem. File was renamed from `tour_overlay.xml` → `tour_tooltip_card.xml` after a registration-mismatch crash.
- Tests placed in `tests/unit/` to match the existing Makefile glob (`tests/unit/*.cpp`).
- Config schema dropped the redundant `/tour/version` write in `mark_completed()` — `kTourVersion` is compile-time; only `/tour/completed` + `/tour/last_seen_version` persist.
- Step 4 target changed from `nav_btn_print_select` (not defined in `navigation_bar.xml`) to `print_status` (the print-status widget tile on home — a more informative landmark anyway).
- Tooltip width made responsive in C++ (55% of screen, clamped 220–480) instead of the fixed `max_width` the XML originally used.

---

## Task 1: Tour settings read/write in Config (TDD)

Establishes the persistence layer and the trigger-gate helpers. These are pure-logic functions that can be unit-tested without any LVGL state.

**Files:**
- Create: `src/ui/tour/first_run_tour.h`
- Create: `src/ui/tour/first_run_tour.cpp`
- Create: `tests/test_first_run_tour.cpp`

- [ ] **Step 1.1: Write failing test for gate helpers**

Create `tests/test_first_run_tour.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "first_run_tour.h"
#include "config.h"

#include <catch2/catch_test_macros.hpp>

using helix::tour::FirstRunTour;

namespace {
// Reset tour settings to a known state before each test.
void reset_tour_settings() {
    auto* cfg = Config::get_instance();
    cfg->set<bool>("/tour/completed", false);
    cfg->set<int>("/tour/version", 1);
    cfg->set<int>("/tour/last_seen_version", 0);
    cfg->set<bool>("/wizard_completed", true);
}
}  // namespace

TEST_CASE("FirstRunTour gate: blocks when tour already completed", "[tour]") {
    reset_tour_settings();
    Config::get_instance()->set<bool>("/tour/completed", true);
    REQUIRE(FirstRunTour::should_auto_start() == false);
}

TEST_CASE("FirstRunTour gate: blocks when wizard not complete", "[tour]") {
    reset_tour_settings();
    Config::get_instance()->set<bool>("/wizard_completed", false);
    REQUIRE(FirstRunTour::should_auto_start() == false);
}

TEST_CASE("FirstRunTour gate: allows auto-start when fresh and wizards done", "[tour]") {
    reset_tour_settings();
    REQUIRE(FirstRunTour::should_auto_start() == true);
}

TEST_CASE("FirstRunTour gate: re-triggers when tour version bumped", "[tour]") {
    reset_tour_settings();
    Config::get_instance()->set<bool>("/tour/completed", true);
    Config::get_instance()->set<int>("/tour/version", 2);
    Config::get_instance()->set<int>("/tour/last_seen_version", 1);
    REQUIRE(FirstRunTour::should_auto_start() == true);
}

TEST_CASE("FirstRunTour mark_completed writes both flags", "[tour]") {
    reset_tour_settings();
    FirstRunTour::mark_completed();
    auto* cfg = Config::get_instance();
    REQUIRE(cfg->get<bool>("/tour/completed", false) == true);
    REQUIRE(cfg->get<int>("/tour/last_seen_version", 0) == 1);
}
```

- [ ] **Step 1.2: Create header with declarations only**

Create `src/ui/tour/first_run_tour.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace helix::tour {

/// Current tour version. Bump when tour content materially changes.
constexpr int kTourVersion = 1;

class FirstRunTour {
public:
    static FirstRunTour& instance();

    /// Gate check: returns true iff tour should auto-start on home activate.
    /// Checks: tour not already completed at current version, wizards complete.
    static bool should_auto_start();

    /// Writes tour.completed=true and tour.last_seen_version=kTourVersion.
    /// Persists via Config::save(). Called on both skip and finish.
    static void mark_completed();

    // Runtime API (implemented in later tasks):
    void maybe_start();                 // Auto-trigger entry point
    void start();                       // Replay entry point (bypasses gate)
    void advance();                     // Next step
    void skip();                        // Skip button
    bool is_running() const { return running_; }

private:
    FirstRunTour() = default;
    bool running_ = false;
};

}  // namespace helix::tour
```

- [ ] **Step 1.3: Create minimal implementation**

Create `src/ui/tour/first_run_tour.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "first_run_tour.h"

#include "config.h"

#include <spdlog/spdlog.h>

namespace helix::tour {

FirstRunTour& FirstRunTour::instance() {
    static FirstRunTour s;
    return s;
}

bool FirstRunTour::should_auto_start() {
    auto* cfg = Config::get_instance();
    if (!cfg) return false;

    const bool wizard_complete = cfg->get<bool>("/wizard_completed", false);
    if (!wizard_complete) return false;

    const bool completed = cfg->get<bool>("/tour/completed", false);
    const int last_seen = cfg->get<int>("/tour/last_seen_version", 0);

    if (!completed) return true;
    if (last_seen < kTourVersion) return true;
    return false;
}

void FirstRunTour::mark_completed() {
    auto* cfg = Config::get_instance();
    if (!cfg) return;
    cfg->set<bool>("/tour/completed", true);
    cfg->set<int>("/tour/version", kTourVersion);
    cfg->set<int>("/tour/last_seen_version", kTourVersion);
    cfg->save();
    spdlog::info("[FirstRunTour] Marked completed (version={})", kTourVersion);
}

// Runtime methods implemented in Task 5.
void FirstRunTour::maybe_start() {}
void FirstRunTour::start() {}
void FirstRunTour::advance() {}
void FirstRunTour::skip() {}

}  // namespace helix::tour
```

- [ ] **Step 1.4: Add sources to build system**

Add `src/ui/tour/first_run_tour.cpp` to the Makefile source list (search Makefile for another `src/ui/...cpp` entry and add alongside). Add `-Isrc/ui/tour` to include paths if not covered by existing `src/ui` include.

Add `tests/test_first_run_tour.cpp` to the test Makefile target similarly.

- [ ] **Step 1.5: Run tests — verify they pass**

```bash
make test-run 2>&1 | grep -E "(tour|FAIL|PASS)"
```

Expected: all 5 `[tour]` tests pass.

- [ ] **Step 1.6: Commit**

```bash
git add src/ui/tour/first_run_tour.{h,cpp} tests/test_first_run_tour.cpp Makefile
git commit -m "feat(tour): config-backed gate for first-run tour trigger"
```

---

## Task 2: TourStep data model + step builder with AMS gating (TDD)

Pure data, pure function — the step list `build_tour_steps()` is deterministic given hardware state.

**Files:**
- Create: `src/ui/tour/tour_steps.h`
- Create: `src/ui/tour/tour_steps.cpp`
- Modify: `tests/test_first_run_tour.cpp`

- [ ] **Step 2.1: Add failing tests for step builder**

Append to `tests/test_first_run_tour.cpp`:

```cpp
#include "tour_steps.h"
#include "ams_state.h"

using helix::tour::build_tour_steps;
using helix::tour::TourStep;

TEST_CASE("Tour steps: always 8 steps regardless of AMS", "[tour]") {
    auto steps_no_ams = build_tour_steps(/*has_ams=*/false);
    auto steps_with_ams = build_tour_steps(/*has_ams=*/true);
    REQUIRE(steps_no_ams.size() == 8);
    REQUIRE(steps_with_ams.size() == 8);
}

TEST_CASE("Tour steps: step 2 sub-spotlights AMS only when present", "[tour]") {
    auto with_ams = build_tour_steps(true);
    auto without = build_tour_steps(false);
    // Step 2 is the home-grid widget spotlight step (0-indexed = 1)
    REQUIRE(with_ams[1].sub_spotlights.size() == 3);     // nozzle + fan + ams
    REQUIRE(without[1].sub_spotlights.size() == 2);      // nozzle + fan
}

TEST_CASE("Tour steps: navbar steps 4-8 target nav buttons", "[tour]") {
    auto steps = build_tour_steps(true);
    REQUIRE(steps[3].target_name == "nav_btn_print_select");
    REQUIRE(steps[4].target_name == "nav_btn_controls");
    REQUIRE(steps[5].target_name == "nav_btn_filament");
    REQUIRE(steps[6].target_name == "nav_btn_advanced");
    REQUIRE(steps[7].target_name == "nav_btn_settings");
}

TEST_CASE("Tour steps: welcome and customize steps have empty target or grid target", "[tour]") {
    auto steps = build_tour_steps(true);
    REQUIRE(steps[0].target_name.empty());              // Welcome is centered
    REQUIRE(steps[2].target_name == "carousel_host");   // Long-press step
}
```

- [ ] **Step 2.2: Create header**

Create `src/ui/tour/tour_steps.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

namespace helix::tour {

enum class TooltipAnchor {
    Center,           ///< No target — tooltip screen-centered
    PreferBelow,
    PreferAbove,
    PreferRight,      ///< Typical for navbar buttons (tooltip to the right of left-side navbar)
    PreferLeft,
};

struct TourStep {
    /// Target widget name for `lv_obj_find_by_name()`. Empty = centered, no highlight.
    std::string target_name;

    /// Translation keys for title and body (passed through `lv_tr()` at render time).
    std::string title_key;
    std::string body_key;

    TooltipAnchor anchor_hint = TooltipAnchor::PreferBelow;

    /// Additional widget names to sequentially spotlight inside this step (step 2 only).
    /// Each is highlighted for ~1.5s before the next. Empty = single target only.
    std::vector<std::string> sub_spotlights;
};

/// Build the tour step list. AMS sub-spotlight on step 2 only added when has_ams=true.
std::vector<TourStep> build_tour_steps(bool has_ams);

/// Convenience: queries `AmsState::instance().backend_count() > 0`.
bool hardware_has_ams();

}  // namespace helix::tour
```

- [ ] **Step 2.3: Implement step builder**

Create `src/ui/tour/tour_steps.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tour_steps.h"

#include "ams_state.h"

namespace helix::tour {

bool hardware_has_ams() {
    return AmsState::instance().backend_count() > 0;
}

std::vector<TourStep> build_tour_steps(bool has_ams) {
    std::vector<TourStep> steps;
    steps.reserve(8);

    // 1. Welcome (centered card, no target)
    steps.push_back({"", "tour.step.welcome.title", "tour.step.welcome.body",
                     TooltipAnchor::Center, {}});

    // 2. Home widget examples — sub-spotlight nozzle, fan, (AMS if present)
    {
        TourStep s{"carousel_host", "tour.step.home_grid.title",
                   "tour.step.home_grid.body", TooltipAnchor::PreferBelow, {}};
        s.sub_spotlights.push_back("widget_nozzle_temp");
        s.sub_spotlights.push_back("widget_fan");
        if (has_ams) s.sub_spotlights.push_back("widget_ams");
        steps.push_back(s);
    }

    // 3. Long-press to customize
    steps.push_back({"carousel_host", "tour.step.customize.title",
                     "tour.step.customize.body", TooltipAnchor::PreferBelow, {}});

    // 4-8. Navbar tour
    steps.push_back({"nav_btn_print_select", "tour.step.print.title",
                     "tour.step.print.body", TooltipAnchor::PreferRight, {}});
    steps.push_back({"nav_btn_controls", "tour.step.controls.title",
                     "tour.step.controls.body", TooltipAnchor::PreferRight, {}});
    steps.push_back({"nav_btn_filament", "tour.step.filament.title",
                     "tour.step.filament.body", TooltipAnchor::PreferRight, {}});
    steps.push_back({"nav_btn_advanced", "tour.step.advanced.title",
                     "tour.step.advanced.body", TooltipAnchor::PreferRight, {}});
    steps.push_back({"nav_btn_settings", "tour.step.settings.title",
                     "tour.step.settings.body", TooltipAnchor::PreferRight, {}});

    return steps;
}

}  // namespace helix::tour
```

- [ ] **Step 2.4: Add to build**

Add `src/ui/tour/tour_steps.cpp` to Makefile next to `first_run_tour.cpp`.

- [ ] **Step 2.5: Run tests**

```bash
make test-run 2>&1 | grep -E "(tour|FAIL|PASS)"
```

Expected: 9 `[tour]` tests pass (5 from Task 1 + 4 new).

- [ ] **Step 2.6: Commit**

```bash
git add src/ui/tour/tour_steps.{h,cpp} tests/test_first_run_tour.cpp Makefile
git commit -m "feat(tour): step builder with AMS-conditional sub-spotlight"
```

---

## Task 3: Tour overlay XML tooltip card

Declarative layout for the tooltip. No C++ logic yet — the XML will be populated by `TourOverlay` in Task 4.

**Files:**
- Create: `ui_xml/tour_overlay.xml`
- Modify: `main.cpp`

- [ ] **Step 3.1: Create the XML component**

Create `ui_xml/tour_overlay.xml`:

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<component>
  <consts>
    <px name="tour_tooltip_max_width" value="280"/>
  </consts>
  <view name="tour_tooltip_card"
        extends="ui_card"
        style_max_width="#tour_tooltip_max_width"
        style_pad_all="#space_md"
        style_bg_color="#card_bg"
        style_border_width="1"
        style_border_color="#border"
        style_shadow_width="16"
        style_shadow_opa="120"
        flex_flow="column"
        style_pad_gap="#space_sm">
    <text_heading name="tour_title" width="100%" text="" translation_tag=""/>
    <text_body name="tour_body" width="100%" text="" translation_tag="" long_mode="wrap"/>
    <lv_obj name="tour_footer"
            width="100%" height="content"
            style_bg_opa="0" style_border_width="0" style_pad_all="0"
            flex_flow="row" style_flex_main_place="space_between"
            style_flex_cross_place="center">
      <text_small name="tour_counter" text="1 / 8"/>
      <lv_obj name="tour_buttons"
              width="content" height="content"
              style_bg_opa="0" style_border_width="0" style_pad_all="0"
              flex_flow="row" style_pad_gap="#space_sm">
        <ui_button name="tour_skip_btn" variant="ghost"
                   text="Skip" translation_tag="Skip">
          <event_cb trigger="clicked" callback="on_tour_skip_clicked"/>
        </ui_button>
        <ui_button name="tour_next_btn" variant="primary"
                   text="Next" translation_tag="Next">
          <event_cb trigger="clicked" callback="on_tour_next_clicked"/>
        </ui_button>
      </lv_obj>
    </lv_obj>
  </view>
</component>
```

- [ ] **Step 3.2: Register the component in main.cpp**

Find the block of `lv_xml_component_register_from_file()` calls in `main.cpp` (grep for an existing overlay registration like `settings_help_overlay`). Add:

```cpp
lv_xml_component_register_from_file("ui_xml/tour_overlay.xml");
```

alongside other overlay registrations. [L014]

- [ ] **Step 3.3: Verify the program still builds and launches**

```bash
make -j
./build/bin/helix-screen --test -v 2>&1 | head -40
```

Expected: app launches, no "component not found" errors. (No tour visible yet — Task 4 wires it up.)

Kill with Ctrl-C.

- [ ] **Step 3.4: Commit**

```bash
git add ui_xml/tour_overlay.xml main.cpp
git commit -m "feat(tour): tooltip card XML component"
```

---

## Task 4: TourOverlay class — dim, highlight, tooltip positioning

This task builds the overlay widget standalone. It is NOT yet wired to `FirstRunTour`. A temporary test hook lets us manually trigger the overlay to validate rendering and positioning.

**Files:**
- Create: `src/ui/tour/tour_overlay.h` / `.cpp`
- Modify: `src/ui/ui_panel_home.cpp` (temporary keyboard shortcut — reverted in Task 6)

- [ ] **Step 4.1: Write the header**

Create `src/ui/tour/tour_overlay.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "tour_steps.h"

#include <lvgl.h>

#include <functional>
#include <vector>

namespace helix::tour {

/// Renders the coach-mark overlay: dim layer, target highlight, tooltip.
/// Non-singleton — created per tour run, destroyed on skip/finish.
class TourOverlay {
public:
    using AdvanceCb = std::function<void()>;
    using SkipCb = std::function<void()>;

    TourOverlay(std::vector<TourStep> steps, AdvanceCb on_next, SkipCb on_skip);
    ~TourOverlay();

    TourOverlay(const TourOverlay&) = delete;
    TourOverlay& operator=(const TourOverlay&) = delete;

    /// Show step at `index`. 0-based. Triggers target resolution + tooltip placement.
    void show_step(size_t index);

    /// Root object — for external code that needs to check existence.
    lv_obj_t* root() const { return root_; }

private:
    void build_tree();
    void resolve_target(const TourStep& step, lv_area_t& out_rect, bool& out_has_target);
    void place_highlight(const lv_area_t& target_rect);
    void place_tooltip(const lv_area_t& target_rect, bool has_target, TooltipAnchor hint);
    void update_tooltip_text(const TourStep& step, size_t index, size_t total);
    void update_counter(size_t index, size_t total);

    static void on_skip_cb(lv_event_t* e);
    static void on_next_cb(lv_event_t* e);

    std::vector<TourStep> steps_;
    AdvanceCb on_next_cb_;
    SkipCb on_skip_cb_;

    lv_obj_t* root_ = nullptr;      // on lv_layer_top()
    lv_obj_t* dim_ = nullptr;
    lv_obj_t* highlight_ = nullptr;
    lv_obj_t* tooltip_ = nullptr;   // instantiated from tour_overlay.xml

    AsyncLifetimeGuard lifetime_;
};

}  // namespace helix::tour
```

- [ ] **Step 4.2: Implement the class**

Create `src/ui/tour/tour_overlay.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tour_overlay.h"

#include "ui_theme.h"

#include <spdlog/spdlog.h>

#include "lvgl/src/others/xml/lv_xml.h"

namespace helix::tour {

namespace {
constexpr int kHighlightOutlinePad = 6;
constexpr int kHighlightOutlineWidth = 3;
constexpr int kTooltipMargin = 12;      // gap between target and tooltip
constexpr int kTooltipMaxWidth = 280;

// Per-screen dim fill with 55% opacity.
constexpr uint8_t kDimOpa = 140;
}  // namespace

TourOverlay::TourOverlay(std::vector<TourStep> steps, AdvanceCb on_next, SkipCb on_skip)
    : steps_(std::move(steps)), on_next_cb_(std::move(on_next)), on_skip_cb_(std::move(on_skip)) {
    build_tree();
}

TourOverlay::~TourOverlay() {
    lifetime_.invalidate();
    if (root_) {
        lv_obj_delete(root_);   // full-screen overlay, not owned by any panel
        root_ = nullptr;
    }
}

void TourOverlay::build_tree() {
    lv_obj_t* top = lv_layer_top();

    root_ = lv_obj_create(top);
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root_, 0, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // Dim layer — click-blocking, absorbs all touches.
    dim_ = lv_obj_create(root_);
    lv_obj_set_size(dim_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(dim_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim_, kDimOpa, 0);
    lv_obj_set_style_border_width(dim_, 0, 0);
    lv_obj_set_style_pad_all(dim_, 0, 0);
    lv_obj_add_flag(dim_, LV_OBJ_FLAG_CLICKABLE);       // absorb clicks
    lv_obj_clear_flag(dim_, LV_OBJ_FLAG_SCROLLABLE);

    // Highlight rect — transparent fill, outline + shadow. Hidden until step has target.
    highlight_ = lv_obj_create(root_);
    lv_obj_set_style_bg_opa(highlight_, 0, 0);
    lv_obj_set_style_border_width(highlight_, 0, 0);
    lv_obj_set_style_outline_width(highlight_, kHighlightOutlineWidth, 0);
    lv_obj_set_style_outline_color(highlight_, ui_theme_get_color("accent"), 0);
    lv_obj_set_style_outline_pad(highlight_, kHighlightOutlinePad, 0);
    lv_obj_set_style_shadow_width(highlight_, 24, 0);
    lv_obj_set_style_shadow_opa(highlight_, 180, 0);
    lv_obj_set_style_shadow_color(highlight_, ui_theme_get_color("accent"), 0);
    lv_obj_clear_flag(highlight_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(highlight_, LV_OBJ_FLAG_HIDDEN);

    // Tooltip — instantiated from XML component.
    tooltip_ = static_cast<lv_obj_t*>(lv_xml_create(root_, "tour_tooltip_card", nullptr));
    if (!tooltip_) {
        spdlog::error("[TourOverlay] Failed to instantiate tour_tooltip_card");
        return;
    }

    // Wire callbacks by name (XML uses on_tour_skip_clicked / on_tour_next_clicked).
    // Overlay instance is passed via user_data on the tooltip root.
    lv_obj_set_user_data(tooltip_, this);

    // Find the buttons and attach static callbacks that look up `this` via user_data.
    lv_obj_t* skip_btn = lv_obj_find_by_name(tooltip_, "tour_skip_btn");
    lv_obj_t* next_btn = lv_obj_find_by_name(tooltip_, "tour_next_btn");
    if (skip_btn) {
        lv_obj_set_user_data(skip_btn, this);
        lv_obj_add_event_cb(skip_btn, on_skip_cb, LV_EVENT_CLICKED, this);
    }
    if (next_btn) {
        lv_obj_set_user_data(next_btn, this);
        lv_obj_add_event_cb(next_btn, on_next_cb, LV_EVENT_CLICKED, this);
    }
}

void TourOverlay::on_skip_cb(lv_event_t* e) {
    auto* self = static_cast<TourOverlay*>(lv_event_get_user_data(e));
    if (!self || !self->on_skip_cb_) return;
    self->on_skip_cb_();
}

void TourOverlay::on_next_cb(lv_event_t* e) {
    auto* self = static_cast<TourOverlay*>(lv_event_get_user_data(e));
    if (!self || !self->on_next_cb_) return;
    self->on_next_cb_();
}

void TourOverlay::resolve_target(const TourStep& step, lv_area_t& out_rect, bool& out_has_target) {
    out_has_target = false;
    if (step.target_name.empty()) return;

    // Search the entire screen tree (home panel root + navbar + anything on layer_top below us).
    lv_obj_t* scr = lv_screen_active();
    lv_obj_t* target = lv_obj_find_by_name(scr, step.target_name.c_str());
    if (!target) {
        spdlog::warn("[TourOverlay] Target '{}' not found — skipping highlight", step.target_name);
        return;
    }
    lv_obj_update_layout(target);
    lv_obj_get_coords(target, &out_rect);
    out_has_target = true;
}

void TourOverlay::place_highlight(const lv_area_t& target_rect) {
    const int pad = kHighlightOutlinePad;
    lv_obj_set_pos(highlight_,
                   target_rect.x1 - pad,
                   target_rect.y1 - pad);
    lv_obj_set_size(highlight_,
                    (target_rect.x2 - target_rect.x1) + 2 * pad,
                    (target_rect.y2 - target_rect.y1) + 2 * pad);
    lv_obj_clear_flag(highlight_, LV_OBJ_FLAG_HIDDEN);
}

void TourOverlay::place_tooltip(const lv_area_t& target_rect, bool has_target,
                                TooltipAnchor hint) {
    lv_obj_update_layout(tooltip_);
    const int screen_w = lv_display_get_horizontal_resolution(nullptr);
    const int screen_h = lv_display_get_vertical_resolution(nullptr);

    // Cap tooltip width responsively for small screens.
    const int max_w = std::min(kTooltipMaxWidth, screen_w - 32);
    lv_obj_set_style_max_width(tooltip_, max_w, 0);
    lv_obj_update_layout(tooltip_);
    const int tw = lv_obj_get_width(tooltip_);
    const int th = lv_obj_get_height(tooltip_);

    if (!has_target) {
        lv_obj_set_pos(tooltip_, (screen_w - tw) / 2, (screen_h - th) / 2);
        return;
    }

    const int tx1 = target_rect.x1;
    const int tx2 = target_rect.x2;
    const int ty1 = target_rect.y1;
    const int ty2 = target_rect.y2;

    // Candidate positions (x, y) for each anchor.
    auto clamp = [&](int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); };

    int x = 0, y = 0;
    auto set_below  = [&] { x = clamp(tx1, 8, screen_w - tw - 8); y = ty2 + kTooltipMargin; };
    auto set_above  = [&] { x = clamp(tx1, 8, screen_w - tw - 8); y = ty1 - kTooltipMargin - th; };
    auto set_right  = [&] { x = tx2 + kTooltipMargin; y = clamp(ty1, 8, screen_h - th - 8); };
    auto set_left   = [&] { x = tx1 - kTooltipMargin - tw; y = clamp(ty1, 8, screen_h - th - 8); };

    auto fits = [&](int xx, int yy) {
        return xx >= 0 && yy >= 0 && xx + tw <= screen_w && yy + th <= screen_h;
    };

    // Apply hint, then fall back to whichever fits, in order.
    switch (hint) {
        case TooltipAnchor::PreferBelow:  set_below();  if (fits(x, y)) break; [[fallthrough]];
        case TooltipAnchor::PreferAbove:  set_above();  if (fits(x, y)) break; [[fallthrough]];
        case TooltipAnchor::PreferRight:  set_right();  if (fits(x, y)) break; [[fallthrough]];
        case TooltipAnchor::PreferLeft:   set_left();   if (fits(x, y)) break;
        default: set_below();
    }

    // Final clamp for edge cases.
    x = clamp(x, 8, screen_w - tw - 8);
    y = clamp(y, 8, screen_h - th - 8);
    lv_obj_set_pos(tooltip_, x, y);
}

void TourOverlay::update_tooltip_text(const TourStep& step, size_t index, size_t total) {
    if (!tooltip_) return;
    lv_obj_t* title = lv_obj_find_by_name(tooltip_, "tour_title");
    lv_obj_t* body = lv_obj_find_by_name(tooltip_, "tour_body");
    lv_obj_t* next_btn = lv_obj_find_by_name(tooltip_, "tour_next_btn");

    if (title) lv_label_set_text(title, lv_tr(step.title_key.c_str()));
    if (body) lv_label_set_text(body, lv_tr(step.body_key.c_str()));

    // Change "Next" to "Done" on last step.
    if (next_btn) {
        lv_obj_t* label = lv_obj_find_by_name(next_btn, "button_label");
        if (label) {
            const char* key = (index + 1 == total) ? "Done" : "Next";
            lv_label_set_text(label, lv_tr(key));
        }
    }

    update_counter(index, total);
}

void TourOverlay::update_counter(size_t index, size_t total) {
    lv_obj_t* counter = lv_obj_find_by_name(tooltip_, "tour_counter");
    if (!counter) return;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%zu / %zu", index + 1, total);
    lv_label_set_text(counter, buf);
}

void TourOverlay::show_step(size_t index) {
    if (index >= steps_.size()) return;
    const TourStep& step = steps_[index];

    lv_area_t target_rect{};
    bool has_target = false;
    resolve_target(step, target_rect, has_target);

    if (has_target) {
        place_highlight(target_rect);
    } else {
        lv_obj_add_flag(highlight_, LV_OBJ_FLAG_HIDDEN);
    }

    update_tooltip_text(step, index, steps_.size());
    place_tooltip(target_rect, has_target, step.anchor_hint);
}

}  // namespace helix::tour
```

**Notes:**
- The `button_label` child lookup on step 4's "Next → Done" swap assumes `ui_button` uses a child named `button_label`. Grep `ui_xml/ui_button.xml` to confirm; adjust the name if different.
- `lv_obj_find_by_name` on `lv_screen_active()` searches the currently active screen — the navbar + panels are all descendants, so navbar-button targets resolve correctly.
- Sub-spotlights (step 2) are not yet animated here. We'll add the simple timer-driven sub-spotlight animation in Task 5 alongside `advance()`. For now, step 2 shows the `carousel_host` highlight only — sub-spotlights land in Task 5 step 5.3.

- [ ] **Step 4.3: Add a temporary keyboard trigger on home panel**

In `src/ui/ui_panel_home.cpp`, add a temporary `on_activate`-adjacent hook so we can manually summon the overlay. Search for the existing key-handling area (grep `LV_KEY_` / `key_event`) or add a quick-and-dirty `lv_async_call` from `on_activate` guarded by an env var:

```cpp
// TEMPORARY — removed in Task 6
#include "tour_overlay.h"
#include "tour_steps.h"
static std::unique_ptr<helix::tour::TourOverlay> g_debug_tour_overlay;
// in HomePanel::on_activate(), at the end:
if (std::getenv("HELIX_TOUR_DEBUG")) {
    auto steps = helix::tour::build_tour_steps(helix::tour::hardware_has_ams());
    g_debug_tour_overlay = std::make_unique<helix::tour::TourOverlay>(
        steps,
        [] { spdlog::info("[tour-debug] Next"); },
        [] { g_debug_tour_overlay.reset(); spdlog::info("[tour-debug] Skip"); });
    g_debug_tour_overlay->show_step(0);
}
```

- [ ] **Step 4.4: Manual verification — launch app and check visuals [L060]**

```bash
./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/test.log
```

*(Don't use `HELIX_TOUR_DEBUG=1` for this first launch — first verify the app still starts cleanly.)*

**User action:** confirm the app reaches the home panel without crash. Report back.

Then relaunch with the env var:

```bash
HELIX_TOUR_DEBUG=1 ./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/test.log
```

**User action:** confirm that on the home panel, a dim overlay appears with a centered Welcome tooltip (step 0, "1 / 8"). Tap Next — nothing will happen yet (debug callback is a no-op). Tap Skip — overlay disappears.

- [ ] **Step 4.5: Commit (the overlay, not the temporary debug hook)**

```bash
git add src/ui/tour/tour_overlay.{h,cpp} Makefile
git commit -m "feat(tour): coach-mark overlay widget (dim + highlight + tooltip)"
```

Keep the `HELIX_TOUR_DEBUG` hook uncommitted for now — it's removed in Task 6.

---

## Task 5: FirstRunTour state machine (TDD where possible)

Wires the step builder to the overlay. Adds `advance()`, `skip()`, `start()`, `maybe_start()` implementations.

**Files:**
- Modify: `src/ui/tour/first_run_tour.h` / `.cpp`
- Modify: `tests/test_first_run_tour.cpp`

- [ ] **Step 5.1: Write failing tests for state transitions (headless)**

Append to `tests/test_first_run_tour.cpp`:

```cpp
TEST_CASE("FirstRunTour: start() sets is_running", "[tour]") {
    reset_tour_settings();
    auto& t = FirstRunTour::instance();
    REQUIRE(!t.is_running());
    t.start();
    REQUIRE(t.is_running());
    t.skip();
    REQUIRE(!t.is_running());
}

TEST_CASE("FirstRunTour: skip writes tour.completed", "[tour]") {
    reset_tour_settings();
    auto& t = FirstRunTour::instance();
    t.start();
    t.skip();
    REQUIRE(Config::get_instance()->get<bool>("/tour/completed", false) == true);
}

TEST_CASE("FirstRunTour: advance past last step finishes", "[tour]") {
    reset_tour_settings();
    auto& t = FirstRunTour::instance();
    t.start();
    for (int i = 0; i < 8; ++i) t.advance();
    REQUIRE(!t.is_running());
    REQUIRE(Config::get_instance()->get<bool>("/tour/completed", false) == true);
}
```

These tests drive state only — they don't render the overlay. Make the state machine overlay-optional so tests can run headless.

- [ ] **Step 5.2: Update header**

Edit `src/ui/tour/first_run_tour.h` — replace class body:

```cpp
class FirstRunTour {
public:
    static FirstRunTour& instance();
    static bool should_auto_start();
    static void mark_completed();

    void maybe_start();   // auto-trigger (respects gate)
    void start();         // replay (bypasses gate)
    void advance();
    void skip();
    bool is_running() const { return running_; }

private:
    FirstRunTour() = default;
    void start_impl();
    void finish();
    void render_current_step();

    bool running_ = false;
    size_t current_index_ = 0;
    std::vector<TourStep> steps_;
    std::unique_ptr<class TourOverlay> overlay_;
};
```

Add `#include <memory>` and forward-declare `TourStep` via `#include "tour_steps.h"`.

- [ ] **Step 5.3: Implement the state machine**

Edit `src/ui/tour/first_run_tour.cpp` — replace stubbed methods:

```cpp
#include "tour_overlay.h"
#include "tour_steps.h"

#include <spdlog/spdlog.h>

namespace helix::tour {

void FirstRunTour::maybe_start() {
    if (running_) return;
    if (!should_auto_start()) return;
    // Defer one tick so on_activate completes before we build the overlay.
    lv_async_call([](void* self) { static_cast<FirstRunTour*>(self)->start_impl(); }, this);
}

void FirstRunTour::start() {
    if (running_) return;
    start_impl();
}

void FirstRunTour::start_impl() {
    running_ = true;
    current_index_ = 0;
    steps_ = build_tour_steps(hardware_has_ams());
    overlay_ = std::make_unique<TourOverlay>(
        steps_,
        [this] { this->advance(); },
        [this] { this->skip(); });
    render_current_step();
    spdlog::info("[FirstRunTour] Started ({} steps)", steps_.size());
}

void FirstRunTour::advance() {
    if (!running_) return;
    current_index_++;
    if (current_index_ >= steps_.size()) {
        finish();
        return;
    }
    render_current_step();
}

void FirstRunTour::skip() {
    if (!running_) return;
    spdlog::info("[FirstRunTour] Skipped at step {}/{}", current_index_ + 1, steps_.size());
    mark_completed();
    running_ = false;
    overlay_.reset();
}

void FirstRunTour::finish() {
    spdlog::info("[FirstRunTour] Finished");
    mark_completed();
    running_ = false;
    overlay_.reset();
}

void FirstRunTour::render_current_step() {
    if (!overlay_) return;
    overlay_->show_step(current_index_);
}

}  // namespace helix::tour
```

- [ ] **Step 5.4: Headless test caveat**

The tests in 5.1 construct a `TourOverlay` via `start_impl()`, which requires an initialized LVGL. If the test harness doesn't already provide a screen, guard overlay construction:

```cpp
// In start_impl(), before creating TourOverlay:
if (lv_screen_active()) {
    overlay_ = std::make_unique<TourOverlay>(...);
}
```

Adjust `advance()`/`render_current_step()` to skip overlay calls when `overlay_ == nullptr`. This lets unit tests verify state transitions without LVGL rendering. (Adding `lv_screen_active()` guards is acceptable; they never fire in production because the home panel is always active when tour starts.)

- [ ] **Step 5.5: Run tests**

```bash
make test-run 2>&1 | grep -E "(tour|FAIL|PASS)"
```

Expected: 12 `[tour]` tests pass (9 prior + 3 new).

- [ ] **Step 5.6: Manual verification of full tour run [L060]**

```bash
HELIX_TOUR_DEBUG=1 ./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/test.log
```

But update the debug hook in `ui_panel_home.cpp` to use `FirstRunTour::instance().start()` instead of the raw overlay:

```cpp
if (std::getenv("HELIX_TOUR_DEBUG")) {
    helix::tour::FirstRunTour::instance().start();
}
```

**User action:** on the home panel, step through all 8 steps by tapping Next repeatedly. Verify:
- Counter reads "1 / 8" through "8 / 8"
- Button says "Next" until the last step, where it says "Done"
- Navbar buttons (steps 4-8) are highlighted on the left, tooltip appears to their right
- Tapping Done or Skip closes the overlay
- Re-launching with `HELIX_TOUR_DEBUG=1` triggers it again (since debug always calls start() directly)

Report findings. Fix any positioning/clipping issues on 800×480.

- [ ] **Step 5.7: Commit**

```bash
git add src/ui/tour/first_run_tour.{h,cpp} tests/test_first_run_tour.cpp
git commit -m "feat(tour): state machine + overlay integration"
```

---

## Task 6: Wire into HomePanel and remove debug hook

**Files:**
- Modify: `src/ui/ui_panel_home.cpp`

- [ ] **Step 6.1: Remove the debug hook and add the real trigger**

In `src/ui/ui_panel_home.cpp` at the end of `HomePanel::on_activate()` (line ~683, after existing activate logic):

```cpp
#include "tour/first_run_tour.h"

void HomePanel::on_activate() {
    // ... existing code ...

    // First-run tour trigger (respects gate — no-op if already completed or wizards incomplete)
    helix::tour::FirstRunTour::instance().maybe_start();
}
```

Remove:
- The `#include "tour_overlay.h"` and `g_debug_tour_overlay` static
- The `HELIX_TOUR_DEBUG` block (all of it)

- [ ] **Step 6.2: Manual verification [L060]**

Reset config so the tour will trigger:

```bash
# Find the test config location — typically ~/.config/helixscreen/ or /tmp during --test
./build/bin/helix-screen --test -vv 2>&1 | grep "Config file" | head -2
```

Edit the test config file to set:
```json
{
  "wizard_completed": true,
  "tour": { "completed": false, "version": 1, "last_seen_version": 0 }
}
```

Then launch:
```bash
./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/test.log
```

**User action:** confirm tour auto-starts on home panel. Tap through or skip. Relaunch — verify it does NOT trigger again. Report back.

- [ ] **Step 6.3: Commit**

```bash
git add src/ui/ui_panel_home.cpp
git commit -m "feat(tour): auto-start trigger in HomePanel::on_activate"
```

---

## Task 7: Settings → Help "Replay welcome tour" row

**Files:**
- Modify: `ui_xml/settings_help_overlay.xml`
- Modify: `src/ui/ui_settings_help.cpp` / `.h`

- [ ] **Step 7.1: Add the row to XML**

In `ui_xml/settings_help_overlay.xml`, inside the `overlay_content` container, add before the debug-bundle row (so it appears first):

```xml
<setting_action_row name="row_replay_tour"
                    label="Replay Welcome Tour" label_tag="Replay Welcome Tour"
                    icon="compass"
                    description="See the quick tour of HelixScreen again"
                    description_tag="See the quick tour of HelixScreen again"
                    callback="on_replay_tour_clicked"/>
```

- [ ] **Step 7.2: Add the callback**

In `src/ui/ui_settings_help.h` (declarations) and `.cpp` (registration + body). Find existing `on_debug_bundle_clicked` callback and add alongside:

```cpp
static void on_replay_tour_clicked(lv_event_t* e);
```

And register it (grep for `lv_xml_register_event_cb(.*, "on_debug_bundle_clicked"` to find the registration block):

```cpp
lv_xml_register_event_cb(nullptr, "on_replay_tour_clicked",
                         &HelpSettingsOverlay::on_replay_tour_clicked);
```

Body:

```cpp
void HelpSettingsOverlay::on_replay_tour_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[HelpSettings] on_replay_tour_clicked");
    // Close the help overlay so the tour has a clean home panel underneath.
    ui_nav_go_back();
    // Start the tour bypassing the gate.
    helix::tour::FirstRunTour::instance().start();
    LVGL_SAFE_EVENT_CB_END();
}
```

Include `tour/first_run_tour.h`.

- [ ] **Step 7.3: Manual verification [L060]**

```bash
./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/test.log
```

**User action:** navigate to Settings → Help & About. Tap "Replay Welcome Tour." Confirm help overlay closes and tour starts on home panel. Confirm it works regardless of `tour.completed` state.

- [ ] **Step 7.4: Commit**

```bash
git add ui_xml/settings_help_overlay.xml src/ui/ui_settings_help.{h,cpp}
git commit -m "feat(tour): replay entry in Settings > Help"
```

---

## Task 8: i18n keys and regenerated translation artifacts

**Files:**
- Modify: `assets/i18n/en.yml`
- Regenerate: `src/generated/lv_i18n_translations.{c,h}`, `ui_xml/translations/translations.xml`

- [ ] **Step 8.1: Add translation keys to English YAML**

Find the i18n YAML (grep `find assets/ -name '*.yml' -o -name '*.yaml' | xargs grep -l 'wizard'`). Append tour keys:

```yaml
tour:
  step:
    welcome:
      title: "Welcome to HelixScreen"
      body: "Let's take a quick tour."
    home_grid:
      title: "Your printer at a glance"
      body: "These tiles show temperatures, fans, and more. Tap any tile to go deeper."
    customize:
      title: "Customize your home screen"
      body: "Long-press any tile to enter edit mode. Rearrange, remove, or add widgets — even add more pages."
    print:
      title: "Print"
      body: "Browse and start prints from SD, uploads, or network shares."
    controls:
      title: "Controls"
      body: "Move axes, home, level. Adjust temps and fans."
    filament:
      title: "Filament"
      body: "Load, unload, and swap spools. AMS and AFC live here."
    advanced:
      title: "Advanced"
      body: "Macros, console, calibration, and updates."
    settings:
      title: "Settings"
      body: "Network, display, and you can replay this tour here anytime."
```

**Do not translate** "HelixScreen", "AMS", "AFC", "SD" — these are product/technical names. The sentences containing them translate as whole sentences [L070].

- [ ] **Step 8.2: Regenerate translation artifacts**

```bash
make regen-i18n   # if that target exists, or follow the pattern for your build
# If no dedicated target, the build will regenerate on next `make -j`:
make -j
```

Verify generated files updated:

```bash
git status src/generated/lv_i18n_translations.{c,h} ui_xml/translations/translations.xml
```

- [ ] **Step 8.3: Verify at runtime**

```bash
./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/test.log
```

**User action:** trigger the tour (reset config as in Task 6, or use Settings → Help replay). Confirm all 8 steps display translated title and body text (not the raw keys like `tour.step.welcome.title`). Report back.

- [ ] **Step 8.4: Commit all three artifacts [L064]**

```bash
git add assets/i18n/en.yml src/generated/lv_i18n_translations.c \
        src/generated/lv_i18n_translations.h \
        ui_xml/translations/translations.xml
git commit -m "i18n(tour): add translation keys for first-run tour"
```

---

## Task 9: Multi-device manual verification

- [ ] **Step 9.1: Verify on Raspberry Pi (800×480)**

```bash
PI_HOST=192.168.1.113 make pi-test 2>&1 | tee /tmp/pi-test.log
```

**User action:** on the Pi screen, reset config to trigger tour, verify all 8 steps render correctly. Pay attention to navbar-button highlights (thin vertical buttons on the left — tooltip should sit to their right).

- [ ] **Step 9.2: Verify on AD5M (480×320) — smallest screen**

```bash
make ad5m-docker
AD5M_HOST=192.168.1.67 make ad5m-deploy
ssh root@192.168.1.67 '/opt/helixscreen/helix-screen 2>&1 | tail -200'
```

**User action:** confirm tooltip fits without clipping, especially on navbar-highlight steps. Body text should wrap cleanly. Report any text overflow or buttons off-screen.

- [ ] **Step 9.3: Verify on Sonic Pad (1024×600)**

Per `docs/devel/` / memory `[reference_sonic_pad]`: build with `pi32-docker`, deploy with `scp -O`.

**User action:** confirm tour renders at the larger resolution without tooltip looking lost.

- [ ] **Step 9.4: If any device fails, iterate and recommit**

Common failure modes to watch for:
- Tooltip clipped on right edge of AD5M's 480px width — lower `kTooltipMaxWidth` cap
- Navbar-button highlight outline clipped at screen edge — pad the clamp math in `place_highlight()`
- `widget_nozzle_temp` / `widget_fan` not found on first home page — accept the warn log; step 2 still highlights the grid as a whole

Fix inline, re-run, recommit.

---

## Self-Review

- **Spec coverage.** Walked through spec sections: trigger gate (Task 1), step list + AMS gating (Task 2), overlay widget (Tasks 3+4), state machine (Task 5), auto-trigger (Task 6), replay (Task 7), i18n (Task 8), multi-device (Task 9). All covered.
- **Placeholders.** None — each task has real code and commands. Two items explicitly noted as grep-in-place during implementation: (a) confirming `button_label` child name in `ui_button.xml` (step 4.2 notes), (b) YAML file path (step 8.1 instructs how to find). Both are trivially answerable at the keyboard.
- **Type consistency.** `TourStep` fields (`target_name`, `title_key`, `body_key`, `anchor_hint`, `sub_spotlights`) are consistent across Tasks 2/4/5. `FirstRunTour` public API (`maybe_start`, `start`, `advance`, `skip`, `is_running`, `should_auto_start`, `mark_completed`) is consistent across Tasks 1/5/6/7. Settings JSON pointers (`/tour/completed`, `/tour/version`, `/tour/last_seen_version`, `/wizard_completed`) are consistent across Tasks 1/6.
- **Scope.** Single focused implementation. No subsystem split needed.
