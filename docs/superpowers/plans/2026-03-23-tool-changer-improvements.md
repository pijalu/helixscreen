# Tool Changer Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add multi-tool preheat, tool switcher widget, and multi-nozzle temperature display for tool changer printers (issue #493).

**Architecture:** Three independent features built as panel widgets. Feature 1 extends the existing PreheatWidget. Features 2 and 3 are new panel widgets following the established PanelWidget base class pattern (attach/detach lifecycle, ObserverGuard, static callback dispatch). All use `ToolState::tools()` for ordered tool enumeration and reactive subjects for live updates.

**Tech Stack:** C++17, LVGL 9.5 XML engine, spdlog, nlohmann::json, Catch2 (tests)

**Spec:** `docs/superpowers/specs/2026-03-23-tool-changer-improvements-design.md`

---

## File Map

### Feature 1: Preheat Combo Button
| Action | File | Purpose |
|--------|------|---------|
| Modify | `src/ui/panel_widgets/preheat_widget.cpp` | Add multi-tool preheat logic, tool target cycling |
| Modify | `include/preheat_widget.h` | Add tool target state, new callbacks |
| Modify | `ui_xml/components/panel_widget_preheat.xml` | Add tool target button/label |
| Test | `tests/unit/test_preheat_multi_tool.cpp` | Unit tests for multi-tool preheat |

### Feature 2: Tool Switcher Widget
| Action | File | Purpose |
|--------|------|---------|
| Create | `src/ui/panel_widgets/tool_switcher_widget.h` | Class definition |
| Create | `src/ui/panel_widgets/tool_switcher_widget.cpp` | Widget implementation |
| Create | `ui_xml/components/panel_widget_tool_switcher.xml` | XML layout |
| Modify | `src/ui/panel_widget_registry.cpp` | Widget def + forward decl |
| Modify | `src/xml_registration.cpp` | XML component registration |
| Test | `tests/unit/test_tool_switcher_widget.cpp` | Unit tests |

### Feature 3: Multi-Nozzle Temperature List
| Action | File | Purpose |
|--------|------|---------|
| Create | `src/ui/panel_widgets/nozzle_temps_widget.h` | Class definition |
| Create | `src/ui/panel_widgets/nozzle_temps_widget.cpp` | Widget implementation |
| Create | `ui_xml/components/panel_widget_nozzle_temps.xml` | XML layout (single row template) |
| Modify | `src/ui/panel_widget_registry.cpp` | Widget def + forward decl |
| Modify | `src/xml_registration.cpp` | XML component registration |
| Test | `tests/unit/test_nozzle_temps_widget.cpp` | Unit tests |

---

## Task 1: Preheat Multi-Tool Logic

Extend the existing preheat widget to heat all tools on a multi-tool printer.

**Files:**
- Modify: `include/preheat_widget.h`
- Modify: `src/ui/panel_widgets/preheat_widget.cpp`
- Test: `tests/unit/test_preheat_multi_tool.cpp`

**Docs to read first:**
- `docs/superpowers/specs/2026-03-23-tool-changer-improvements-design.md` (Feature 1 section)
- `include/tool_state.h` — `ToolInfo` struct, `tools()`, `is_multi_tool()`, `effective_heater()`
- `include/preheat_widget.h` — current class structure
- `src/ui/panel_widgets/preheat_widget.cpp` — current `handle_apply()` and `set_temperatures()`

- [ ] **Step 1: Write failing test for multi-tool preheat**

Create `tests/unit/test_preheat_multi_tool.cpp`. Test that when tool target is "all" and there are N tools, `set_temperature()` is called N times (once per tool heater) plus once for bed.

The test should:
- Mock or stub `ToolState` with 3 tools (extruder, extruder1, extruder2)
- Verify the preheat logic calls the API for each tool's `effective_heater()`
- Verify tools with no valid heater (`extruder_name == std::nullopt`) are skipped

Use the Catch2 pattern from `tests/unit/test_panel_widget_thermistor.cpp` as a reference for test structure with `[preheat][panel_widget]` tags.

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[preheat]" -v`
Expected: FAIL — new test function references code that doesn't exist yet.

- [ ] **Step 3: Add tool target state to PreheatWidget header**

In `include/preheat_widget.h`, add:

