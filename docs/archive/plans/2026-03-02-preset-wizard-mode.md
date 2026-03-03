# Preset-Aware Wizard Mode Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Allow known-hardware builds (AD5M, CC1, etc.) to skip hardware wizard steps and show only calibration, language, connection (if needed), and telemetry opt-in.

**Architecture:** Add a `preset_mode` flag to the wizard that's set when `Config` has a non-empty `/preset` field. In preset mode, `on_next_clicked()` and `ui_wizard_precalculate_skips()` force-skip hardware steps (4-11) and summary (12). A new telemetry step becomes the final step. Connection step auto-validates before showing.

**Tech Stack:** C++17, LVGL 9.5, XML declarative UI, Catch2 tests

**Key docs:** `docs/plans/2026-03-02-preset-wizard-mode-design.md`, `docs/devel/LVGL9_XML_GUIDE.md`

---

### Task 1: Add `has_preset()` / `get_preset()` to Config

**Files:**
- Modify: `include/config.h`
- Modify: `src/system/config.cpp`
- Test: `tests/unit/test_config.cpp` (or create if absent)

**Step 1: Write the failing tests**

Add to the test file (find existing config tests or create new):

```cpp
TEST_CASE("Config::has_preset returns false for default config", "[config][preset]") {
    Config config;
    config.init_from_string(R"({"wizard_completed": false})");
    REQUIRE(config.has_preset() == false);
    REQUIRE(config.get_preset().empty());
}

TEST_CASE("Config::has_preset returns true when preset field exists", "[config][preset]") {
    Config config;
    config.init_from_string(R"({"preset": "ad5m", "wizard_completed": false})");
    REQUIRE(config.has_preset() == true);
    REQUIRE(config.get_preset() == "ad5m");
}

TEST_CASE("Config::has_preset handles empty string preset", "[config][preset]") {
    Config config;
    config.init_from_string(R"({"preset": "", "wizard_completed": false})");
    REQUIRE(config.has_preset() == false);
}
```

**Step 2: Run tests to verify they fail**

Run: `make test-run` or `./build/bin/helix-tests "[config][preset]"`
Expected: FAIL — `has_preset()` and `get_preset()` don't exist

**Step 3: Implement**

In `include/config.h`, add to the `Config` class public section:

```cpp
/// Check if this config was loaded from a platform preset
bool has_preset() const;

/// Get the preset name (e.g., "ad5m"), or empty string if no preset
std::string get_preset() const;
```

In `src/system/config.cpp`:

```cpp
bool Config::has_preset() const {
    static const nlohmann::json::json_pointer ptr("/preset");
    return data.contains(ptr) && data[ptr].is_string() && !data[ptr].get<std::string>().empty();
}

std::string Config::get_preset() const {
    if (has_preset()) {
        return data[nlohmann::json::json_pointer("/preset")].get<std::string>();
    }
    return "";
}
```

**Note:** Check how other Config methods access `data` — use the same pattern. The `data` member is a `nlohmann::json` object. Check if `init_from_string()` exists or if you need to use a different test setup (some Config tests may use file-based init).

**Step 4: Run tests to verify they pass**

Run: `./build/bin/helix-tests "[config][preset]" -v`
Expected: PASS

**Step 5: Commit**

```
git add include/config.h src/system/config.cpp tests/unit/test_config.cpp
git commit -m "feat(wizard): add Config::has_preset() and get_preset() for preset detection"
```

---

### Task 2: Add preset skip flags to WizardSkipFlags

**Files:**
- Modify: `include/wizard_step_logic.h`
- Modify: `src/system/wizard_step_logic.cpp`
- Modify: `tests/unit/test_wizard_step_logic.cpp` (find existing or check test_wizard_validation.cpp for patterns)

**Step 1: Write failing tests for preset mode skip logic**

The `WizardSkipFlags` needs new flags for the steps that are currently never-skip (connection=3, printer_identify=4, heater_select=5, fan_select=6, summary=12). In preset mode these will be skippable.

