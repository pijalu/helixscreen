# Multi-Instance Widget System + Power Device Widget

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace hardcoded favorite_macro_1..5 pattern with dynamic multi-instance widgets, then build a power device widget with WebSocket-driven live state.

**Architecture:** A `multi_instance` flag on `PanelWidgetDef` enables dynamic instance creation with `base_id:N` IDs. The catalog mints new IDs on add, grid_edit_mode deletes entries on remove. A new `PowerDeviceState` singleton subscribes to Moonraker's `notify_power_changed` WebSocket events and exposes per-device LVGL subjects.

**Tech Stack:** C++17, LVGL 9.5, nlohmann/json, spdlog, Moonraker WebSocket API

**Spec:** `docs/superpowers/specs/2026-03-22-multi-instance-widgets-design.md`

---

## File Map

### New Files
| File | Responsibility |
|------|---------------|
| `include/power_device_state.h` | PowerDeviceState singleton header |
| `src/printer/power_device_state.cpp` | Per-device subjects, notify_power_changed handler, lock state |
| `include/power_device_widget.h` | PowerDeviceWidget class header |
| `src/ui/widgets/power_device_widget.cpp` | Widget impl, device picker, click handler |
| `ui_xml/components/panel_widget_power_device.xml` | Widget XML layout (circular badge) |
| `tests/unit/test_multi_instance_widget.cpp` | Tests for multi-instance infra + config migration |
| `tests/unit/test_power_device_state.cpp` | Tests for PowerDeviceState |

### Modified Files
| File | What Changes |
|------|-------------|
| `include/panel_widget_registry.h:16,19-55` | `WidgetFactory` signature, add `multi_instance`, remove `catalog_group` |
| `src/ui/panel_widget_registry.cpp:90-94,100-108,120-155` | `find_widget_def()` colon lookup, remove kFavMacroIds/loop, add power_device def |
| `include/panel_widget_config.h:37-83` | Add `mint_instance_id()`, `delete_entry()` |
| `src/system/panel_widget_config.cpp:23-131,212-299` | Migration in `load()`, skip multi_instance in `build_default_grid()`, mint/delete impl |
| `src/ui/ui_widget_catalog_overlay.cpp:179-304` | Replace catalog_group with multi_instance logic |
| `src/ui/panel_widget_manager.cpp:140` | Pass instance ID to factory |
| `include/favorite_macro_widget.h:21,24-25,30-31,43-45,56` | Remove kMaxFavoriteMacroSlots, update comments/constructor |
| `src/ui/widgets/favorite_macro_widget.cpp:28-42` | Single factory registration |
| `src/ui/grid_edit_mode.cpp:648-649` | Delete (not disable) multi-instance entries |
| `src/application/application.cpp:1919-1928` | Wire PowerDeviceState subscribe/unsubscribe |
| `src/api/moonraker_discovery_sequence.cpp:285` | Call PowerDeviceState::set_devices() |
| `src/api/moonraker_client_mock.cpp:603-611` | Mock notify_power_changed, call set_devices() |
| `tests/unit/test_panel_widget_config.cpp` | Update tests for new find_widget_def behavior |

---

## Task 1: Multi-Instance Infrastructure — PanelWidgetDef & Registry

**Files:**
- Modify: `include/panel_widget_registry.h:16,19-55`
- Modify: `src/ui/panel_widget_registry.cpp:90-94,100-108`
- Test: `tests/unit/test_multi_instance_widget.cpp` (create)

- [ ] **Step 1: Write failing tests for find_widget_def with colon IDs**