```cpp
// Tool target for multi-tool preheat
int tool_target_ = -1; // -1 = all tools, 0..N = specific tool index

void cycle_tool_target();
void set_temperatures_multi(MoonrakerAPI* api, int nozzle, int bed);
```

Add a new static callback:
```cpp
static void tool_target_cb(lv_event_t* e);
```

- [ ] **Step 4: Implement multi-tool preheat in preheat_widget.cpp**

In `set_temperatures_multi()`:
- Get `ToolState::instance().tools()`
- If `tool_target_ == -1` (all tools):
  - Iterate tools, call `api->set_temperature(tool.effective_heater(), nozzle, ...)` for each
  - Skip tools where `extruder_name == std::nullopt && heater_name == std::nullopt`
  - Call `api->set_temperature("heater_bed", bed, ...)` once
- If `tool_target_ >= 0` (specific tool):
  - Call `api->set_temperature(tools[tool_target_].effective_heater(), nozzle, ...)`
  - Call `api->set_temperature("heater_bed", bed, ...)`

Modify `handle_apply()`:
- If custom macro is configured, fire macro once (existing behavior, unchanged)
- Otherwise, if `ToolState::instance().is_multi_tool()`, call `set_temperatures_multi()`
- Otherwise, call existing `set_temperatures()` (single-tool path unchanged)

Implement `cycle_tool_target()`:
- Cycle: -1 → 0 → 1 → ... → N-1 → -1
- Update the button label to show "All (N)" or "T0", "T1", etc.

- [ ] **Step 5: Run test to verify it passes**

Run: `make test && ./build/bin/helix-tests "[preheat]" -v`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add tests/unit/test_preheat_multi_tool.cpp include/preheat_widget.h src/ui/panel_widgets/preheat_widget.cpp
git commit -m "feat(preheat): heat all tools on multi-tool printers (prestonbrown/helixscreen#493)"
```

---

## Task 2: Preheat Widget XML — Tool Target Button

Add a tool target selector to the preheat widget XML layout.

**Files:**
- Modify: `ui_xml/components/panel_widget_preheat.xml`
- Modify: `src/ui/panel_widgets/preheat_widget.cpp` (attach the new button)

**Docs to read first:**
- `ui_xml/components/panel_widget_preheat.xml` — current layout structure
- `docs/devel/LVGL9_XML_GUIDE.md` — XML widget syntax, event_cb binding

- [ ] **Step 1: Add tool target button to XML**

In `panel_widget_preheat.xml`, add a small button next to the split button for tool targeting. Use a `ui_button` with `variant="ghost"` and name `"preheat_tool_target"`:

```xml
<ui_button name="preheat_tool_target" variant="ghost"
           width="content" height="#button_height"
           style_pad_left="#space_sm" style_pad_right="#space_sm">
    <text_small name="tool_target_label" text="All"/>
    <event_cb trigger="clicked" callback="preheat_tool_target_cb"/>