```cpp
TEST_CASE("Preset mode: skip hardware steps", "[wizard][step_logic][preset]") {
    helix::WizardSkipFlags flags{};
    // Preset mode skips: wifi(2), printer_identify(4), heater(5), fan(6),
    // ams(7), led(8), filament(9), probe(10), input_shaper(11), summary(12)
    flags.wifi = true;
    flags.printer_identify = true;
    flags.heater_select = true;
    flags.fan_select = true;
    flags.ams = true;
    flags.led = true;
    flags.filament = true;
    flags.probe = true;
    flags.input_shaper = true;
    flags.summary = true;

    // With only touch_cal(0), language(1), connection(3) remaining + telemetry(13)
    // But telemetry is a new step — for now test without it
    // Steps shown: 0, 1, 3 = 3 steps
    REQUIRE(helix::wizard_calculate_display_total(flags) == 3);

    // Step 0 displays as step 1
    REQUIRE(helix::wizard_calculate_display_step(0, flags) == 1);
    // Step 1 displays as step 2
    REQUIRE(helix::wizard_calculate_display_step(1, flags) == 2);
    // Step 3 displays as step 3
    REQUIRE(helix::wizard_calculate_display_step(3, flags) == 3);
}

TEST_CASE("Preset mode: next_step skips hardware", "[wizard][step_logic][preset]") {
    helix::WizardSkipFlags flags{};
    flags.wifi = true;
    flags.printer_identify = true;
    flags.heater_select = true;
    flags.fan_select = true;
    flags.ams = true;
    flags.led = true;
    flags.filament = true;
    flags.probe = true;
    flags.input_shaper = true;
    flags.summary = true;

    REQUIRE(helix::wizard_next_step(0, flags) == 1);   // touch_cal → language
    REQUIRE(helix::wizard_next_step(1, flags) == 3);   // language → connection (skip wifi)
    REQUIRE(helix::wizard_next_step(3, flags) == -1);   // connection → end (all remaining skipped)
}
```