Create `tests/unit/test_multi_instance_widget.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_config.h"
#include "panel_widget_registry.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("find_widget_def resolves multi-instance IDs",
          "[panel_widget][multi_instance]") {
    // Exact match still works for single-instance
    REQUIRE(find_widget_def("power") != nullptr);
    REQUIRE(std::string(find_widget_def("power")->id) == "power");

    // Multi-instance base ID resolves
    const auto* fav = find_widget_def("favorite_macro");
    REQUIRE(fav != nullptr);
    REQUIRE(fav->multi_instance == true);

    // Colon-suffixed ID resolves to base def
    const auto* fav_inst = find_widget_def("favorite_macro:1");
    REQUIRE(fav_inst != nullptr);
    REQUIRE(std::string(fav_inst->id) == "favorite_macro");
    REQUIRE(fav_inst->multi_instance == true);

    // Higher numbers work
    const auto* fav_inst2 = find_widget_def("favorite_macro:42");
    REQUIRE(fav_inst2 != nullptr);
    REQUIRE(std::string(fav_inst2->id) == "favorite_macro");

    // Non-existent base returns nullptr
    REQUIRE(find_widget_def("nonexistent:1") == nullptr);

    // Colon on non-multi_instance def returns nullptr
    REQUIRE(find_widget_def("power:1") == nullptr);
}

TEST_CASE("WidgetFactory receives instance ID",
          "[panel_widget][multi_instance]") {
    const auto* def = find_widget_def("favorite_macro");
    REQUIRE(def != nullptr);
    REQUIRE(def->factory != nullptr);

    auto widget = def->factory("favorite_macro:7");
    REQUIRE(widget != nullptr);
    REQUIRE(std::string(widget->id()) == "favorite_macro:7");
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[multi_instance]" -v`
Expected: Compilation errors (multi_instance field doesn't exist yet, factory signature wrong)

- [ ] **Step 3: Update PanelWidgetDef and WidgetFactory**

In `include/panel_widget_registry.h`:

Change line 16:
```cpp
// BEFORE
using WidgetFactory = std::function<std::unique_ptr<PanelWidget>()>;
// AFTER
using WidgetFactory = std::function<std::unique_ptr<PanelWidget>(const std::string& instance_id)>;
```

In `PanelWidgetDef` (line 34), replace `catalog_group` with:
```cpp
    bool multi_instance = false;       // Allows dynamic instance creation with base_id:N IDs
```

- [ ] **Step 4: Update find_widget_def() with colon-aware lookup**

In `src/ui/panel_widget_registry.cpp`, replace `find_widget_def()` (lines 90-94):

```cpp
const PanelWidgetDef* find_widget_def(std::string_view id) {
    auto it = std::find_if(s_widget_defs.begin(), s_widget_defs.end(),
                           [&id](const PanelWidgetDef& def) { return id == def.id; });
    if (it != s_widget_defs.end())
        return &*it;

    // Multi-instance: strip ":N" suffix and retry
    auto colon = id.rfind(':');
    if (colon != std::string_view::npos) {
        auto base = id.substr(0, colon);
        it = std::find_if(s_widget_defs.begin(), s_widget_defs.end(),
                           [&base](const PanelWidgetDef& def) {
                               return base == def.id && def.multi_instance;
                           });
        if (it != s_widget_defs.end())
            return &*it;
    }
    return nullptr;
}
```

- [ ] **Step 5: Fix all existing factory call sites**

In `src/ui/panel_widget_manager.cpp` line 140, change:
```cpp
// BEFORE
slot.instance = def->factory();
// AFTER
slot.instance = def->factory(entry.id);
```

Update ALL `register_widget_factory` call sites across widget .cpp files. Each existing single-instance factory lambda gets a `const std::string&` parameter it ignores:

```cpp
// Pattern for single-instance widgets (each widget's register function):
register_widget_factory("power", [](const std::string&) {
    return std::make_unique<PowerWidget>();
});
```

Find all with: `grep -rn "register_widget_factory" src/ui/`

- [ ] **Step 6: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[multi_instance]" -v`
Expected: PASS (but favorite_macro tests may still fail — that's Task 2)

- [ ] **Step 7: Commit**

```bash
git add include/panel_widget_registry.h src/ui/panel_widget_registry.cpp \
  src/ui/panel_widget_manager.cpp src/ui/widgets/*.cpp src/ui/panel_widgets/*.cpp \
  tests/unit/test_multi_instance_widget.cpp
git commit -m "feat(widgets): add multi_instance flag and colon-aware find_widget_def (#342)"
```

---

## Task 2: Favorite Macro Migration

**Files:**
- Modify: `include/favorite_macro_widget.h:21,24-25,30-31,56`
- Modify: `src/ui/widgets/favorite_macro_widget.cpp:28-42`
- Modify: `src/ui/panel_widget_registry.cpp:120-155`
- Modify: `tests/unit/test_multi_instance_widget.cpp`

- [ ] **Step 1: Write failing test for config migration**

Add to `tests/unit/test_multi_instance_widget.cpp`:

```cpp
namespace helix {
class PanelWidgetConfigFixture {
  protected:
    Config config;
    void setup_with_widgets(const nlohmann::json& widgets_json,
                            const std::string& panel_id = "home") {
        config.data = nlohmann::json::object();
        config.data["printers"]["default"]["panel_widgets"][panel_id] = widgets_json;
    }
};
} // namespace helix

TEST_CASE_METHOD(helix::PanelWidgetConfigFixture,
                 "Config migration renames favorite_macro_N to favorite_macro:N",
                 "[panel_widget][multi_instance][migration]") {
    nlohmann::json widgets = nlohmann::json::array({
        {{"id", "favorite_macro_1"}, {"enabled", true},
         {"config", {{"macro", "CLEAN_NOZZLE"}}}, {"col", 2}, {"row", 0},
         {"colspan", 1}, {"rowspan", 1}},
        {{"id", "favorite_macro_3"}, {"enabled", true},
         {"config", {{"macro", "PARK"}}}, {"col", 3}, {"row", 0},
         {"colspan", 1}, {"rowspan", 1}},
        {{"id", "power"}, {"enabled", true}, {"col", 4}, {"row", 0},
         {"colspan", 1}, {"rowspan", 1}},
    });
    setup_with_widgets(widgets);

    PanelWidgetConfig pwc("home", config);
    pwc.load();

    const auto& entries = pwc.entries();
    // Should find migrated IDs
    bool found_1 = false, found_3 = false;
    for (const auto& e : entries) {
        if (e.id == "favorite_macro:1") {
            found_1 = true;
            REQUIRE(e.enabled == true);
            REQUIRE(e.config.value("macro", "") == "CLEAN_NOZZLE");
        }
        if (e.id == "favorite_macro:3") {
            found_3 = true;
        }
        // Old IDs should NOT exist
        REQUIRE(e.id != "favorite_macro_1");
        REQUIRE(e.id != "favorite_macro_3");
    }
    REQUIRE(found_1);
    REQUIRE(found_3);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[migration]" -v`
Expected: FAIL (migration logic doesn't exist yet)

- [ ] **Step 3: Update favorite_macro_widget.h**

Remove `kMaxFavoriteMacroSlots` (line 21). Update comments (lines 24-25) and constructor doc (line 30):

```cpp
/// Home panel widget for one-tap macro execution.
/// Registered as multi_instance "favorite_macro" — instances get IDs like "favorite_macro:1".
/// All share a single XML component. Tap executes assigned macro; configure button opens picker.
class FavoriteMacroWidget : public PanelWidget {
  public:
    /// @param instance_id e.g. "favorite_macro:1", "favorite_macro:7"
    explicit FavoriteMacroWidget(const std::string& instance_id);
```

Update `widget_id_` comment (line 56):
```cpp
    std::string widget_id_; ///< Instance ID, e.g. "favorite_macro:1"
```

- [ ] **Step 4: Update favorite_macro_widget.cpp registration**

Replace `register_favorite_macro_widgets()` (lines 28-42):

```cpp
void register_favorite_macro_widgets() {
    register_widget_factory("favorite_macro", [](const std::string& id) {
        return std::make_unique<FavoriteMacroWidget>(id);
    });
    // Register XML callbacks early — before any XML is parsed
    lv_xml_register_event_cb(nullptr, "favorite_macro_clicked_cb",
                             FavoriteMacroWidget::clicked_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_picker_backdrop_cb",
                             FavoriteMacroWidget::picker_backdrop_cb);
    lv_xml_register_event_cb(nullptr, "fav_macro_picker_done_cb",
                             FavoriteMacroWidget::picker_done_cb);
}
```

- [ ] **Step 5: Update panel_widget_registry.cpp**

Replace the favorite_macro section in `s_widget_defs` and `init_widget_registrations()`:

In `s_widget_defs`, where the comment `// favorite_macro_1..5 generated in init_widget_registrations()` is (before `clock`), replace with a static entry:
```cpp
    {"favorite_macro", "Macro Button", "play", "Run a configured macro with one tap", "Macro Button", nullptr, nullptr, false, 1, 1, 1, 1, 2, 1, true},
```

Note: `multi_instance` replaces `catalog_group` (same position in the struct). Since `catalog_group` was a pointer defaulting to `nullptr`, and `multi_instance` is a `bool` defaulting to `false`, existing entries that relied on the default `nullptr` will now get the default `false` — no changes needed to other entries. Only `favorite_macro` gets `true`.

Remove from `init_widget_registrations()`:
- The `kFavMacroIds[]` array (lines 121-124)
- The `static_assert` (line 125)
- The entire favorite_macro insertion block (lines 135-155)

Remove `#include "favorite_macro_widget.h"` from the top if `kMaxFavoriteMacroSlots` was the only reason for it.

- [ ] **Step 6: Add config migration to PanelWidgetConfig::load()**

In `src/system/panel_widget_config.cpp`, in `load()`, after line 63 (`std::string id = item["id"].get<std::string>();`), add migration:

```cpp
        // Migration: favorite_macro_N → favorite_macro:N
        {
            static const std::string prefix = "favorite_macro_";
            if (id.size() > prefix.size() && id.substr(0, prefix.size()) == prefix) {
                auto suffix = id.substr(prefix.size());
                bool all_digits = !suffix.empty() && std::all_of(
                    suffix.begin(), suffix.end(), [](char c) { return c >= '0' && c <= '9'; });
                if (all_digits) {
                    std::string new_id = "favorite_macro:" + suffix;
                    spdlog::info("[PanelWidgetConfig] Migrating '{}' → '{}'", id, new_id);
                    id = new_id;
                }
            }
        }
```

- [ ] **Step 7: Add multi_instance guard to build_default_grid()**

In `src/system/panel_widget_config.cpp`, add `multi_instance` guard in TWO places:

**1. `build_default_grid()` at line 279:**
```cpp
    for (const auto& def : defs) {
        if (fixed_ids.count(def.id) > 0)
            continue;
        // Multi-instance defs should never appear as bare base IDs in defaults
        if (def.multi_instance)
            continue;
        result.push_back({def.id, def.default_enabled, {}, -1, -1, def.colspan, def.rowspan});
    }
```

**2. `load()` "append new defs" loop at line 107:**
```cpp
    for (const auto& def : get_all_widget_defs()) {
        if (seen_ids.count(def.id) == 0) {
            // Multi-instance defs are user-created — never auto-append bare base IDs
            if (def.multi_instance)
                continue;
            spdlog::debug("[PanelWidgetConfig] Appending new widget: {} (default_enabled={})",
                          def.id, def.default_enabled);
            entries_.push_back({def.id, def.default_enabled, {}, -1, -1, def.colspan, def.rowspan});
        }
    }
```

- [ ] **Step 8: Run all tests**

Run: `make test && ./build/bin/helix-tests "[multi_instance]" -v`
Expected: ALL PASS

Also run: `./build/bin/helix-tests "[panel_widget]" -v`
Expected: ALL PASS (existing tests still work)

- [ ] **Step 9: Smoke test the binary**

Run: `make -j && ./build/bin/helix-screen --test -vv`
Expected: Home screen loads, any existing favorite macro widgets still appear and work. Check logs for migration messages if testing with existing config.

- [ ] **Step 10: Commit**

```bash
git add include/favorite_macro_widget.h src/ui/widgets/favorite_macro_widget.cpp \
  src/ui/panel_widget_registry.cpp src/system/panel_widget_config.cpp \
  tests/unit/test_multi_instance_widget.cpp
git commit -m "refactor(widgets): migrate favorite_macro to multi_instance system (#342)"
```

---

## Task 3: Multi-Instance Catalog & Grid Edit Mode

**Files:**
- Modify: `src/ui/ui_widget_catalog_overlay.cpp:179-304`
- Modify: `include/panel_widget_config.h:37-83`
- Modify: `src/system/panel_widget_config.cpp`
- Modify: `src/ui/grid_edit_mode.cpp:648-649`
- Test: `tests/unit/test_multi_instance_widget.cpp`

- [ ] **Step 1: Write failing test for mint_instance_id**

Add to `tests/unit/test_multi_instance_widget.cpp`:

```cpp
TEST_CASE_METHOD(helix::PanelWidgetConfigFixture,
                 "mint_instance_id generates sequential IDs",
                 "[panel_widget][multi_instance]") {
    setup_with_widgets(nlohmann::json::array());
    PanelWidgetConfig pwc("home", config);
    pwc.load();

    // First mint: base_id:1
    REQUIRE(pwc.mint_instance_id("favorite_macro") == "favorite_macro:1");

    // Simulate adding it to entries
    auto& entries = pwc.mutable_entries();
    entries.push_back({"favorite_macro:1", true, {}, -1, -1, 1, 1});

    // Second mint: base_id:2
    REQUIRE(pwc.mint_instance_id("favorite_macro") == "favorite_macro:2");

    // Add :5 (gap)
    entries.push_back({"favorite_macro:5", true, {}, -1, -1, 1, 1});

    // Next mint should be :6 (monotonic, based on highest existing)
    REQUIRE(pwc.mint_instance_id("favorite_macro") == "favorite_macro:6");
}

TEST_CASE_METHOD(helix::PanelWidgetConfigFixture,
                 "delete_entry removes multi-instance entry entirely",
                 "[panel_widget][multi_instance]") {
    nlohmann::json widgets = nlohmann::json::array({
        {{"id", "favorite_macro:1"}, {"enabled", true}, {"col", 0}, {"row", 0},
         {"colspan", 1}, {"rowspan", 1}},
        {{"id", "favorite_macro:2"}, {"enabled", true}, {"col", 1}, {"row", 0},
         {"colspan", 1}, {"rowspan", 1}},
        {{"id", "power"}, {"enabled", true}, {"col", 2}, {"row", 0},
         {"colspan", 1}, {"rowspan", 1}},
    });
    setup_with_widgets(widgets);
    PanelWidgetConfig pwc("home", config);
    pwc.load();

    REQUIRE(pwc.entries().size() >= 3);

    pwc.delete_entry("favorite_macro:1");

    // Entry should be gone entirely
    bool found = false;
    for (const auto& e : pwc.entries()) {
        if (e.id == "favorite_macro:1") found = true;
    }
    REQUIRE_FALSE(found);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[multi_instance]" -v`
Expected: FAIL (mint_instance_id and delete_entry don't exist)

- [ ] **Step 3: Implement mint_instance_id and delete_entry**

Add to `include/panel_widget_config.h` in the `PanelWidgetConfig` class:

```cpp
    /// Generate next instance ID for a multi-instance widget type.
    /// Scans existing entries for base_id:N, returns base_id:(max_N+1).
    std::string mint_instance_id(const std::string& base_id);

    /// Delete an entry entirely (for multi-instance widget removal).
    /// Single-instance widgets use set_enabled(false) instead.
    void delete_entry(const std::string& id);
```

Add to `src/system/panel_widget_config.cpp`:

```cpp
std::string PanelWidgetConfig::mint_instance_id(const std::string& base_id) {
    int max_n = 0;
    std::string prefix = base_id + ":";
    for (const auto& entry : entries_) {
        if (entry.id.size() > prefix.size() &&
            entry.id.substr(0, prefix.size()) == prefix) {
            auto suffix = entry.id.substr(prefix.size());
            try {
                int n = std::stoi(suffix);
                if (n > max_n) max_n = n;
            } catch (...) {}
        }
    }
    return base_id + ":" + std::to_string(max_n + 1);
}

void PanelWidgetConfig::delete_entry(const std::string& id) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [&id](const PanelWidgetEntry& e) { return e.id == id; }),
        entries_.end());
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[multi_instance]" -v`
Expected: PASS

- [ ] **Step 5: Update widget catalog overlay**

Replace `populate_rows()` in `src/ui/ui_widget_catalog_overlay.cpp` (lines 179-304). The key changes:

Replace the `catalog_group` pre-pass (lines 183-199) and grouped branch (lines 204-249) with `multi_instance` logic:

```cpp
void WidgetCatalogOverlay::populate_rows(lv_obj_t* scroll, const PanelWidgetConfig& config,
                                         WidgetSelectedCallback /*on_select*/) {
    const auto& defs = get_all_widget_defs();

    // Pre-pass: count placed instances per multi_instance base ID
    std::unordered_map<std::string, int> multi_placed_count;
    for (const auto& def : defs) {
        if (!def.multi_instance)
            continue;
        std::string base(def.id);
        std::string prefix = base + ":";
        int count = 0;
        for (const auto& entry : config.entries()) {
            if (entry.enabled && entry.id.size() > prefix.size() &&
                entry.id.substr(0, prefix.size()) == prefix) {
                count++;
            }
        }
        multi_placed_count[base] = count;
    }

    for (const auto& def : defs) {
        if (def.multi_instance) {
            // Multi-instance widget — show one row with placed count
            int placed = multi_placed_count[def.id];

            const char* display_name = def.display_name ? lv_tr(def.display_name) : def.id;
            std::string name_str(display_name);
            if (placed > 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), " (%d %s)", placed, lv_tr("Placed"));
                name_str += buf;
            }

            // Check hardware gate
            bool hardware_gated = false;
            if (def.hardware_gate_subject) {
                lv_subject_t* gate = lv_xml_get_subject(nullptr, def.hardware_gate_subject);
                if (gate && lv_subject_get_int(gate) == 0) {
                    hardware_gated = true;
                }
            }
            if (hardware_gated) {
                const char* hint =
                    def.hardware_gate_hint ? lv_tr(def.hardware_gate_hint) : lv_tr("not detected");
                name_str += std::string(" (") + hint + ")";
            }

            const char* desc = def.description ? lv_tr(def.description) : nullptr;
            lv_obj_t* row = create_row(scroll, name_str.c_str(), def.icon, desc,
                                       def.colspan, def.rowspan,
                                       /*already_placed=*/false, hardware_gated);

            if (!hardware_gated) {
                // Store base ID pointer (from static def table — stable)
                lv_obj_add_event_cb(
                    row,
                    [](lv_event_t* ev) {
                        auto* base_id = static_cast<const char*>(lv_event_get_user_data(ev));
                        if (!base_id)
                            return;
                        // Mint new instance ID and pass by value to callback
                        auto& wc = helix::PanelWidgetManager::instance().get_widget_config("home");
                        std::string new_id = wc.mint_instance_id(base_id);
                        spdlog::info("[WidgetCatalog] Minted multi-instance widget: {}", new_id);
                        auto cb = g_catalog_state.on_select;
                        close_catalog();
                        if (cb) {
                            cb(new_id);
                        }
                    },
                    LV_EVENT_CLICKED, const_cast<char*>(def.id));
            }
        } else {
            // Single-instance widget — existing logic (lines 251-301 unchanged)
            // ... keep existing single-instance code ...
        }
    }
}
```

Keep the existing single-instance branch (the `else` block from the original lines 250-301) unchanged.

- [ ] **Step 6: Update grid_edit_mode.cpp for multi-instance removal**

In `src/ui/grid_edit_mode.cpp`, at the widget removal section (around line 648-649), change the removal logic to delete multi-instance entries:

```cpp
    // Check if this is a multi-instance widget (ID contains ':')
    const auto& widget_id = entries[static_cast<size_t>(config_index)].id;
    if (widget_id.find(':') != std::string::npos) {
        // Multi-instance: delete entry entirely
        config_->delete_entry(widget_id);
    } else {
        // Single-instance: disable
        config_->set_enabled(static_cast<size_t>(config_index), false);
    }
```

- [ ] **Step 7: Run all tests and smoke test**

Run: `make test && ./build/bin/helix-tests "[panel_widget]" -v`
Expected: ALL PASS

Run: `make -j && ./build/bin/helix-screen --test -vv`
Expected: Long-press home → widget catalog → "Macro Button" shows correctly. Can add multiple instances. Can remove via X button.

- [ ] **Step 8: Commit**

```bash
git add src/ui/ui_widget_catalog_overlay.cpp include/panel_widget_config.h \
  src/system/panel_widget_config.cpp src/ui/grid_edit_mode.cpp \
  tests/unit/test_multi_instance_widget.cpp
git commit -m "feat(widgets): multi-instance catalog, minting, and deletion (#342)"
```

---

## Task 4: PowerDeviceState Singleton

**Files:**
- Create: `include/power_device_state.h`
- Create: `src/printer/power_device_state.cpp`
- Modify: `src/application/application.cpp:1919-1928`
- Modify: `src/api/moonraker_discovery_sequence.cpp:285`
- Modify: `src/api/moonraker_client_mock.cpp:603-611`
- Test: `tests/unit/test_power_device_state.cpp` (create)

- [ ] **Step 1: Write failing test for PowerDeviceState**

Create `tests/unit/test_power_device_state.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "power_device_state.h"

#include "moonraker_types.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("PowerDeviceState tracks device state",
          "[power_device_state]") {
    auto& state = PowerDeviceState::instance();

    // Set up devices from discovery
    std::vector<PowerDevice> devices = {
        {"printer_psu", "gpio", "off", false},
        {"chamber_light", "klipper_device", "on", true},
    };
    state.set_devices(devices);

    REQUIRE(state.device_names().size() == 2);
    REQUIRE(state.is_locked_while_printing("chamber_light") == true);
    REQUIRE(state.is_locked_while_printing("printer_psu") == false);

    // Get status subjects
    SubjectLifetime lt;
    auto* psu_subj = state.get_status_subject("printer_psu", lt);
    REQUIRE(psu_subj != nullptr);
    REQUIRE(lv_subject_get_int(psu_subj) == 0); // off

    auto* light_subj = state.get_status_subject("chamber_light", lt);
    REQUIRE(light_subj != nullptr);
    REQUIRE(lv_subject_get_int(light_subj) == 1); // on

    // Unknown device returns nullptr
    REQUIRE(state.get_status_subject("nonexistent", lt) == nullptr);

    // Cleanup
    state.deinit_subjects();
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[power_device_state]" -v`
Expected: Compilation error (PowerDeviceState doesn't exist)

- [ ] **Step 3: Create power_device_state.h**

Create `include/power_device_state.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "moonraker_types.h"
#include "ui_observer_guard.h"

#include "hv/json.hpp"
#include <lvgl/lvgl.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class MoonrakerAPI;

namespace helix {

/// Centralized reactive state for Moonraker power devices.
/// Subscribes to notify_power_changed WebSocket events and exposes
/// per-device LVGL subjects for widget observation.
///
/// Status subject values: 0=off, 1=on, 2=locked (locked_while_printing + active print)
class PowerDeviceState {
  public:
    static PowerDeviceState& instance();

    /// Register for notify_power_changed WebSocket events.
    void subscribe(MoonrakerAPI& api);
    void unsubscribe(MoonrakerAPI& api);

    /// Initialize per-device subjects from discovery data.
    /// Registers cleanup with StaticSubjectRegistry on first call.
    void set_devices(const std::vector<PowerDevice>& devices);

    /// Per-device status subject (dynamic — requires SubjectLifetime).
    /// Returns nullptr if device not found.
    lv_subject_t* get_status_subject(const std::string& device, SubjectLifetime& lt);

    bool is_locked_while_printing(const std::string& device) const;
    std::vector<std::string> device_names() const;

    /// Shutdown cleanup — deinits all subjects and clears state.
    void deinit_subjects();

  private:
    PowerDeviceState() = default;

    void on_power_changed(const nlohmann::json& msg);
    void reevaluate_lock_states();

    struct DeviceInfo {
        std::string name;
        std::string type;
        bool locked_while_printing = false;
        std::unique_ptr<lv_subject_t> status_subject; // heap-allocated for pointer stability
        SubjectLifetime lifetime;                      // shared_ptr<bool> for observer safety
        int raw_status = 0;                            // 0=off, 1=on (before lock evaluation)
    };

    std::unordered_map<std::string, DeviceInfo> devices_;
    ObserverGuard print_state_observer_;
    bool subjects_initialized_ = false;
};

} // namespace helix
```

- [ ] **Step 4: Create power_device_state.cpp**

Create `src/printer/power_device_state.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "power_device_state.h"

#include "moonraker_api.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

namespace helix {

PowerDeviceState& PowerDeviceState::instance() {
    static PowerDeviceState instance;
    return instance;
}

void PowerDeviceState::subscribe(MoonrakerAPI& api) {
    api.register_method_callback("notify_power_changed", "PowerDeviceState",
                                 [this](const nlohmann::json& msg) {
                                     on_power_changed(msg);
                                 });
    spdlog::debug("[PowerDeviceState] Subscribed to notify_power_changed");
}

void PowerDeviceState::unsubscribe(MoonrakerAPI& api) {
    api.unregister_method_callback("notify_power_changed", "PowerDeviceState");
    print_state_observer_.reset();
    deinit_subjects();
    spdlog::debug("[PowerDeviceState] Unsubscribed and cleaned up");
}

void PowerDeviceState::set_devices(const std::vector<PowerDevice>& devices) {
    // Deinit existing subjects before recreating
    if (subjects_initialized_) {
        deinit_subjects();
    }

    for (const auto& dev : devices) {
        DeviceInfo info;
        info.name = dev.device;
        info.type = dev.type;
        info.locked_while_printing = dev.locked_while_printing;
        info.raw_status = (dev.status == "on") ? 1 : 0;
        info.status_subject = std::make_unique<lv_subject_t>();
        lv_subject_init_int(info.status_subject.get(), info.raw_status);
        info.lifetime = std::make_shared<bool>(true);
        devices_[dev.device] = std::move(info);
    }

    // Register shutdown cleanup on first call
    if (!subjects_initialized_) {
        StaticSubjectRegistry::instance().register_deinit(
            "PowerDeviceState", []() { PowerDeviceState::instance().deinit_subjects(); });
        subjects_initialized_ = true;
    }

    // Observe print state for lock evaluation
    lv_subject_t* print_subj = lv_xml_get_subject(nullptr, "print_state");
    if (print_subj) {
        print_state_observer_ = ui::observe_int_sync<PowerDeviceState>(
            print_subj, this, [](PowerDeviceState* self, int /*value*/) {
                self->reevaluate_lock_states();
            });
    }

    reevaluate_lock_states();

    spdlog::info("[PowerDeviceState] Initialized {} power devices", devices_.size());
}

lv_subject_t* PowerDeviceState::get_status_subject(const std::string& device,
                                                    SubjectLifetime& lt) {
    auto it = devices_.find(device);
    if (it == devices_.end()) {
        lt.reset();
        return nullptr;
    }
    lt = it->second.lifetime;
    return it->second.status_subject.get();
}

bool PowerDeviceState::is_locked_while_printing(const std::string& device) const {
    auto it = devices_.find(device);
    return it != devices_.end() && it->second.locked_while_printing;
}

std::vector<std::string> PowerDeviceState::device_names() const {
    std::vector<std::string> names;
    names.reserve(devices_.size());
    for (const auto& [name, info] : devices_) {
        names.push_back(name);
    }
    return names;
}

void PowerDeviceState::deinit_subjects() {
    print_state_observer_.reset();
    for (auto& [name, info] : devices_) {
        if (info.status_subject) {
            lv_subject_deinit(info.status_subject.get());
            info.status_subject.reset();
        }
    }
    devices_.clear();
    spdlog::debug("[PowerDeviceState] Subjects deinitialized");
}

void PowerDeviceState::on_power_changed(const nlohmann::json& msg) {
    // Moonraker sends: {"method":"notify_power_changed","params":[{"device":"name","status":"on"}]}
    if (!msg.contains("params") || !msg["params"].is_array() || msg["params"].empty())
        return;

    const auto& params = msg["params"][0];
    if (!params.contains("device") || !params.contains("status"))
        return;

    std::string device = params["device"].get<std::string>();
    std::string status = params["status"].get<std::string>();
    int raw = (status == "on") ? 1 : 0;

    ui::queue_update("PowerDeviceState::on_power_changed", [this, device, raw]() {
        auto it = devices_.find(device);
        if (it == devices_.end()) {
            spdlog::debug("[PowerDeviceState] Ignoring unknown device: {}", device);
            return;
        }
        it->second.raw_status = raw;

        // Apply lock evaluation
        int effective = raw;
        if (it->second.locked_while_printing) {
            lv_subject_t* print_subj = lv_xml_get_subject(nullptr, "print_state");
            if (print_subj && lv_subject_get_int(print_subj) == 1) {
                effective = 2; // locked
            }
        }

        if (it->second.status_subject) {
            lv_subject_set_int(it->second.status_subject.get(), effective);
        }
        spdlog::debug("[PowerDeviceState] {} → {} (effective={})", device,
                       raw ? "on" : "off", effective);
    });
}

void PowerDeviceState::reevaluate_lock_states() {
    lv_subject_t* print_subj = lv_xml_get_subject(nullptr, "print_state");
    bool printing = print_subj && lv_subject_get_int(print_subj) == 1;

    for (auto& [name, info] : devices_) {
        if (!info.status_subject)
            continue;
        int effective = info.raw_status;
        if (info.locked_while_printing && printing) {
            effective = 2;
        }
        lv_subject_set_int(info.status_subject.get(), effective);
    }
}

} // namespace helix
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make test && ./build/bin/helix-tests "[power_device_state]" -v`
Expected: PASS

- [ ] **Step 6: Wire into Application and discovery**

In `src/application/application.cpp`, near the existing `register_method_callback` calls (around line 1919-1928). Note: the existing code uses `c->client->register_method_callback()` directly. `PowerDeviceState::subscribe()` takes `MoonrakerAPI&` which wraps the client. Find the API object — it's typically `c->api` or accessible via `m_moonraker->api()`. Wire it:

```cpp
    helix::PowerDeviceState::instance().subscribe(*c->api);
```

In the teardown/disconnect section (near the `unregister_method_callback` calls), add:

```cpp
    helix::PowerDeviceState::instance().unsubscribe(*c->api);
```

If `c->api` is not the right path, check how `MoonrakerAPI::register_method_callback()` is accessed — it just forwards to `client_.register_method_callback()`. Use whichever `MoonrakerAPI&` reference is available in the connection context.

In `src/api/moonraker_discovery_sequence.cpp`, the discovery callback (line 278-285) currently only extracts `device_count`. Expand it to parse full `PowerDevice` structs and call `set_devices()`:

```cpp
    [](json response) {
        std::vector<PowerDevice> devices;
        if (response.contains("result") && response["result"].contains("devices")) {
            for (const auto& [name, info] : response["result"]["devices"].items()) {
                PowerDevice dev;
                dev.device = name;
                dev.type = info.value("type", "");
                dev.status = info.value("status", "off");
                dev.locked_while_printing = info.value("locked_while_printing", false);
                devices.push_back(dev);
            }
        }
        spdlog::info("[Moonraker Client] Power device detection: {} devices", devices.size());
        get_printer_state().set_power_device_count(static_cast<int>(devices.size()));
        helix::PowerDeviceState::instance().set_devices(devices);
    },
```

This replaces the existing callback that only counted devices. The `PowerDevice` struct is already defined in `moonraker_types.h`. Add `#include "power_device_state.h"` to the file.

- [ ] **Step 7: Update mock client**

In `src/api/moonraker_client_mock.cpp` (around line 609), after `set_power_device_count(4)`, add mock device initialization:

```cpp
    // Initialize PowerDeviceState with mock devices
    std::vector<PowerDevice> mock_power_devices = {
        {"printer_psu", "gpio", "on", false},
        {"chamber_light", "klipper_device", "on", true},
        {"exhaust_fan", "klipper_device", "off", false},
        {"led_strip", "gpio", "on", false},
    };
    helix::PowerDeviceState::instance().set_devices(mock_power_devices);
```

Add `#include "power_device_state.h"` to the includes.

- [ ] **Step 8: Run full test suite and smoke test**

Run: `make test-run`
Expected: ALL PASS

Run: `make -j && ./build/bin/helix-screen --test -vv`
Expected: Log shows "PowerDeviceState Initialized 4 power devices"

- [ ] **Step 9: Commit**

```bash
git add include/power_device_state.h src/printer/power_device_state.cpp \
  src/application/application.cpp src/api/moonraker_discovery_sequence.cpp \
  src/api/moonraker_client_mock.cpp tests/unit/test_power_device_state.cpp
git commit -m "feat(power): add PowerDeviceState with WebSocket-driven live state (#467)"
```

---

## Task 5: Power Device Widget — XML & Registration

**Files:**
- Create: `ui_xml/components/panel_widget_power_device.xml`
- Modify: `src/ui/panel_widget_registry.cpp`

- [ ] **Step 1: Create XML component**

Create `ui_xml/components/panel_widget_power_device.xml`:

```xml
<component>
  <view name="panel_widget_power_device" extends="lv_obj"
        height="100%" flex_grow="1" style_pad_all="0" scrollable="false"
        flex_flow="column" style_flex_main_place="center"
        style_flex_cross_place="center" style_flex_track_place="center"
        clickable="true">
    <bind_state_if_not_eq subject="printer_connection_state"
                          state="disabled" ref_value="2"/>
    <event_cb trigger="clicked" callback="power_device_clicked_cb"/>

    <!-- Circular badge background -->
    <lv_obj name="power_badge" width="48" height="48"
            style_radius="24" style_bg_opa="40"
            style_bg_color="#secondary" style_border_width="0"
            clickable="false" scrollable="false"
            flex_flow="column" style_flex_main_place="center"
            style_flex_cross_place="center">
      <icon name="power_icon" src="power_cycle" size="md"
            variant="secondary" clickable="false" event_bubble="true"/>
    </lv_obj>

    <!-- Lock icon (hidden by default) -->
    <icon name="power_lock_icon" src="lock" size="xs" variant="muted"
          clickable="false" event_bubble="true" hidden="true"/>

    <!-- Device name -->
    <text_tiny name="power_device_name" text="Configure"
               translation_tag="Configure"
               style_text_align="center" style_text_color="#text"
               long_mode="wrap" width="100%"
               clickable="false" event_bubble="true"/>

    <!-- ON/OFF/LOCKED status -->
    <text_tiny name="power_device_status" text=""
               style_text_align="center" style_text_color="#text_muted"
               clickable="false" event_bubble="true"/>
  </view>
</component>
```

- [ ] **Step 2: Add power_device def to registry**

In `src/ui/panel_widget_registry.cpp`, add to `s_widget_defs` (near the existing `"power"` entry). Add a forward declaration for the registration function and add the def entry:

```cpp
void register_power_device_widget();
```

In `s_widget_defs`, after the `"power"` entry:
```cpp
    {"power_device", "Power Device", "power_cycle", "Toggle a Moonraker power device", "Power Device", "power_device_count", "Requires Moonraker power device", false, 1, 1, 1, 1, 1, 1, true},
```

In `init_widget_registrations()`, add:
```cpp
    register_power_device_widget();
```

- [ ] **Step 3: Register XML component**

In `src/xml_registration.cpp`, find where `power_device_row.xml` is registered and add:
```cpp
    register_xml("panel_widget_power_device.xml");
```

Check the `register_xml` pattern — components in `ui_xml/components/` may need the `components/` prefix. Search for how `panel_widget_favorite_macro.xml` is registered.

- [ ] **Step 4: Smoke test**

Run: `make -j && ./build/bin/helix-screen --test -vv`
Expected: No errors related to power_device XML. Widget catalog should show "Power Device" entry.

- [ ] **Step 5: Commit**

```bash
git add ui_xml/components/panel_widget_power_device.xml \
  src/ui/panel_widget_registry.cpp src/xml_registration.cpp
git commit -m "feat(power): add power_device widget XML and registry entry (#467)"
```

---

## Task 6: Power Device Widget — C++ Implementation

**Files:**
- Create: `include/power_device_widget.h`
- Create: `src/ui/widgets/power_device_widget.cpp`

- [ ] **Step 1: Create power_device_widget.h**

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"
#include "ui_observer_guard.h"

#include <memory>
#include <string>

class MoonrakerAPI;

namespace helix {

class PowerDeviceWidget : public PanelWidget {
  public:
    explicit PowerDeviceWidget(const std::string& instance_id);
    ~PowerDeviceWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    bool has_edit_configure() const override { return true; }
    bool on_edit_configure() override;
    std::string get_component_name() const override {
        return "panel_widget_power_device";
    }
    const char* id() const override {
        return instance_id_.c_str();
    }

    void handle_clicked();

    static void power_device_clicked_cb(lv_event_t* e);

  private:
    std::string instance_id_;
    std::string device_name_;

    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* parent_screen_ = nullptr;
    lv_obj_t* badge_obj_ = nullptr;
    lv_obj_t* icon_obj_ = nullptr;
    lv_obj_t* name_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* lock_icon_ = nullptr;

    SubjectLifetime subject_lifetime_;
    ObserverGuard status_observer_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(false);

    MoonrakerAPI* get_api() const;
    void update_display(int status); // 0=off, 1=on, 2=locked
    void show_device_picker();
    void save_config();
};

} // namespace helix
```

- [ ] **Step 2: Create power_device_widget.cpp**

Create `src/ui/widgets/power_device_widget.cpp`. Key methods:

**Registration function:**
```cpp
namespace helix {
void register_power_device_widget() {
    register_widget_factory("power_device", [](const std::string& id) {
        return std::make_unique<PowerDeviceWidget>(id);
    });
    lv_xml_register_event_cb(nullptr, "power_device_clicked_cb",
                             PowerDeviceWidget::power_device_clicked_cb);
}
```

**Constructor/destructor:**
```cpp
PowerDeviceWidget::PowerDeviceWidget(const std::string& instance_id)
    : instance_id_(instance_id) {
    *alive_ = true;
}

PowerDeviceWidget::~PowerDeviceWidget() {
    *alive_ = false;
}
```

**set_config:**
```cpp
void PowerDeviceWidget::set_config(const nlohmann::json& config) {
    if (config.contains("device") && config["device"].is_string()) {
        device_name_ = config["device"].get<std::string>();
    }
}
```

**attach — look up LVGL objects by name, observe status subject:**
```cpp
void PowerDeviceWidget::attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) {
    widget_obj_ = widget_obj;
    parent_screen_ = parent_screen;
    lv_obj_set_user_data(widget_obj, this);

    badge_obj_ = lv_obj_find_by_name(widget_obj, "power_badge");
    icon_obj_ = lv_obj_find_by_name(widget_obj, "power_icon");
    name_label_ = lv_obj_find_by_name(widget_obj, "power_device_name");
    status_label_ = lv_obj_find_by_name(widget_obj, "power_device_status");
    lock_icon_ = lv_obj_find_by_name(widget_obj, "power_lock_icon");

    if (!device_name_.empty()) {
        // Show device display name
        if (name_label_) {
            lv_label_set_text(name_label_, device_name_.c_str());
        }

        // Observe status subject from PowerDeviceState
        auto* subj = PowerDeviceState::instance().get_status_subject(
            device_name_, subject_lifetime_);
        if (subj) {
            status_observer_ = ui::observe_int_sync<PowerDeviceWidget>(
                subj, this, [](PowerDeviceWidget* self, int value) {
                    self->update_display(value);
                }, subject_lifetime_);
        }
    } else {
        // Unconfigured state
        update_display(-1);
    }
}
```

**detach:**
```cpp
void PowerDeviceWidget::detach() {
    status_observer_.reset();
    subject_lifetime_.reset();
    if (widget_obj_) {
        lv_obj_set_user_data(widget_obj_, nullptr);
    }
    widget_obj_ = nullptr;
    parent_screen_ = nullptr;
    badge_obj_ = nullptr;
    icon_obj_ = nullptr;
    name_label_ = nullptr;
    status_label_ = nullptr;
    lock_icon_ = nullptr;
}
```

**update_display — set colors, text, lock icon based on state:**

Use `ui_theme_get_color()` for design tokens. Set badge bg color/opacity, icon variant, status text, and lock icon visibility. See the visual states table in the spec. Use `lv_obj_set_style_bg_color()` etc. on `badge_obj_` (this falls under the "widget pool recycling" exception to declarative UI rules). Reference how `PowerWidget` or `FavoriteMacroWidget` update their icons for the exact LVGL calls.

**handle_clicked:**
```cpp
void PowerDeviceWidget::handle_clicked() {
    if (device_name_.empty()) {
        show_device_picker();
        return;
    }
    // Check lock state
    auto* subj = PowerDeviceState::instance().get_status_subject(
        device_name_, subject_lifetime_);
    if (subj && lv_subject_get_int(subj) == 2) {
        return; // Locked during print
    }
    // Toggle via Moonraker API
    auto* api = get_api();
    if (api) {
        api->set_device_power(device_name_, "toggle", nullptr, nullptr);
    }
}
```

**power_device_clicked_cb — static XML callback:**
```cpp
void PowerDeviceWidget::power_device_clicked_cb(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PowerDeviceWidget] power_device_clicked_cb");
    auto* widget = panel_widget_from_event<PowerDeviceWidget>(e);
    if (widget) {
        widget->handle_clicked();
    }
    LVGL_SAFE_EVENT_CB_END;
}
```

**on_edit_configure:**
```cpp
bool PowerDeviceWidget::on_edit_configure() {
    show_device_picker();
    return true;
}
```

**show_device_picker — follow FavoriteMacroWidget's picker pattern.** Fetch device names from `PowerDeviceState::instance().device_names()`, show a scrollable list overlay, user taps one, call `save_config()`. This is the most complex part — study `FavoriteMacroWidget::show_macro_picker()` for the pattern.

**save_config:**
```cpp
void PowerDeviceWidget::save_config() {
    nlohmann::json config;
    config["device"] = device_name_;
    save_widget_config(config);
}
```

- [ ] **Step 3: Build and fix compilation**

Run: `make -j`
Expected: Compiles cleanly. Fix any issues.

- [ ] **Step 4: Smoke test**

Run: `./build/bin/helix-screen --test -vv`
Expected: Can add "Power Device" from catalog. Shows "Configure" text. Tapping shows device picker (if implemented). After selecting a device, shows ON/OFF state with circular badge.

- [ ] **Step 5: Test toggle and lock behavior**

In `--test` mode, verify:
- Tapping an ON device sends toggle and updates to OFF
- Tapping an OFF device sends toggle and updates to ON
- During simulated print, locked devices show "LOCKED" and ignore taps

- [ ] **Step 6: Commit**

```bash
git add include/power_device_widget.h src/ui/widgets/power_device_widget.cpp
git commit -m "feat(power): add PowerDeviceWidget with live state and device picker (#467)"
```

---

## Task 7: Final Integration & Cleanup

**Files:**
- Various cleanup and verification

- [ ] **Step 1: Run full test suite**

Run: `make test-run`
Expected: ALL PASS — no regressions

- [ ] **Step 2: Test config migration end-to-end**

Create a test config with old-style `favorite_macro_1` entries, start the binary, verify they migrate correctly to `favorite_macro:1` format. Check logs for migration messages.

- [ ] **Step 3: Test multi-instance UX flow end-to-end**

In `--test` mode:
1. Long-press home → enter edit mode
2. Open widget catalog
3. Add "Macro Button" → verify `favorite_macro:1` created
4. Add another → verify `favorite_macro:2` created
5. Remove first → verify entry deleted (not just disabled)
6. Add "Power Device" → verify `power_device:1` created
7. Configure with a device → verify ON/OFF toggle works
8. Add another power device → verify independent config

- [ ] **Step 4: Verify PowerPanel still works**

Open the Power panel overlay (if the `power` widget is still on the home screen). Verify all devices still show correctly with toggles.

- [ ] **Step 5: Commit any remaining fixes**

```bash
git add -u
git commit -m "fix(widgets): final multi-instance integration fixes (#342, #467)"
```