</ui_button>
```

Place it in the existing row container, between the split button and the temp display. The button should only be visible on multi-tool printers — this will be handled in C++ attach by hiding it when `!is_multi_tool()`.

- [ ] **Step 2: Wire up tool target button in C++ attach**

In `preheat_widget.cpp` `attach()`:
- Find `tool_target_btn_` via `lv_obj_find_by_name(widget_obj_, "preheat_tool_target")`
- Register the callback: `lv_xml_register_event_cb(nullptr, "preheat_tool_target_cb", PreheatWidget::tool_target_cb)`  (in `register_preheat_widget()`)
- If `!ToolState::instance().is_multi_tool()`, hide the button with `lv_obj_add_flag(tool_target_btn_, LV_OBJ_FLAG_HIDDEN)`
- Update label text based on `tool_target_` state

- [ ] **Step 3: Build and test visually**

Run: `make -j && ./build/bin/helix-screen --test -vv`
The tool target button should appear on multi-tool mock printers. Verify single-extruder mock doesn't show it.

- [ ] **Step 4: Commit**

```bash
git add ui_xml/components/panel_widget_preheat.xml src/ui/panel_widgets/preheat_widget.cpp
git commit -m "feat(preheat): add tool target selector button (prestonbrown/helixscreen#493)"
```

---

## Task 3: Tool Switcher Widget — Registration & Skeleton

Create the new widget skeleton with registry entry and XML component.

**Files:**
- Create: `src/ui/panel_widgets/tool_switcher_widget.h`
- Create: `src/ui/panel_widgets/tool_switcher_widget.cpp`
- Create: `ui_xml/components/panel_widget_tool_switcher.xml`
- Modify: `src/ui/panel_widget_registry.cpp`
- Modify: `src/xml_registration.cpp`

**Docs to read first:**
- `include/panel_widget.h` — PanelWidget base class, virtual methods
- `src/ui/panel_widgets/temp_stack_widget.h` — example widget header pattern
- `src/ui/panel_widgets/temp_stack_widget.cpp` lines 27-42 — registration pattern
- `src/ui/panel_widget_registry.cpp` lines 46-87 — widget def table format
- `src/xml_registration.cpp` lines 327-349 — XML registration pattern

- [ ] **Step 1: Create tool_switcher_widget.h**

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "panel_widget.h"
#include "ui_observer_guard.h"
#include <memory>
#include <vector>

class PrinterState;

namespace helix {

class ToolSwitcherWidget : public PanelWidget {
  public:
    explicit ToolSwitcherWidget(PrinterState& printer_state);
    ~ToolSwitcherWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override { return "tool_switcher"; }
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;

  private:
    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* picker_backdrop_ = nullptr;

    int current_colspan_ = 1;
    int current_rowspan_ = 1;

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(false);

    ObserverGuard active_tool_observer_;
    ObserverGuard tool_count_observer_;

    // Pill button references (for inline mode)
    std::vector<lv_obj_t*> pill_buttons_;

    void rebuild_pills();
    void rebuild_compact();
    void show_tool_picker();
    void dismiss_tool_picker();
    void handle_tool_selected(int tool_index);
    void on_active_tool_changed(int tool_index);

  public:
    static void tool_pill_cb(lv_event_t* e);
    static void tool_compact_cb(lv_event_t* e);
};

void register_tool_switcher_widget();

} // namespace helix
```

- [ ] **Step 2: Create minimal tool_switcher_widget.cpp**

Create with: registration function, constructor/destructor, empty attach/detach, `on_size_changed` stub. Follow `temp_stack_widget.cpp` registration pattern.

Key registration details:
- Factory: `register_widget_factory("tool_switcher", [](const std::string&) { ... })`
- Widget def in `panel_widget_registry.cpp`: `{"tool_switcher", "Tool Switcher", "swap_horizontal", "Quick tool switching for multi-tool printers", "Tool Switcher", "show_tool_badge", "Requires multi-tool printer", false, 1, 1, 1, 1, 2, 2}`
- Note `hardware_gate_subject = "tool_count"` so it only appears for multi-tool printers
- Note `default_enabled = false` — opt-in widget
- XML callbacks: register `tool_pill_cb` and `tool_compact_cb` in `register_tool_switcher_widget()`

- [ ] **Step 3: Create panel_widget_tool_switcher.xml**

Minimal XML with a named container for dynamic content:

```xml
<component name="panel_widget_tool_switcher">
    <view extends="lv_obj" name="tool_switcher_root"
          width="100%" height="100%" flex_flow="column"
          style_flex_main_place="center" style_flex_cross_place="center"
          style_pad_all="#space_xs" scrollable="false"
          style_bg_opa="0" style_border_width="0">
        <lv_obj name="tool_switcher_container"
                width="100%" height="100%" flex_flow="row"
                style_flex_main_place="center" style_flex_cross_place="center"
                style_pad_all="0" style_gap_column="#space_xs"
                scrollable="false" style_bg_opa="0" style_border_width="0"/>
    </view>
</component>
```

- [ ] **Step 4: Add to registry and XML registration**

In `src/ui/panel_widget_registry.cpp`:
- Add forward declaration: `void register_tool_switcher_widget();`
- Add widget def entry in `s_widget_defs` (after `fan` entry, tools section)
- Add `register_tool_switcher_widget();` call in `init_widget_registrations()`

In `src/xml_registration.cpp`:
- Add `register_xml("components/panel_widget_tool_switcher.xml");` in the panel widget section

- [ ] **Step 5: Build to verify compilation**

Run: `make -j`
Expected: Compiles without errors. Widget appears in the widget picker for multi-tool printers.

- [ ] **Step 6: Commit**

```bash
git add src/ui/panel_widgets/tool_switcher_widget.h src/ui/panel_widgets/tool_switcher_widget.cpp ui_xml/components/panel_widget_tool_switcher.xml src/ui/panel_widget_registry.cpp src/xml_registration.cpp
git commit -m "feat(tool-switcher): add widget skeleton and registry entry (prestonbrown/helixscreen#493)"
```