**Step 2: Run tests — expected FAIL** (new flags don't exist)

**Step 3: Add new skip flags**

In `include/wizard_step_logic.h`, add to `WizardSkipFlags`:
```cpp
struct WizardSkipFlags {
    bool touch_cal = false;
    bool language = false;
    bool wifi = false;
    bool printer_identify = false;  // NEW
    bool heater_select = false;     // NEW
    bool fan_select = false;        // NEW
    bool ams = false;
    bool led = false;
    bool filament = false;
    bool probe = false;
    bool input_shaper = false;
    bool summary = false;           // NEW
};
```

In `src/system/wizard_step_logic.cpp`, update `is_step_skipped()`:
```cpp
static bool is_step_skipped(int step, const WizardSkipFlags& skips) {
    switch (step) {
    case 0:  return skips.touch_cal;
    case 1:  return skips.language;
    case 2:  return skips.wifi;
    case 4:  return skips.printer_identify;
    case 5:  return skips.heater_select;
    case 6:  return skips.fan_select;
    case 7:  return skips.ams;
    case 8:  return skips.led;
    case 9:  return skips.filament;
    case 10: return skips.probe;
    case 11: return skips.input_shaper;
    case 12: return skips.summary;
    default: return false;
    }
}
```

**Step 4: Run tests — expected PASS**

**Step 5: Commit**

```
git add include/wizard_step_logic.h src/system/wizard_step_logic.cpp tests/unit/...
git commit -m "feat(wizard): add skip flags for hardware steps and summary"
```

---

### Task 3: Add preset_mode flag to ui_wizard.cpp

**Files:**
- Modify: `src/ui/ui_wizard.cpp` (lines ~70-102 for static vars, ~385-396 for get_current_skip_flags, ~534-588 for precalculate_skips)

**Step 1: Add static preset_mode flag**

Near line 102 (after `static bool skips_precalculated`):
```cpp
// Track if wizard is in preset mode (known hardware, abbreviated flow)
static bool preset_mode = false;
```

**Step 2: Set preset_mode during wizard init**

In `ui_wizard_init_subjects()` (find exact location), add:
```cpp
preset_mode = Config::get_instance()->has_preset();
if (preset_mode) {
    spdlog::info("[Wizard] Preset mode active (preset: {})", Config::get_instance()->get_preset());
}
```

Add `#include "config.h"` if not already included.

**Step 3: Update get_current_skip_flags() to include new flags**

At line ~385, update the return to include the new fields:
```cpp
static helix::WizardSkipFlags get_current_skip_flags() {
    return helix::WizardSkipFlags{
        .touch_cal = touch_cal_step_skipped,
        .language = language_step_skipped,
        .wifi = wifi_step_skipped,
        .printer_identify = printer_identify_step_skipped,
        .heater_select = heater_select_step_skipped,
        .fan_select = fan_select_step_skipped,
        .ams = ams_step_skipped,
        .led = led_step_skipped,
        .filament = filament_step_skipped,
        .probe = probe_step_skipped,
        .input_shaper = input_shaper_step_skipped,
        .summary = summary_step_skipped,
    };
}
```

**Step 4: Add new static skip bools**

Near the existing skip flags (lines 78-99), add:
```cpp
static bool printer_identify_step_skipped = false;
static bool heater_select_step_skipped = false;
static bool fan_select_step_skipped = false;
static bool summary_step_skipped = false;
```

**Step 5: Update ui_wizard_precalculate_skips()**

At line ~534, add preset mode skip logic at the TOP of the function:
```cpp
static void ui_wizard_precalculate_skips() {
    spdlog::info("[Wizard] Pre-calculating step skips based on hardware data");

    // Preset mode: skip all hardware steps and summary
    if (preset_mode) {
        printer_identify_step_skipped = true;
        heater_select_step_skipped = true;
        fan_select_step_skipped = true;
        ams_step_skipped = true;
        led_step_skipped = true;
        filament_step_skipped = true;
        probe_step_skipped = true;
        input_shaper_step_skipped = true;
        summary_step_skipped = true;
        spdlog::info("[Wizard] Preset mode: skipping all hardware steps and summary");
    } else {
        // ... existing skip logic unchanged ...
    }

    // ... existing total_skipped calculation (update to include new flags) ...
    skips_precalculated = true;
}
```

**Step 6: Update on_next_clicked() skip chain**

In `on_next_clicked()` (line ~1052 onwards), the existing skip chain checks steps 7-11 individually. In preset mode, these are already flagged, but the chain at lines 1075-1127 checks `should_skip()` on each step instance. For preset mode, we need to skip steps 4-6 too, which currently have no skip check.

Add after the WiFi skip check (line ~1066) and before the precalculate call:
```cpp
    // Preset mode: skip steps 4-6 (printer identify, heaters, fans)
    if (preset_mode) {
        if (next_step >= 4 && next_step <= 6) {
            next_step = 7;  // Jump past hardware config steps
        }
    }
```

The existing skip checks for steps 7-11 will then handle those via the precalculated flags.

Also, update the summary skip — after step 11 skip (line ~1127):
```cpp
    // Skip summary step (12) in preset mode
    if (next_step == 12 && summary_step_skipped) {
        next_step = 13;  // Will be telemetry step
    }
```

**Step 7: Update the "last step" detection**

The existing code at line 1041 uses `current >= STEP_COMPONENT_COUNT` (12) to detect the last step. With the telemetry step at index 13, we need to update:
- `STEP_COMPONENT_COUNT` from 12 to 13
- Add `"wizard_telemetry"` to `STEP_COMPONENT_NAMES[]` at index 13
- The summary step remains at 12; telemetry at 13

Alternatively, in preset mode, if we're on the telemetry step (13) or if we're on connection (3) and there's nothing after, the "Finish" trigger fires. The cleanest approach: add telemetry to the array and bump the count.

**Step 8: Update ui_wizard_navigate_to_step() last-step detection**

At line ~472:
```cpp
    // Determine if this is the last step
    bool is_last_step = (step == STEP_COMPONENT_COUNT);
```

With `STEP_COMPONENT_COUNT = 13`, the telemetry step (13) is the new last step. But in non-preset mode, summary (12) is still the last step. The existing `WizardSkipFlags` handles this — if telemetry is skipped (in non-preset mode), `wizard_next_step()` will return -1 from summary.

Better approach: check if there's no next step:
```cpp
    auto skips = get_current_skip_flags();
    bool is_last_step = (helix::wizard_next_step(step, skips) == -1);
```

**Step 9: Commit**

```
git add src/ui/ui_wizard.cpp
git commit -m "feat(wizard): add preset mode flag and skip hardware steps in preset builds"
```

---

### Task 4: Update preset files

**Files:**
- Modify: `config/presets/ad5m.json`
- Modify: `config/presets/ad5x.json`
- Modify: `config/presets/cc1.json`

**Step 1: Add "preset" field and set wizard_completed to false**

For each file, add `"preset": "<name>"` at the top level and ensure `"wizard_completed": false`.

`ad5m.json`:
```json
{
  "preset": "ad5m",
  "wizard_completed": false,
  ...existing content...
}
```

`ad5x.json`:
```json
{
  "preset": "ad5x",
  "wizard_completed": false,
  ...existing content...
}
```

`cc1.json`:
```json
{
  "preset": "cc1",
  ...already has wizard_completed: false...
}
```

**Step 2: Commit**

```
git add config/presets/
git commit -m "feat(wizard): add preset field to platform config presets"
```

---

### Task 5: Simplify package.sh to convention-based preset lookup

**Files:**
- Modify: `scripts/package.sh` (~line 177)

**Step 1: Replace hardcoded if/elif**

Replace lines 177-185:
```bash
    # Platform-specific default config: use preset for known platforms, template otherwise
    # Presets skip the wizard with pre-configured hardware mappings and touch calibration
    # Printer type is left empty for runtime auto-detection (AD5M vs AD5M Pro)
    # The installer preserves existing configs on upgrade (backup/restore in release.sh)
    local preset_file="${PROJECT_DIR}/config/presets/${platform}.json"
    if [ -f "$preset_file" ]; then
        cp "$preset_file" "$pkg_dir/config/helixconfig.json"
        log_info "  Using ${platform} preset as default config"
    else
        cp "${PROJECT_DIR}/config/helixconfig.json.template" "$pkg_dir/config/helixconfig.json.template" 2>/dev/null || true
    fi
```

**Step 2: Commit**

```
git add scripts/package.sh
git commit -m "refactor(package): convention-based preset lookup by platform target name"
```

---

### Task 6: Create wizard telemetry step

**Files:**
- Create: `include/ui_wizard_telemetry.h`
- Create: `src/ui/ui_wizard_telemetry.cpp`
- Create: `ui_xml/wizard_telemetry.xml`
- Modify: `src/xml_registration.cpp` (add XML registration)
- Modify: `src/ui/ui_wizard.cpp` (add step to switch statements)

**Step 1: Create the XML component**

Look at `ui_xml/wizard_summary.xml` lines 16-42 for the existing telemetry UI. Extract that into `ui_xml/wizard_telemetry.xml`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<component>
  <consts>
    <str name="step_title" value="Help Improve HelixScreen"/>
    <str name="step_subtitle" value="Anonymous usage statistics"/>
  </consts>

  <view name="wizard_telemetry_root"
        extends="lv_obj" width="100%" height="100%" style_bg_opa="0" style_border_width="0"
        style_pad_all="0"
        flex_flow="column" style_flex_main_place="center" style_flex_cross_place="center">

    <ui_card width="100%" flex_grow="1" style_pad_all="#space_lg" style_pad_gap="#space_md"
             flex_flow="column" style_flex_main_place="center" style_flex_cross_place="center"
             scrollable="false">

      <!-- Icon -->
      <icon src="chart_line" size="lg" variant="primary"/>

      <!-- Title -->
      <text_heading text="Help Improve HelixScreen"
                    translation_tag="Help Improve HelixScreen"
                    style_text_align="center"/>

      <!-- Description -->
      <text_body text="Share anonymous usage statistics to help us make HelixScreen better. No personal data is ever collected."
                 translation_tag="Share anonymous usage statistics to help us make HelixScreen better. No personal data is ever collected."
                 style_text_align="center" width="80%"/>

      <!-- Toggle row -->
      <lv_obj width="content" height="content" style_pad_all="#space_sm" style_pad_gap="#space_md"
              flex_flow="row" style_flex_cross_place="center" scrollable="false">
        <text_body text="Enable anonymous statistics"
                   translation_tag="Enable anonymous statistics"/>
        <ui_switch name="wizard_telemetry_toggle" size="tiny">
          <bind_state_if_eq subject="settings_telemetry_enabled" state="checked" ref_value="1"/>
          <event_cb trigger="value_changed" callback="on_wizard_telemetry_changed"/>
        </ui_switch>
      </lv_obj>

      <!-- Learn More button -->
      <ui_button name="btn_telemetry_info" variant="ghost" width="content" height="content">
        <text_small text="Learn More" translation_tag="Learn More"
                    style_text_color="#primary" style_text_decor="underline"
                    clickable="false" event_bubble="true"/>
        <event_cb trigger="clicked" callback="on_wizard_telemetry_info"/>
      </ui_button>

    </ui_card>
  </view>
</component>
```

**Step 2: Create the header file**

`include/ui_wizard_telemetry.h`:
```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl/lvgl.h"

class WizardTelemetryStep {
  public:
    void init_subjects();
    void register_callbacks();
    lv_obj_t* create(lv_obj_t* parent);
    void cleanup();
    bool is_validated() const { return true; } // No validation needed
    const char* get_name() const { return "WizardTelemetry"; }
    bool should_skip() const;

  private:
    lv_obj_t* root_ = nullptr;

    static void on_wizard_telemetry_changed(lv_event_t* e);
    static void on_wizard_telemetry_info(lv_event_t* e);
};

WizardTelemetryStep* get_wizard_telemetry_step();
void destroy_wizard_telemetry_step();
```

**Step 3: Create the source file**

`src/ui/ui_wizard_telemetry.cpp`:

Model after existing steps (e.g., `ui_wizard_summary.cpp`). Key implementation:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_wizard_telemetry.h"
#include "system_settings_manager.h"
#include "toast_manager.h"
#include "ui_modal.h"
#include "safe_event_cb.h"
#include <spdlog/spdlog.h>

static WizardTelemetryStep* s_instance = nullptr;

WizardTelemetryStep* get_wizard_telemetry_step() {
    if (!s_instance) s_instance = new WizardTelemetryStep();
    return s_instance;
}

void destroy_wizard_telemetry_step() {
    delete s_instance;
    s_instance = nullptr;
}

void WizardTelemetryStep::init_subjects() {
    // Telemetry subject (settings_telemetry_enabled) already exists in SystemSettingsManager
    spdlog::debug("[{}] init_subjects", get_name());
}

void WizardTelemetryStep::register_callbacks() {
    spdlog::debug("[{}] Registering callbacks", get_name());
    lv_xml_register_event_cb(nullptr, "on_wizard_telemetry_changed",
                             WizardTelemetryStep::on_wizard_telemetry_changed);
    lv_xml_register_event_cb(nullptr, "on_wizard_telemetry_info",
                             WizardTelemetryStep::on_wizard_telemetry_info);
}

lv_obj_t* WizardTelemetryStep::create(lv_obj_t* parent) {
    spdlog::debug("[{}] Creating telemetry step UI", get_name());
    root_ = (lv_obj_t*)lv_xml_create(parent, "wizard_telemetry", nullptr);
    return root_;
}

void WizardTelemetryStep::cleanup() {
    root_ = nullptr;
}

bool WizardTelemetryStep::should_skip() const {
    // Only shown in preset mode — caller handles this
    return false;
}

// Copy on_wizard_telemetry_changed and on_wizard_telemetry_info
// from WizardSummaryStep (ui_wizard_summary.cpp lines 362-394)
// They are identical — just update the log tag.
void WizardTelemetryStep::on_wizard_telemetry_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[WizardTelemetry] on_wizard_telemetry_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    spdlog::info("[WizardTelemetry] Telemetry toggled: {}", enabled ? "ON" : "OFF");
    SystemSettingsManager::instance().set_telemetry_enabled(enabled);
    if (enabled) {
        ToastManager::instance().show(
            ToastSeverity::SUCCESS,
            lv_tr("Thanks! Anonymous usage data helps improve HelixScreen."), 4000);
    }
    LVGL_SAFE_EVENT_CB_END();
}

void WizardTelemetryStep::on_wizard_telemetry_info(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[WizardTelemetry] on_wizard_telemetry_info");
    spdlog::debug("[WizardTelemetry] Showing telemetry info modal");
    lv_obj_t* dialog = Modal::show("telemetry_info_modal");
    if (dialog) {
        lv_obj_t* ok_btn = lv_obj_find_by_name(dialog, "btn_primary");
        if (ok_btn) {
            lv_obj_add_event_cb(
                ok_btn,
                [](lv_event_t* ev) {
                    auto* dlg = static_cast<lv_obj_t*>(lv_event_get_user_data(ev));
                    Modal::hide(dlg);
                },
                LV_EVENT_CLICKED, dialog);
        }
    }
    LVGL_SAFE_EVENT_CB_END();
}
```

**Step 4: Register XML component**

In `src/xml_registration.cpp`, after the `wizard_summary.xml` registration (line ~493):
```cpp
register_xml("wizard_telemetry.xml");
```

**Step 5: Add telemetry step to wizard switch statements**

In `src/ui/ui_wizard.cpp`:

1. Add to `STEP_COMPONENT_NAMES[]` array (after "wizard_summary"):
```cpp
    "wizard_telemetry"              // 13 (preset mode only)
```

2. Update `STEP_COMPONENT_COUNT` from 12 to 13.

3. Add case 13 to `ui_wizard_load_screen()` switch (after case 12):
```cpp
    case 13: // Telemetry (preset mode only)
        spdlog::debug("[Wizard] Creating telemetry screen");
        get_wizard_telemetry_step()->init_subjects();
        get_wizard_telemetry_step()->register_callbacks();
        get_wizard_telemetry_step()->create(content);
        break;
```

4. Add case 13 to `ui_wizard_cleanup_current_screen()` switch:
```cpp
    case 13: // Telemetry
        get_wizard_telemetry_step()->cleanup();
        break;
```

5. Add a `telemetry_step_skipped` flag (default `true`) — only set to `false` in preset mode:
```cpp
static bool telemetry_step_skipped = true;  // Skip by default, enabled in preset mode
```

In the preset_mode block of `ui_wizard_precalculate_skips()`:
```cpp
    if (preset_mode) {
        // ... skip hardware steps ...
        telemetry_step_skipped = false;  // ENABLE telemetry step in preset mode
    }
```

6. Add the telemetry flag to `WizardSkipFlags` struct and `get_current_skip_flags()`:

In `wizard_step_logic.h`:
```cpp
    bool telemetry = false;  // NEW — step 13
```

In `wizard_step_logic.cpp` `is_step_skipped()`:
```cpp
    case 13: return skips.telemetry;
```

Update `TOTAL_STEPS` from 13 to 14.

In `get_current_skip_flags()`:
```cpp
    .telemetry = telemetry_step_skipped,
```

7. Include the header:
```cpp
#include "ui_wizard_telemetry.h"
```

**Step 6: Add Makefile entry**

Check `Makefile` or `mk/*.mk` for where source files are listed. The project likely auto-discovers `src/ui/*.cpp`, but verify. If there's an explicit list, add `src/ui/ui_wizard_telemetry.cpp`.

**Step 7: Commit**

```
git add include/ui_wizard_telemetry.h src/ui/ui_wizard_telemetry.cpp ui_xml/wizard_telemetry.xml
git add src/xml_registration.cpp src/ui/ui_wizard.cpp include/wizard_step_logic.h src/system/wizard_step_logic.cpp
git commit -m "feat(wizard): add dedicated telemetry step for preset mode"
```

---

### Task 7: Connection step auto-validation in preset mode

**Files:**
- Modify: `src/ui/ui_wizard.cpp` (on_next_clicked, around step 3 handling)

**Step 1: Implement connection auto-skip**

In preset mode, when advancing to step 3, we want to try connecting to the preset's Moonraker before showing the connection UI. This happens in `on_next_clicked()`.

The tricky part: the connection step currently sets up a WebSocket and validates asynchronously. For auto-validation, we need a quick synchronous HTTP check.

In `on_next_clicked()`, after the WiFi skip check and before navigating:

```cpp
    // Preset mode: auto-validate connection before showing step 3
    if (preset_mode && next_step == 3) {
        Config* config = Config::get_instance();
        std::string host = config->get<std::string>("/printer/moonraker_host", "");
        int port = config->get<int>("/printer/moonraker_port", 7125);
        if (!host.empty()) {
            // Quick synchronous check — try HTTP GET to /server/info
            // Use the existing MoonrakerRestApi or a simple HTTP client
            spdlog::info("[Wizard] Preset mode: testing connection to {}:{}", host, port);
            // If the MoonrakerClient is already connected (it may have auto-connected
            // using the preset config), skip the connection step
            MoonrakerClient* client = get_moonraker_client();
            if (client && client->get_connection_state() == ConnectionState::CONNECTED) {
                spdlog::info("[Wizard] Preset mode: already connected, skipping connection step");
                // Trigger precalculate since we're skipping step 3's exit
                ui_wizard_precalculate_skips();
                // Jump past connection and all hardware steps
                next_step = helix::wizard_next_step(3, get_current_skip_flags());
                if (next_step == -1) {
                    // All remaining steps skipped — complete wizard
                    ui_wizard_complete();
                    return;
                }
                ui_wizard_navigate_to_step(next_step);
                return;
            }
        }
    }
```

**Important:** Check how `MoonrakerClient` is initialized relative to the wizard. Look at `application.cpp` — does it connect before the wizard starts? If the preset has `moonraker_host: 127.0.0.1`, the app may auto-connect during init. If so, checking `get_connection_state() == CONNECTED` is sufficient.

If the client isn't connected yet at wizard time, we may need to show the connection step regardless and let the user click "Test Connection" as normal. The skip only works if auto-connection succeeded.

**Step 2: Commit**

```
git add src/ui/ui_wizard.cpp
git commit -m "feat(wizard): auto-skip connection step in preset mode when already connected"
```

---

### Task 8: Build and manual test

**Step 1: Build**

```bash
make -j
```

**Step 2: Test normal wizard (no preset)**

```bash
# Use test config without preset field
./build/bin/helix-screen --test --wizard -vv
```

Verify: All 13 steps show as before. No regressions.

**Step 3: Test preset mode**

Create a test config with preset field:
```bash
# Copy ad5m preset as test config
cp config/presets/ad5m.json config/helixconfig-test.json
# Edit to add "preset": "ad5m" and "wizard_completed": false
```

```bash
./build/bin/helix-screen --test --wizard -vv
```

Verify:
- Touch calibration skipped (ad5m has valid calibration)
- Language step shows
- Connection step may show or skip (depends on mock Moonraker)
- Hardware steps (4-11) all skipped
- Telemetry step shows
- Summary skipped
- Clicking "Finish" on telemetry completes wizard

**Step 4: Commit any fixes**

```
git commit -m "fix(wizard): address issues found in preset mode testing"
```

---

### Task 9: Update tests for wizard step logic with telemetry

**Files:**
- Modify: tests for wizard_step_logic (find existing)

**Step 1: Add tests for the full 14-step flow**

```cpp
TEST_CASE("Preset mode: full flow with telemetry", "[wizard][step_logic][preset]") {
    helix::WizardSkipFlags flags{};
    // Preset mode flags
    flags.wifi = true;
    flags.printer_identify = true;
    flags.heater_select = true;
    flags.fan_select = true;
    flags.ams = true;
    flags.led = true;
    flags.filament = true;
    flags.probe = true;
    flags.input_shaper = true;
    flags.summary = true;
    // telemetry = false (shown in preset mode)

    // Steps shown: 0(touch), 1(lang), 3(conn), 13(telemetry) = 4
    REQUIRE(helix::wizard_calculate_display_total(flags) == 4);

    // Navigation: 0 → 1 → 3 → 13
    REQUIRE(helix::wizard_next_step(0, flags) == 1);
    REQUIRE(helix::wizard_next_step(1, flags) == 3);
    REQUIRE(helix::wizard_next_step(3, flags) == 13);
    REQUIRE(helix::wizard_next_step(13, flags) == -1);  // End
}

TEST_CASE("Normal mode: telemetry skipped", "[wizard][step_logic]") {
    helix::WizardSkipFlags flags{};
    flags.telemetry = true;  // Not in preset mode

    // Steps: 0-12 shown (minus telemetry) = 13
    REQUIRE(helix::wizard_calculate_display_total(flags) == 13);
    REQUIRE(helix::wizard_next_step(12, flags) == -1);  // Summary is last
}
```

**Step 2: Run all wizard tests**

```bash
./build/bin/helix-tests "[wizard]" -v
```

**Step 3: Commit**

```
git add tests/unit/...
git commit -m "test(wizard): add preset mode and telemetry step tests"
```

---

### Summary of all tasks

| # | Task | Files | Type |
|---|------|-------|------|
| 1 | Config::has_preset() / get_preset() | config.h, config.cpp, test | Feature + Test |
| 2 | WizardSkipFlags new fields | wizard_step_logic.h/.cpp, test | Feature + Test |
| 3 | preset_mode flag in ui_wizard.cpp | ui_wizard.cpp | Feature |
| 4 | Update preset JSON files | config/presets/*.json | Config |
| 5 | Convention-based package.sh | scripts/package.sh | Refactor |
| 6 | Wizard telemetry step | New step class + XML + registration | Feature |
| 7 | Connection auto-validation | ui_wizard.cpp | Feature |
| 8 | Build and manual test | - | Testing |
| 9 | Wizard step logic tests | test files | Test |

**Dependencies:** Task 1 before 3. Task 2 before 3 and 6. Task 3 before 7. Tasks 4, 5 are independent. Task 6 depends on 2. Tasks 8, 9 are final.