---

## Task 4: Tool Switcher Widget — Pill Buttons (Inline Mode)

Implement the pill button layout for 1x2/2x1/2x2 sizes.

**Files:**
- Modify: `src/ui/panel_widgets/tool_switcher_widget.cpp`
- Modify: `src/ui/panel_widgets/tool_switcher_widget.h` (if needed)
- Test: `tests/unit/test_tool_switcher_widget.cpp`

**Docs to read first:**
- `include/tool_state.h` — `tools()`, `get_active_tool_subject()`, `request_tool_change()`
- `src/ui/panel_widgets/fan_stack_widget.cpp` — dynamic button creation pattern
- `docs/devel/UI_CONTRIBUTOR_GUIDE.md` — design tokens, button variants

- [ ] **Step 1: Write failing test for tool switching**

Create `tests/unit/test_tool_switcher_widget.cpp` with `[tool_switcher][panel_widget]` tags:
- Test widget def exists in registry with correct metadata
- Test that `hardware_gate_subject` is `"show_tool_badge"` (0 for single-tool, 1 for multi-tool)

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[tool_switcher]" -v`
Expected: FAIL (or pass if skeleton is already registered — adjust test to check a behavior)

- [ ] **Step 3: Implement rebuild_pills()**

In `tool_switcher_widget.cpp`:
- Get `ToolState::instance().tools()`
- Find container via `lv_obj_find_by_name(widget_obj_, "tool_switcher_container")`
- Clear container: `lv_obj_clean(container)`
- For each tool, create a `ui_button`:
  - Use `lv_xml_create(container, "ui_button", ...)` or create manually with `lv_button_create(container)`
  - Set text to tool name ("T0", "T1", ...)
  - Set variant: active tool = primary, others = ghost/secondary
  - Store tool index in button's user_data (use small int cast, not heap allocation)
  - Add click event via `lv_obj_add_event_cb()` — OK here since these are dynamic C++ buttons, not XML components
  - Set `flex_grow="1"` so pills divide space equally
- Store button pointers in `pill_buttons_` vector
- Set flex_flow on container based on size: row for 2x1, column for 1x2, wrap for 2x2

- [ ] **Step 4: Implement on_active_tool_changed()**

Observer callback: when active tool subject changes, update pill highlighting:
- Iterate `pill_buttons_`, set active pill to primary variant, others to secondary
- Use `lv_obj_set_style_bg_color()` with theme colors

- [ ] **Step 5: Wire up observers in attach()**

```cpp
void ToolSwitcherWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    *alive_ = true;
    s_active_instance = this;

    auto& tool_state = ToolState::instance();
    std::weak_ptr<bool> weak_alive = alive_;

    active_tool_observer_ = observe_int_sync<ToolSwitcherWidget>(
        tool_state.get_active_tool_subject(), this,
        [weak_alive](ToolSwitcherWidget* self, int tool) {
            if (weak_alive.expired()) return;
            self->on_active_tool_changed(tool);
        });

    rebuild_pills(); // initial build
}
```

- [ ] **Step 6: Implement handle_tool_selected() with safety gate**

```cpp
void ToolSwitcherWidget::handle_tool_selected(int tool_index) {
    auto& tool_state = ToolState::instance();
    auto* api = printer_state_.api();
    if (!api) return;

    // Safety: warn during active print
    auto print_state = lv_subject_get_int(printer_state_.get_print_state_subject());
    if (print_state == static_cast<int>(PrintState::PRINTING)) {
        // Pack tool_index into user_data (small int, safe to cast)
        helix::ui::modal_show_confirmation(
            "Tool Change", "Changing tools during a print may cause issues. Continue?",
            ModalSeverity::WARNING, "Change Tool",
            [](lv_event_t* e) {
                auto idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
                auto* api_ptr = PrinterState::instance().api();
                if (api_ptr) ToolState::instance().request_tool_change(static_cast<int>(idx), api_ptr);
            },
            nullptr, reinterpret_cast<void*>(static_cast<intptr_t>(tool_index)));
        return;
    }

    tool_state.request_tool_change(tool_index, api);
}
```

- [ ] **Step 7: Implement on_size_changed()**

```cpp
void ToolSwitcherWidget::on_size_changed(int colspan, int rowspan, int, int) {
    current_colspan_ = colspan;
    current_rowspan_ = rowspan;
    if (colspan == 1 && rowspan == 1) {
        rebuild_compact();
    } else {
        rebuild_pills();
    }
}
```

- [ ] **Step 8: Implement detach()**

```cpp
void ToolSwitcherWidget::detach() {
    *alive_ = false;
    dismiss_tool_picker();
    active_tool_observer_.reset();
    tool_count_observer_.reset();
    pill_buttons_.clear();

    if (s_active_instance == this) s_active_instance = nullptr;
    if (widget_obj_) lv_obj_set_user_data(widget_obj_, nullptr);
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}
```

- [ ] **Step 9: Build and test visually**

Run: `make -j && ./build/bin/helix-screen --test -vv`
Add the tool switcher widget to the home panel. Verify pills appear with correct tool labels and active highlighting. Test tool switching.

- [ ] **Step 10: Commit**

```bash
git add src/ui/panel_widgets/tool_switcher_widget.cpp src/ui/panel_widgets/tool_switcher_widget.h tests/unit/test_tool_switcher_widget.cpp
git commit -m "feat(tool-switcher): implement pill button layout and tool switching (prestonbrown/helixscreen#493)"
```

---

## Task 5: Tool Switcher Widget — Compact Mode (1x1 Picker)

Implement the 1x1 context menu/picker for compact layout.

**Files:**
- Modify: `src/ui/panel_widgets/tool_switcher_widget.cpp`

**Docs to read first:**
- `src/ui/panel_widgets/fan_stack_widget.cpp` lines 707-987 — picker backdrop pattern (`show_fan_picker`, `dismiss_fan_picker`)
- The fan picker creates a full-screen backdrop, positions a card, populates it with buttons

- [ ] **Step 1: Implement rebuild_compact()**

Show current tool label centered in widget. Tap opens picker:
- Find container, clean it
- Create a centered label showing active tool name ("T2")
- Make the whole widget clickable: register `tool_compact_cb` in XML, or add event in C++

- [ ] **Step 2: Implement show_tool_picker()**

Follow `FanStackWidget::show_fan_picker()` pattern:
- Create `picker_backdrop_` on `parent_screen_` (full screen, semi-transparent)
- Create a card container centered on screen
- Populate with tool buttons in a grid (3 columns for 6 tools)
- Each button shows tool name, active tool highlighted
- Click backdrop → dismiss
- Click tool → `handle_tool_selected(index)` + dismiss

- [ ] **Step 3: Implement dismiss_tool_picker()**

```cpp
void ToolSwitcherWidget::dismiss_tool_picker() {
    if (!picker_backdrop_) return;
    lv_obj_t* backdrop = picker_backdrop_;
    picker_backdrop_ = nullptr;
    helix::ui::safe_delete(backdrop);
}
```

- [ ] **Step 4: Build and test visually**

Run: `make -j && ./build/bin/helix-screen --test -vv`
Add tool switcher at 1x1 size. Verify tap opens picker, selection works, backdrop dismisses.

- [ ] **Step 5: Commit**

```bash
git add src/ui/panel_widgets/tool_switcher_widget.cpp
git commit -m "feat(tool-switcher): add 1x1 compact mode with picker popup (prestonbrown/helixscreen#493)"
```

---

## Task 6: Nozzle Temps Widget — Registration & Skeleton

Create the new widget skeleton with registry entry and XML component.

**Files:**
- Create: `src/ui/panel_widgets/nozzle_temps_widget.h`
- Create: `src/ui/panel_widgets/nozzle_temps_widget.cpp`
- Create: `ui_xml/components/panel_widget_nozzle_temps.xml`
- Modify: `src/ui/panel_widget_registry.cpp`
- Modify: `src/xml_registration.cpp`

**Docs to read first:**
- `include/panel_widget.h` — base class
- `src/ui/panel_widgets/temp_stack_widget.h` — observer pattern reference
- `include/printer_temperature_state.h` — `ExtruderInfo`, per-extruder subjects, `get_extruder_version_subject()`
- `include/tool_state.h` — `tools()` for ordered iteration

- [ ] **Step 1: Create nozzle_temps_widget.h**

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "panel_widget.h"
#include "ui_observer_guard.h"
// SubjectLifetime is in ui_observer_guard.h — no separate include needed
#include <memory>
#include <string>
#include <vector>

class PrinterState;

namespace helix {

class NozzleTempsWidget : public PanelWidget {
  public:
    explicit NozzleTempsWidget(PrinterState& printer_state);
    ~NozzleTempsWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override { return "nozzle_temps"; }

  private:
    PrinterState& printer_state_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(false);

    // Per-extruder row tracking
    struct ExtruderRow {
        std::string name;
        lv_obj_t* row_obj = nullptr;
        lv_obj_t* temp_label = nullptr;
        lv_obj_t* target_label = nullptr;
        lv_obj_t* progress_bar = nullptr;
        ObserverGuard temp_observer;
        ObserverGuard target_observer;
        SubjectLifetime temp_lifetime;
        SubjectLifetime target_lifetime;
        int cached_temp = 0;   // centidegrees
        int cached_target = 0; // centidegrees
    };

    std::vector<ExtruderRow> extruder_rows_;

    // Bed row (static subjects, no lifetime needed)
    lv_obj_t* bed_row_ = nullptr;
    lv_obj_t* bed_temp_label_ = nullptr;
    lv_obj_t* bed_target_label_ = nullptr;
    lv_obj_t* bed_progress_bar_ = nullptr;
    ObserverGuard bed_temp_observer_;
    ObserverGuard bed_target_observer_;
    int cached_bed_temp_ = 0;
    int cached_bed_target_ = 0;

    // Version observer to trigger rebuilds
    ObserverGuard version_observer_;

    void rebuild_rows();
    void clear_rows();
    void create_extruder_row(lv_obj_t* container, ExtruderRow& row);
    void create_bed_row(lv_obj_t* container);
    void update_row_display(lv_obj_t* temp_label, lv_obj_t* target_label,
                            lv_obj_t* progress_bar, int temp_centi, int target_centi,
                            bool is_bed);
};

void register_nozzle_temps_widget();

} // namespace helix
```

- [ ] **Step 2: Create minimal nozzle_temps_widget.cpp**

Registration function + constructor/destructor + empty attach/detach. Follow `temp_stack_widget.cpp` registration pattern.

Registration details:
- Factory: `register_widget_factory("nozzle_temps", [](const std::string&) { ... })`
- Widget def: `{"nozzle_temps", "Nozzle Temperatures", "thermometer", "All extruder temperatures with progress bars", "Nozzle Temperatures", "show_tool_badge", "Requires multi-tool printer", false, 1, 2, 1, 1, 2, 3}`
- Note: `min_rowspan=1`, default `rowspan=2` (1x2 preferred), `max_rowspan=3`
- `hardware_gate_subject = "tool_count"` — only shows for multi-tool
- `default_enabled = false` — opt-in

- [ ] **Step 3: Create panel_widget_nozzle_temps.xml**

```xml
<component name="panel_widget_nozzle_temps">
    <view extends="lv_obj" name="nozzle_temps_root"
          width="100%" height="100%" flex_flow="column"
          style_flex_main_place="start" style_flex_cross_place="center"
          style_pad_all="#space_xs" style_gap_row="#space_xxs"
          scrollable="true" style_bg_opa="0" style_border_width="0">
        <!-- Label -->
        <text_small name="nozzle_temps_title" text="Nozzle Temps"
                    width="100%" style_text_align="left"
                    style_pad_left="#space_xxs" style_pad_bottom="#space_xxs"/>
        <!-- Dynamic rows created in C++ -->
        <lv_obj name="nozzle_temps_container"
                width="100%" flex_grow="1" flex_flow="column"
                style_pad_all="0" style_gap_row="#space_xxs"
                scrollable="false" style_bg_opa="0" style_border_width="0"/>
    </view>
</component>
```

- [ ] **Step 4: Add to registry and XML registration**

Same pattern as Task 3 Step 4:
- Forward decl + widget def in `panel_widget_registry.cpp`
- `register_nozzle_temps_widget();` in `init_widget_registrations()`
- `register_xml(...)` in `xml_registration.cpp`

- [ ] **Step 5: Build to verify compilation**

Run: `make -j`
Expected: Compiles. Widget shows in picker for multi-tool printers.

- [ ] **Step 6: Commit**

```bash
git add src/ui/panel_widgets/nozzle_temps_widget.h src/ui/panel_widgets/nozzle_temps_widget.cpp ui_xml/components/panel_widget_nozzle_temps.xml src/ui/panel_widget_registry.cpp src/xml_registration.cpp
git commit -m "feat(nozzle-temps): add widget skeleton and registry entry (prestonbrown/helixscreen#493)"
```

---

## Task 7: Nozzle Temps Widget — Dynamic Rows & Observers

Implement the temperature rows with per-extruder observers and progress bars.

**Files:**
- Modify: `src/ui/panel_widgets/nozzle_temps_widget.cpp`
- Test: `tests/unit/test_nozzle_temps_widget.cpp`

**Docs to read first:**
- `include/printer_temperature_state.h` — `get_extruder_temp_subject(name, lifetime)`, `get_extruder_version_subject()`
- `include/tool_state.h` — `tools()` for ordered iteration
- `include/ui_observer_guard.h` — `SubjectLifetime` usage pattern
- `src/ui/panel_widgets/fan_stack_widget.cpp` — dynamic row creation + version observer pattern
- `docs/devel/UI_CONTRIBUTOR_GUIDE.md` — design tokens for colors

- [ ] **Step 1: Write failing test**

Create `tests/unit/test_nozzle_temps_widget.cpp`:
- Test widget def exists with correct metadata
- Test `hardware_gate_subject` is `"show_tool_badge"` (0 for single-tool, 1 for multi-tool)

- [ ] **Step 2: Run test**

Run: `make test && ./build/bin/helix-tests "[nozzle_temps]" -v`

- [ ] **Step 3: Implement rebuild_rows()**

Core logic:
- Call `clear_rows()` first (reset all observers, clean container)
- Get `ToolState::instance().tools()` for ordered tool list
- Find container via `lv_obj_find_by_name(widget_obj_, "nozzle_temps_container")`
- For each tool with a valid extruder:
  - Create an `ExtruderRow` entry
  - Call `create_extruder_row(container, row)`
  - Look up subjects with lifetime: `temp_state.get_extruder_temp_subject(name, row.temp_lifetime)`
  - Create observers with `observe_int_sync` using `weak_alive` guard
  - Observer callbacks update `cached_temp`/`cached_target` and call `update_row_display()`
- Call `create_bed_row(container)` at the end
- Set up bed observers on static subjects (no lifetime needed)

- [ ] **Step 4: Implement create_extruder_row()**

Each row is a small container created programmatically:
- Container: horizontal flex, full width, height based on design tokens
- Tool label: "T0" in primary accent color, bold, fixed width
- Temp label: current temp in large readable font
- Target label: "→ 210°" or "off" in muted color
- Progress bar: `lv_bar_create()`, 3px height, full width, color-coded
  - Green (`#00b894`): `abs(temp - target) <= 20` (2°C in centidegrees)
  - Yellow (`#fdcb6e`): heating (temp < target)
  - Gray: off (target == 0)

Use `ui_theme_get_color()` for token-based colors where available.

- [ ] **Step 5: Implement create_bed_row()**

Same as extruder row but:
- Label: "Bed" or bed icon instead of "T0"
- Different accent color for the progress bar (use bed/orange accent)
- Add a thin divider above it (`divider_horizontal` or manual 1px line)

- [ ] **Step 6: Implement update_row_display()**

```cpp
void NozzleTempsWidget::update_row_display(
    lv_obj_t* temp_label, lv_obj_t* target_label,
    lv_obj_t* progress_bar, int temp_centi, int target_centi, bool is_bed) {

    // Format temp: centidegrees to degrees
    float temp = temp_centi / 10.0f;
    float target = target_centi / 10.0f;

    lv_label_set_text_fmt(temp_label, "%.0f°", temp);

    if (target_centi > 0) {
        lv_label_set_text_fmt(target_label, "→ %.0f°", target);
        // Progress: percentage toward target
        int progress = (target_centi > 0) ? (temp_centi * 100 / target_centi) : 0;
        progress = std::clamp(progress, 0, 100);
        lv_bar_set_value(progress_bar, progress, LV_ANIM_ON);

        // Color: green if at temp, yellow if heating
        bool at_temp = std::abs(temp_centi - target_centi) <= 20; // 2°C
        lv_color_t bar_color;
        if (at_temp) {
            bar_color = ui_theme_get_color("success"); // green for all heaters at temp
        } else if (is_bed) {
            bar_color = ui_theme_get_color("danger");  // orange/red accent for bed heating
        } else {
            bar_color = ui_theme_get_color("warning"); // yellow for nozzles heating
        }
        lv_obj_set_style_bg_color(progress_bar, bar_color, LV_PART_INDICATOR);
    } else {
        lv_label_set_text(target_label, "off");
        lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(progress_bar,
            ui_theme_get_color("muted"), LV_PART_INDICATOR);
    }
}
```

- [ ] **Step 7: Wire version observer in attach()**

```cpp
// Rebuild rows when extruder list changes (reconnection/rediscovery)
version_observer_ = observe_int_sync<NozzleTempsWidget>(
    printer_state_.temperature_state().get_extruder_version_subject(), this,
    [weak_alive](NozzleTempsWidget* self, int) {
        if (weak_alive.expired()) return;
        self->rebuild_rows();
    });
```

- [ ] **Step 8: Implement clear_rows() with ScopedFreeze**

```cpp
void NozzleTempsWidget::clear_rows() {
    // Freeze queue to prevent race between drain and widget destruction
    auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
    helix::ui::UpdateQueue::instance().drain();

    // Reset all observers (drops SubjectLifetime tokens too)
    // Version observer reset here under freeze to prevent race
    version_observer_.reset();
    extruder_rows_.clear();
    bed_temp_observer_.reset();
    bed_target_observer_.reset();

    // Clean the container
    auto* container = lv_obj_find_by_name(widget_obj_, "nozzle_temps_container");
    if (container) lv_obj_clean(container);

    bed_row_ = nullptr;
    bed_temp_label_ = nullptr;
    bed_target_label_ = nullptr;
    bed_progress_bar_ = nullptr;
}
```

- [ ] **Step 9: Implement detach()**

```cpp
void NozzleTempsWidget::detach() {
    *alive_ = false;
    // NOTE: version_observer_ is reset inside clear_rows() under ScopedFreeze
    // to prevent races where a queued version callback fires during teardown
    clear_rows();
    version_observer_.reset();

    if (s_active_instance == this) s_active_instance = nullptr;
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
}
```

- [ ] **Step 10: Build and test visually**

Run: `make -j && ./build/bin/helix-screen --test -vv`
Add nozzle temps widget to home panel. Verify:
- All extruder rows show with correct labels
- Temps update in real-time
- Progress bars animate and color-code correctly
- Bed row appears at bottom with divider

- [ ] **Step 11: Run tests**

Run: `make test && ./build/bin/helix-tests "[nozzle_temps]" -v`

- [ ] **Step 12: Commit**

```bash
git add src/ui/panel_widgets/nozzle_temps_widget.cpp tests/unit/test_nozzle_temps_widget.cpp
git commit -m "feat(nozzle-temps): implement dynamic temperature rows with progress bars (prestonbrown/helixscreen#493)"
```

---

## Task 8: Final Integration & Visual Polish

Wire everything together, test the full flow, and polish.

**Files:**
- Potentially adjust any of the above files for visual polish

- [ ] **Step 1: Build full project**

Run: `make -j`
Expected: Clean compilation, no warnings from new code.

- [ ] **Step 2: Run all tests**

Run: `make test-run`
Expected: All tests pass, including new ones.

- [ ] **Step 3: Visual integration test**

Run: `make -j && ./build/bin/helix-screen --test -vv`

Test checklist:
- [ ] Preheat widget: "All" default heats all nozzles + bed
- [ ] Preheat widget: Cycle to specific tool, verify only that tool heats
- [ ] Preheat widget: Single-extruder mock — no tool target button visible
- [ ] Tool switcher (1x1): Shows current tool, tap opens picker, selection works
- [ ] Tool switcher (2x1): Pill buttons, active highlighted, tap switches
- [ ] Tool switcher: Not visible on single-extruder mock
- [ ] Nozzle temps (1x2): All rows visible, temps updating, progress bars correct
- [ ] Nozzle temps: Bed row at bottom with divider
- [ ] Nozzle temps: Not visible on single-extruder mock
- [ ] No crashes on panel switch or reconnect

- [ ] **Step 4: Commit final integration**

Only if there are remaining unstaged fixes from visual polish:
```bash
git add <specific changed files>
git commit -m "fix(tool-changer): visual polish from integration testing (prestonbrown/helixscreen#493)"
```
