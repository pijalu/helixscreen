# Output Pin Fan Support + fan_feedback Integration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `output_pin fan*` objects visible, controllable, and renameable in the fan overlay and settings, with RPM data from `fan_feedback`.

**Architecture:** Extend the existing fan pipeline (discovery → state → control → UI) to handle `output_pin` fan objects. Add macro analysis for role detection. Add `fan_feedback` RPM integration. Add fan rename support via settings page and overlay long-press.

**Tech Stack:** C++17, LVGL 9.5 XML, Catch2 tests, Moonraker API (JSON-RPC over WebSocket)

**Spec:** `docs/superpowers/specs/2026-03-30-output-pin-fan-support-design.md`

---

## File Structure

| File | Responsibility |
|---|---|
| `include/macro_fan_analyzer.h` | NEW — Parse M106/M107/M141 macros for fan index and role hints |
| `src/printer/macro_fan_analyzer.cpp` | NEW — Implementation |
| `tests/unit/test_macro_fan_analyzer.cpp` | NEW — Macro parsing tests |
| `include/printer_fan_state.h` | MODIFY — Add `OUTPUT_PIN_FAN` type, `rpm` field, `update_fan_rpm()` |
| `src/printer/printer_fan_state.cpp` | MODIFY — Handle output_pin status, fan_feedback, classify output_pin |
| `include/printer_discovery.h` | MODIFY — Recognize `output_pin fan*` as fans, detect `fan_feedback` |
| `src/api/moonraker_api_controls.cpp` | MODIFY — `M106 P<index>` path for output_pin fans |
| `src/api/moonraker_discovery_sequence.cpp` | MODIFY — Subscribe to `fan_feedback` |
| `src/ui/ui_fan_control_overlay.cpp` | MODIFY — RPM display, long-press rename |
| `include/ui_settings_fans.h` | NEW — Fan settings overlay |
| `src/ui/ui_settings_fans.cpp` | NEW — Fan settings page with editable names |
| `ui_xml/fan_settings_overlay.xml` | NEW — Settings layout |
| `ui_xml/fan_settings_row.xml` | NEW — Per-fan row component |
| `src/ui/ui_panel_settings.cpp` | MODIFY — Add Fans entry |
| `main.cpp` | MODIFY — Register new XML components |
| `tests/unit/test_printer_fan_char.cpp` | MODIFY — Add output_pin + fan_feedback tests |

---

### Task 1: Add `OUTPUT_PIN_FAN` type and `rpm` field to FanInfo

**Files:**
- Modify: `include/printer_fan_state.h:23-29` (FanType enum), `include/printer_fan_state.h:54-60` (FanInfo struct)
- Test: `tests/unit/test_printer_fan_char.cpp`

- [ ] **Step 1: Write failing tests for output_pin fan type classification and rpm field**

Add to `tests/unit/test_printer_fan_char.cpp`:

```cpp
TEST_CASE("Fan characterization: output_pin fan type classification",
          "[characterization][fan][type][output_pin]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.init_fans({"output_pin fan0", "output_pin fan1", "output_pin fan2",
                     "heater_fan hotend_fan"});

    const auto& fans = state.get_fans();

    SECTION("output_pin fan0 is OUTPUT_PIN_FAN type") {
        REQUIRE(fans[0].type == FanType::OUTPUT_PIN_FAN);
    }

    SECTION("output_pin fan1 is OUTPUT_PIN_FAN type") {
        REQUIRE(fans[1].type == FanType::OUTPUT_PIN_FAN);
    }

    SECTION("output_pin fan2 is OUTPUT_PIN_FAN type") {
        REQUIRE(fans[2].type == FanType::OUTPUT_PIN_FAN);
    }

    SECTION("OUTPUT_PIN_FAN is controllable") {
        REQUIRE(fans[0].is_controllable == true);
    }

    SECTION("heater_fan is still HEATER_FAN") {
        REQUIRE(fans[3].type == FanType::HEATER_FAN);
    }
}

TEST_CASE("Fan characterization: FanInfo rpm field", "[characterization][fan][rpm]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.init_fans({"output_pin fan0"});

    const auto& fans = state.get_fans();

    SECTION("rpm is nullopt by default") {
        REQUIRE_FALSE(fans[0].rpm.has_value());
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[output_pin]" -v`
Expected: Compilation error — `FanType::OUTPUT_PIN_FAN` doesn't exist, `FanInfo::rpm` doesn't exist.

- [ ] **Step 3: Add OUTPUT_PIN_FAN to FanType enum and rpm to FanInfo**

In `include/printer_fan_state.h`, modify the FanType enum (line 23-29):

```cpp
enum class FanType {
    PART_COOLING,    ///< Main part cooling fan ("fan" or configured part fan)
    HEATER_FAN,      ///< Hotend cooling fan (auto-controlled, not user-adjustable)
    CONTROLLER_FAN,  ///< Electronics cooling (auto-controlled)
    TEMPERATURE_FAN, ///< Thermostatically controlled fan (auto-controlled)
    GENERIC_FAN,     ///< User-controllable generic fan (fan_generic)
    OUTPUT_PIN_FAN   ///< Creality-style output_pin fan (controllable via M106 P<index>)
};
```

In `include/printer_fan_state.h`, modify the FanInfo struct (line 54-60):

```cpp
struct FanInfo {
    std::string object_name;
    std::string display_name;
    FanType type = FanType::GENERIC_FAN;
    int speed_percent = 0;
    bool is_controllable = false;
    std::optional<int> rpm; ///< RPM from fan_feedback or Klipper rpm field
};
```

Add `#include <optional>` to the includes if not already present.

- [ ] **Step 4: Update classify_fan_type() and is_fan_controllable()**

In `src/printer/printer_fan_state.cpp`, modify `classify_fan_type()` (line 115-131). Add before the final `else` block:

```cpp
    } else if (object_name.rfind("output_pin ", 0) == 0) {
        // Check if the short name starts with "fan" (e.g., "output_pin fan0")
        std::string short_name = object_name.substr(11);
        if (short_name.rfind("fan", 0) == 0) {
            return FanType::OUTPUT_PIN_FAN;
        }
        return FanType::GENERIC_FAN;
```

Modify `is_fan_controllable()` (line 142-144):

```cpp
bool PrinterFanState::is_fan_controllable(FanType type) {
    return type == FanType::PART_COOLING || type == FanType::GENERIC_FAN ||
           type == FanType::OUTPUT_PIN_FAN;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[output_pin]" "[rpm]" -v`
Expected: All new tests PASS. Existing fan tests still pass.

- [ ] **Step 6: Commit**

```bash
git add include/printer_fan_state.h src/printer/printer_fan_state.cpp tests/unit/test_printer_fan_char.cpp
git commit -m "feat(fans): add OUTPUT_PIN_FAN type and rpm field to FanInfo"
```

---

### Task 2: Handle output_pin status updates in update_from_status()

**Files:**
- Modify: `src/printer/printer_fan_state.cpp:73-113` (update_from_status)
- Test: `tests/unit/test_printer_fan_char.cpp`

- [ ] **Step 1: Write failing tests for output_pin value parsing**

Add to `tests/unit/test_printer_fan_char.cpp`:

```cpp
TEST_CASE("Fan characterization: output_pin fan speed from value field",
          "[characterization][fan][update][output_pin]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.init_fans({"output_pin fan0", "output_pin fan1", "heater_fan hotend_fan"});

    SECTION("output_pin value 1.0 -> 100%") {
        json status = {{"output_pin fan0", {{"value", 1.0}}}};
        state.update_from_status(status);

        const auto& fans = state.get_fans();
        REQUIRE(fans[0].speed_percent == 100);
    }

    SECTION("output_pin value 0.5 -> 50%") {
        json status = {{"output_pin fan0", {{"value", 0.5}}}};
        state.update_from_status(status);

        const auto& fans = state.get_fans();
        REQUIRE(fans[0].speed_percent == 50);
    }

    SECTION("output_pin value 0.0 -> 0%") {
        json status = {{"output_pin fan0", {{"value", 0.0}}}};
        state.update_from_status(status);

        const auto& fans = state.get_fans();
        REQUIRE(fans[0].speed_percent == 0);
    }

    SECTION("multiple output_pin updates in one status") {
        json status = {{"output_pin fan0", {{"value", 0.75}}},
                       {"output_pin fan1", {{"value", 0.25}}}};
        state.update_from_status(status);

        const auto& fans = state.get_fans();
        REQUIRE(fans[0].speed_percent == 75);
        REQUIRE(fans[1].speed_percent == 25);
    }

    SECTION("output_pin update does not affect heater_fan") {
        json status = {{"output_pin fan0", {{"value", 1.0}}},
                       {"heater_fan hotend_fan", {{"speed", 0.5}}}};
        state.update_from_status(status);

        const auto& fans = state.get_fans();
        REQUIRE(fans[0].speed_percent == 100); // output_pin fan0
        REQUIRE(fans[2].speed_percent == 50);  // heater_fan hotend_fan
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[update][output_pin]" -v`
Expected: FAIL — output_pin fan0 speed_percent remains 0.

- [ ] **Step 3: Add output_pin handling to update_from_status()**

In `src/printer/printer_fan_state.cpp`, inside the `update_from_status()` loop (after the existing `heater_fan`/`fan_generic`/`controller_fan`/`temperature_fan` check at line 96-112), add:

```cpp
        // Handle output_pin fan objects (Creality-style)
        // These report {"value": 0.0-1.0} instead of {"speed": 0.0-1.0}
        if (key.rfind("output_pin ", 0) == 0) {
            if (value.is_object() && value.contains("value") && value["value"].is_number()) {
                double speed = value["value"].get<double>();
                update_fan_speed(key, speed);

                // If this is the configured part fan, also update the main fan_speed_ subject
                if (!roles_.part_fan.empty() && key == roles_.part_fan) {
                    int speed_pct = units::to_percent(speed);
                    if (lv_subject_get_int(&fan_speed_) != speed_pct) {
                        lv_subject_set_int(&fan_speed_, speed_pct);
                    }
                }
            }
        }
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[update][output_pin]" -v`
Expected: All PASS.

- [ ] **Step 5: Run full fan test suite for regressions**

Run: `./build/bin/helix-tests "[fan]" -v`
Expected: All existing tests still PASS.

- [ ] **Step 6: Commit**

```bash
git add src/printer/printer_fan_state.cpp tests/unit/test_printer_fan_char.cpp
git commit -m "feat(fans): handle output_pin value format in status updates"
```

---

### Task 3: Integrate fan_feedback RPM data

**Files:**
- Modify: `include/printer_fan_state.h` (add `update_fan_rpm()`)
- Modify: `src/printer/printer_fan_state.cpp` (implement `update_fan_rpm()`, parse fan_feedback in `update_from_status()`)
- Test: `tests/unit/test_printer_fan_char.cpp`

- [ ] **Step 1: Write failing tests for fan_feedback RPM parsing**

Add to `tests/unit/test_printer_fan_char.cpp`:

```cpp
TEST_CASE("Fan characterization: fan_feedback RPM updates",
          "[characterization][fan][update][fan_feedback]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    state.init_fans({"output_pin fan0", "output_pin fan1", "output_pin fan2"});

    SECTION("fan_feedback maps fanN_speed to output_pin fanN rpm") {
        json status = {{"fan_feedback", {{"fan0_speed", 16000},
                                         {"fan1_speed", 3692},
                                         {"fan2_speed", 0}}}};
        state.update_from_status(status);

        const auto& fans = state.get_fans();
        REQUIRE(fans[0].rpm.has_value());
        REQUIRE(fans[0].rpm.value() == 16000);
        REQUIRE(fans[1].rpm.has_value());
        REQUIRE(fans[1].rpm.value() == 3692);
        REQUIRE(fans[2].rpm.has_value());
        REQUIRE(fans[2].rpm.value() == 0);
    }

    SECTION("fan_feedback for unknown fanN is ignored") {
        json status = {{"fan_feedback", {{"fan5_speed", 1000}}}};
        state.update_from_status(status);

        // No crash, no effect on known fans
        const auto& fans = state.get_fans();
        REQUIRE_FALSE(fans[0].rpm.has_value());
    }

    SECTION("fan_feedback with non-numeric value is ignored") {
        json status = {{"fan_feedback", {{"fan0_speed", nullptr}}}};
        state.update_from_status(status);

        const auto& fans = state.get_fans();
        REQUIRE_FALSE(fans[0].rpm.has_value());
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[fan_feedback]" -v`
Expected: FAIL — rpm remains nullopt because fan_feedback is not parsed.

- [ ] **Step 3: Add update_fan_rpm() method**

In `include/printer_fan_state.h`, add to the public section (after `update_fan_speed` at line 107):

```cpp
    /// Update RPM reading for a fan (from fan_feedback or Klipper rpm field)
    void update_fan_rpm(const std::string& object_name, int rpm);
```

In `src/printer/printer_fan_state.cpp`, add implementation after `update_fan_speed()`:

```cpp
void PrinterFanState::update_fan_rpm(const std::string& object_name, int rpm) {
    for (auto& fan : fans_) {
        if (fan.object_name == object_name) {
            fan.rpm = rpm;
            return;
        }
    }
}
```

- [ ] **Step 4: Add fan_feedback parsing to update_from_status()**

In `src/printer/printer_fan_state.cpp`, add at the end of `update_from_status()` (after the loop):

```cpp
    // Parse fan_feedback RPM data (Creality-specific tachometer module)
    if (status.contains("fan_feedback")) {
        const auto& fb = status["fan_feedback"];
        if (fb.is_object()) {
            for (int i = 0; i < 10; i++) {
                std::string key = "fan" + std::to_string(i) + "_speed";
                if (fb.contains(key) && fb[key].is_number()) {
                    int rpm = fb[key].get<int>();
                    update_fan_rpm("output_pin fan" + std::to_string(i), rpm);
                }
            }
        }
    }
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[fan_feedback]" -v`
Expected: All PASS.

- [ ] **Step 6: Commit**

```bash
git add include/printer_fan_state.h src/printer/printer_fan_state.cpp tests/unit/test_printer_fan_char.cpp
git commit -m "feat(fans): integrate fan_feedback RPM data into FanInfo"
```

---

### Task 4: Add output_pin fan* discovery to printer_discovery.h

**Files:**
- Modify: `include/printer_discovery.h:155-171` (output_pin classification)

- [ ] **Step 1: Add output_pin fan classification to printer_discovery.h**

In `include/printer_discovery.h`, modify the `output_pin` block (lines 155-171). The block currently only classifies output_pins as LEDs or speakers. Add fan detection before the LED check:

```cpp
            // Output pins - classify as fan, LED, or speaker based on name
            else if (name.rfind("output_pin ", 0) == 0) {
                std::string pin_name = name.substr(11); // Remove "output_pin " prefix
                std::string upper_pin = to_upper(pin_name);

                // Fan detection: name starts with "FAN" (e.g., fan0, fan1, fan2)
                if (upper_pin.rfind("FAN", 0) == 0) {
                    fans_.push_back(name);
                }
                // LED detection
                else if (upper_pin.find("LIGHT") != std::string::npos ||
                    upper_pin.find("LED") != std::string::npos ||
                    upper_pin.find("LAMP") != std::string::npos) {
                    leds_.push_back(name);
                    has_led_ = true;
                }
                // Speaker/buzzer detection for M300 support
                if (upper_pin.find("BEEPER") != std::string::npos ||
                    upper_pin.find("BUZZER") != std::string::npos ||
                    upper_pin.find("SPEAKER") != std::string::npos) {
                    has_speaker_ = true;
                }
            }
```

Note: Speaker detection uses `if` (not `else if`) because a pin could theoretically match both categories. Fan and LED use `else if` since they're mutually exclusive.

- [ ] **Step 2: Add fan_feedback detection**

In `include/printer_discovery.h`, add a new capability flag. Find where `has_speaker_` is declared (around line 1085) and add:

```cpp
    bool has_fan_feedback_ = false;
```

Add accessor near the other `has_*()` methods:

```cpp
    [[nodiscard]] bool has_fan_feedback() const { return has_fan_feedback_; }
```

In the classification loop, add detection for `fan_feedback` (before the `output_pin` block):

```cpp
            else if (name == "fan_feedback") {
                has_fan_feedback_ = true;
            }
```

And in `clear()` (around line 529), add:

```cpp
        has_fan_feedback_ = false;
```

- [ ] **Step 3: Build to verify compilation**

Run: `make -j`
Expected: Clean build with no errors.

- [ ] **Step 4: Commit**

```bash
git add include/printer_discovery.h
git commit -m "feat(fans): recognize output_pin fan* and fan_feedback in discovery"
```

---

### Task 5: Subscribe to fan_feedback and output_pin fan objects

**Files:**
- Modify: `src/api/moonraker_discovery_sequence.cpp` (subscription list)

- [ ] **Step 1: Verify current subscription code**

Read `src/api/moonraker_discovery_sequence.cpp` lines 655-690 to confirm the subscription loop structure. The existing code subscribes to all entries in `fans_` at lines 678-682. Since discovery now puts `output_pin fan*` into `fans_`, they'll be subscribed automatically.

- [ ] **Step 2: Add fan_feedback to subscription list**

In `src/api/moonraker_discovery_sequence.cpp`, in the `complete_discovery_subscription()` function, after the fan subscription loop (around line 682), add:

```cpp
    // Subscribe to fan_feedback if available (Creality tachometer module)
    if (discovery_.has_fan_feedback()) {
        subscription_objects["fan_feedback"] = nullptr;
        spdlog::debug("[MoonrakerDiscoverySequence] Subscribing to fan_feedback for RPM data");
    }
```

- [ ] **Step 3: Build to verify compilation**

Run: `make -j`
Expected: Clean build.

- [ ] **Step 4: Commit**

```bash
git add src/api/moonraker_discovery_sequence.cpp
git commit -m "feat(fans): subscribe to fan_feedback for RPM data"
```

---

### Task 6: Add M106 P<index> control path for output_pin fans

**Files:**
- Modify: `src/api/moonraker_api_controls.cpp:109-125` (set_fan_speed gcode generation)
- Test: `tests/unit/test_printer_fan_char.cpp` (or a new test file if there are existing control tests)

- [ ] **Step 1: Write failing test for M106 P<index> gcode generation**

We need to test that `set_fan_speed()` generates the right gcode. Since `MoonrakerAPI::set_fan_speed()` calls `execute_gcode()` internally and we can't easily intercept that in unit tests, we'll extract the gcode generation logic into a testable free function.

Add a new test file `tests/unit/test_fan_control.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../catch_amalgamated.hpp"
#include "fan_gcode.h"

TEST_CASE("Fan gcode generation", "[fan][gcode]") {
    SECTION("bare fan uses M106 S<value>") {
        auto gcode = helix::fan_gcode("fan", 100.0);
        REQUIRE(gcode == "M106 S255");
    }

    SECTION("bare fan at 50% uses M106 S128") {
        auto gcode = helix::fan_gcode("fan", 50.0);
        REQUIRE(gcode == "M106 S128");
    }

    SECTION("bare fan off uses M107") {
        auto gcode = helix::fan_gcode("fan", 0.0);
        REQUIRE(gcode == "M107");
    }

    SECTION("output_pin fan0 uses M106 P0") {
        auto gcode = helix::fan_gcode("output_pin fan0", 100.0);
        REQUIRE(gcode == "M106 P0 S255");
    }

    SECTION("output_pin fan2 at 50% uses M106 P2 S128") {
        auto gcode = helix::fan_gcode("output_pin fan2", 50.0);
        REQUIRE(gcode == "M106 P2 S128");
    }

    SECTION("output_pin fan0 off uses M107 P0") {
        auto gcode = helix::fan_gcode("output_pin fan0", 0.0);
        REQUIRE(gcode == "M107 P0");
    }

    SECTION("output_pin non-fan uses SET_PIN") {
        auto gcode = helix::fan_gcode("output_pin aux_blower", 75.0);
        REQUIRE(gcode == "SET_PIN PIN=aux_blower VALUE=0.75");
    }

    SECTION("fan_generic uses SET_FAN_SPEED") {
        auto gcode = helix::fan_gcode("fan_generic aux_fan", 50.0);
        REQUIRE(gcode == "SET_FAN_SPEED FAN=aux_fan SPEED=0.50");
    }

    SECTION("heater_fan uses SET_FAN_SPEED") {
        auto gcode = helix::fan_gcode("heater_fan hotend_fan", 100.0);
        REQUIRE(gcode == "SET_FAN_SPEED FAN=hotend_fan SPEED=1.00");
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[gcode]" -v`
Expected: Compilation error — `fan_gcode.h` doesn't exist.

- [ ] **Step 3: Create fan_gcode utility**

Create `include/fan_gcode.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <sstream>
#include <string>

namespace helix {

/// Generate the gcode command to set a fan's speed.
/// @param fan Full Moonraker object name (e.g., "fan", "output_pin fan0", "fan_generic aux")
/// @param speed_percent 0.0-100.0
/// @return Gcode string ready for execute_gcode()
inline std::string fan_gcode(const std::string& fan, double speed_percent) {
    int s_value = static_cast<int>(std::round(speed_percent * 255.0 / 100.0));

    // Canonical Klipper [fan] section
    if (fan == "fan") {
        if (s_value == 0) {
            return "M107";
        }
        return "M106 S" + std::to_string(s_value);
    }

    // output_pin fan objects (Creality-style)
    if (fan.rfind("output_pin ", 0) == 0) {
        std::string short_name = fan.substr(11);

        // output_pin fanN -> M106 P<N> S<value>
        if (short_name.rfind("fan", 0) == 0 && short_name.size() > 3) {
            std::string index_str = short_name.substr(3);
            // Verify it's a number
            bool is_numeric = !index_str.empty();
            for (char c : index_str) {
                if (!std::isdigit(c)) {
                    is_numeric = false;
                    break;
                }
            }
            if (is_numeric) {
                if (s_value == 0) {
                    return "M107 P" + index_str;
                }
                return "M106 P" + index_str + " S" + std::to_string(s_value);
            }
        }

        // Non-numeric output_pin fan or non-fan output_pin: SET_PIN
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2)
           << "SET_PIN PIN=" << short_name << " VALUE=" << (speed_percent / 100.0);
        return ss.str();
    }

    // All other fan types: SET_FAN_SPEED
    std::string fan_name = fan;
    size_t space_pos = fan_name.find(' ');
    if (space_pos != std::string::npos) {
        fan_name = fan_name.substr(space_pos + 1);
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2)
       << "SET_FAN_SPEED FAN=" << fan_name << " SPEED=" << (speed_percent / 100.0);
    return ss.str();
}

} // namespace helix
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[gcode]" -v`
Expected: All PASS.

- [ ] **Step 5: Wire fan_gcode() into MoonrakerAPI::set_fan_speed()**

In `src/api/moonraker_api_controls.cpp`, replace the gcode generation block (lines 109-125) with a call to `fan_gcode()`:

```cpp
    #include "fan_gcode.h"

    // ... (validation code stays the same, lines 73-107) ...

    std::string gcode = helix::fan_gcode(fan, speed);
    spdlog::debug("[MoonrakerAPI] set_fan_speed('{}', {:.0f}%) -> {}", fan, speed, gcode);
```

Remove the old `std::ostringstream gcode;` block and the manual gcode construction.

- [ ] **Step 6: Build to verify compilation**

Run: `make -j`
Expected: Clean build.

- [ ] **Step 7: Commit**

```bash
git add include/fan_gcode.h src/api/moonraker_api_controls.cpp tests/unit/test_fan_control.cpp
git commit -m "feat(fans): add M106 P<index> control for output_pin fans"
```

---

### Task 7: MacroFanAnalyzer — Parse M106/M107/M141 macros

**Files:**
- Create: `include/macro_fan_analyzer.h`
- Create: `src/printer/macro_fan_analyzer.cpp`
- Test: `tests/unit/test_macro_fan_analyzer.cpp`

- [ ] **Step 1: Write failing tests**

Create `tests/unit/test_macro_fan_analyzer.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../catch_amalgamated.hpp"
#include "macro_fan_analyzer.h"

using namespace helix;
using json = nlohmann::json;

// Real K1C M106 macro (simplified)
static const char* K1C_M106_MACRO = R"(
{% set fans = printer["gcode_macro PRINTER_PARAM"].fans|int %}
{% set fan = 0 %}
{% set value = 0 %}
{% if params.P is defined %}
{% set tmp = params.P|int %}
{% if tmp < fans %}
{% set fan = tmp %}
{% endif %}
{% endif %}
{% if params.S is defined %}
{% set tmp = params.S|float %}
{% else %}
{% set tmp = 255 %}
{% endif %}
{% if tmp > 0 %}
{% if fan == 0 %}
{% set value = (255 - printer["gcode_macro PRINTER_PARAM"].fan0_min) / 255 * tmp %}
SET_PIN PIN=fan0 VALUE={value|int}
{% elif fan == 2 %}
{% set value = (255 - printer["gcode_macro PRINTER_PARAM"].fan2_min) / 255 * tmp %}
SET_PIN PIN=fan2 VALUE={value|int}
{% endif %}
{% endif %}
)";

static const char* K1C_M107_MACRO = R"(
{% set fans = printer["gcode_macro PRINTER_PARAM"].fans|int %}
{% if params.P is defined %}
{% if params.P|int < fans %}
SET_PIN PIN=fan{params.P|int} VALUE=0
{% else %}
SET_PIN PIN=fan0 VALUE=0
{% endif %}
{% else %}
SET_PIN PIN=fan0 VALUE=0
SET_PIN PIN=fan2 VALUE=0
{% endif %}
)";

static const char* K1C_M141_MACRO = R"(
{% if 'S' in params|upper %}
{% if printer["temperature_fan chamber_fan"].speed > 0.0 %}
SET_PIN PIN=fan1 VALUE=255
{% else %}
SET_PIN PIN=fan1 VALUE=0
{% endif %}
{% if params.S|int > 0 %}
SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=chamber_fan TARGET={params.S|default(35)}
{% else %}
SET_TEMPERATURE_FAN_TARGET TEMPERATURE_FAN=chamber_fan TARGET=35
{% endif %}
{% endif %}
)";

TEST_CASE("MacroFanAnalyzer: extracts fan indices from M106 macro",
          "[macro_fan_analyzer]") {
    json config;
    config["gcode_macro m106"]["gcode"] = K1C_M106_MACRO;
    config["gcode_macro m107"]["gcode"] = K1C_M107_MACRO;

    MacroFanAnalyzer analyzer;
    auto result = analyzer.analyze(config);

    SECTION("detects fan0 from SET_PIN PIN=fan0") {
        REQUIRE(result.fan_indices.count("output_pin fan0") == 1);
        REQUIRE(result.fan_indices["output_pin fan0"] == 0);
    }

    SECTION("detects fan2 from SET_PIN PIN=fan2") {
        REQUIRE(result.fan_indices.count("output_pin fan2") == 1);
        REQUIRE(result.fan_indices["output_pin fan2"] == 2);
    }
}

TEST_CASE("MacroFanAnalyzer: extracts role hints from M141 macro",
          "[macro_fan_analyzer]") {
    json config;
    config["gcode_macro m141"]["gcode"] = K1C_M141_MACRO;

    MacroFanAnalyzer analyzer;
    auto result = analyzer.analyze(config);

    SECTION("fan1 referenced in M141 gets chamber_circulation role hint") {
        REQUIRE(result.role_hints.count("output_pin fan1") == 1);
        REQUIRE(result.role_hints["output_pin fan1"] == "Chamber Circulation");
    }
}

TEST_CASE("MacroFanAnalyzer: handles missing macros gracefully",
          "[macro_fan_analyzer]") {
    json config; // empty

    MacroFanAnalyzer analyzer;
    auto result = analyzer.analyze(config);

    SECTION("no fan indices") {
        REQUIRE(result.fan_indices.empty());
    }

    SECTION("no role hints") {
        REQUIRE(result.role_hints.empty());
    }
}

TEST_CASE("MacroFanAnalyzer: handles malformed gcode gracefully",
          "[macro_fan_analyzer]") {
    json config;
    config["gcode_macro m106"]["gcode"] = "this is not real gcode";

    MacroFanAnalyzer analyzer;
    auto result = analyzer.analyze(config);

    SECTION("no fan indices from garbage") {
        REQUIRE(result.fan_indices.empty());
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[macro_fan_analyzer]" -v`
Expected: Compilation error — `macro_fan_analyzer.h` doesn't exist.

- [ ] **Step 3: Create MacroFanAnalyzer**

Create `include/macro_fan_analyzer.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "hv/json.hpp"

#include <string>
#include <unordered_map>

namespace helix {

/// Results from analyzing M106/M107/M141 macro gcode text
struct MacroFanAnalysis {
    /// Map of "output_pin fanN" -> M106 index N
    std::unordered_map<std::string, int> fan_indices;
    /// Map of "output_pin fanN" -> suggested display name/role
    std::unordered_map<std::string, std::string> role_hints;
};

/// Analyzes Klipper macro gcode text to extract fan index mappings and role hints.
/// Used to auto-detect output_pin fan roles on Creality printers.
class MacroFanAnalyzer {
  public:
    /// Analyze configfile.settings JSON for fan-related macros.
    /// Looks for gcode_macro m106, m107, m141 keys.
    MacroFanAnalysis analyze(const nlohmann::json& config_settings) const;

  private:
    /// Extract SET_PIN PIN=fanN patterns from gcode text
    void extract_set_pin_fans(const std::string& gcode, MacroFanAnalysis& result) const;
    /// Check M141 macro for chamber circulation fan references
    void extract_m141_roles(const std::string& gcode, MacroFanAnalysis& result) const;
};

} // namespace helix
```

Create `src/printer/macro_fan_analyzer.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_fan_analyzer.h"

#include <regex>
#include <spdlog/spdlog.h>

namespace helix {

MacroFanAnalysis MacroFanAnalyzer::analyze(const nlohmann::json& config_settings) const {
    MacroFanAnalysis result;

    // Parse M106 macro for SET_PIN PIN=fanN patterns
    if (config_settings.contains("gcode_macro m106")) {
        const auto& m106 = config_settings["gcode_macro m106"];
        if (m106.contains("gcode") && m106["gcode"].is_string()) {
            extract_set_pin_fans(m106["gcode"].get<std::string>(), result);
        }
    }

    // Parse M107 macro for additional SET_PIN PIN=fanN patterns
    if (config_settings.contains("gcode_macro m107")) {
        const auto& m107 = config_settings["gcode_macro m107"];
        if (m107.contains("gcode") && m107["gcode"].is_string()) {
            extract_set_pin_fans(m107["gcode"].get<std::string>(), result);
        }
    }

    // Parse M141 macro for chamber circulation fan hints
    if (config_settings.contains("gcode_macro m141")) {
        const auto& m141 = config_settings["gcode_macro m141"];
        if (m141.contains("gcode") && m141["gcode"].is_string()) {
            extract_m141_roles(m141["gcode"].get<std::string>(), result);
        }
    }

    if (!result.fan_indices.empty()) {
        spdlog::info("[MacroFanAnalyzer] Detected {} output_pin fans from macros",
                     result.fan_indices.size());
        for (const auto& [name, index] : result.fan_indices) {
            spdlog::debug("[MacroFanAnalyzer]   {} -> M106 P{}", name, index);
        }
    }
    if (!result.role_hints.empty()) {
        for (const auto& [name, role] : result.role_hints) {
            spdlog::debug("[MacroFanAnalyzer]   {} -> role hint: {}", name, role);
        }
    }

    return result;
}

void MacroFanAnalyzer::extract_set_pin_fans(const std::string& gcode,
                                            MacroFanAnalysis& result) const {
    // Match SET_PIN PIN=fanN where N is one or more digits
    // Handles both literal "SET_PIN PIN=fan0" and Jinja "SET_PIN PIN=fan{params.P|int}"
    std::regex pattern(R"(SET_PIN\s+PIN=fan(\d+))");
    auto begin = std::sregex_iterator(gcode.begin(), gcode.end(), pattern);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        int index = std::stoi((*it)[1].str());
        std::string obj_name = "output_pin fan" + std::to_string(index);
        result.fan_indices[obj_name] = index;
    }
}

void MacroFanAnalyzer::extract_m141_roles(const std::string& gcode,
                                          MacroFanAnalysis& result) const {
    // M141 is the chamber temperature command. Any SET_PIN PIN=fanN in M141
    // indicates that fanN is used for chamber circulation/ventilation.
    std::regex pattern(R"(SET_PIN\s+PIN=fan(\d+))");
    auto begin = std::sregex_iterator(gcode.begin(), gcode.end(), pattern);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        int index = std::stoi((*it)[1].str());
        std::string obj_name = "output_pin fan" + std::to_string(index);
        result.role_hints[obj_name] = "Chamber Circulation";
    }
}

} // namespace helix
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[macro_fan_analyzer]" -v`
Expected: All PASS.

- [ ] **Step 5: Commit**

```bash
git add include/macro_fan_analyzer.h src/printer/macro_fan_analyzer.cpp tests/unit/test_macro_fan_analyzer.cpp
git commit -m "feat(fans): add MacroFanAnalyzer for Creality fan role detection"
```

---

### Task 8: Wire macro analysis into discovery and fan name persistence

**Files:**
- Modify: `src/printer/printer_fan_state.cpp` (read custom names from settings, write defaults)
- Modify: `include/printer_fan_state.h` (add config pointer, custom name methods)
- Modify: `src/api/moonraker_discovery_sequence.cpp` (run MacroFanAnalyzer during discovery)
- Test: `tests/unit/test_printer_fan_char.cpp`

- [ ] **Step 1: Write failing test for custom name loading from config**

Add to `tests/unit/test_printer_fan_char.cpp`:

```cpp
TEST_CASE("Fan characterization: custom display names from config",
          "[characterization][fan][names]") {
    lv_init_safe();

    PrinterState& state = get_printer_state();
    PrinterStateTestAccess::reset(state);
    state.init_subjects(false);

    // Simulate saved custom names in config
    auto* config = Config::get_instance();
    config->set(config->df() + "fans/names/output_pin fan0", std::string("Part Fan"));
    config->set(config->df() + "fans/names/output_pin fan1", std::string("Electronics Fan"));

    state.init_fans({"output_pin fan0", "output_pin fan1", "output_pin fan2"});

    const auto& fans = state.get_fans();

    SECTION("fan with custom name uses it") {
        REQUIRE(fans[0].display_name == "Part Fan");
        REQUIRE(fans[1].display_name == "Electronics Fan");
    }

    SECTION("fan without custom name gets auto-generated name") {
        // fan2 has no custom name, should get default
        REQUIRE_FALSE(fans[2].display_name.empty());
    }
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[names]" -v`
Expected: FAIL — custom names not loaded.

- [ ] **Step 3: Add custom name loading to init_fans()**

In `src/printer/printer_fan_state.cpp`, modify the display name resolution in `init_fans()` (around lines 194-197). Replace:

```cpp
        // Use role-based display name if configured, otherwise auto-generate
        std::string role_name = get_role_display_name(obj_name);
        info.display_name =
            role_name.empty() ? get_display_name(obj_name, DeviceType::FAN) : role_name;
```

With:

```cpp
        // Name priority: custom name > role name > auto-generated
        auto* config = Config::get_instance();
        std::string custom_name;
        if (config) {
            custom_name = config->get<std::string>(
                config->df() + "fans/names/" + obj_name, "");
        }
        if (!custom_name.empty()) {
            info.display_name = custom_name;
        } else {
            std::string role_name = get_role_display_name(obj_name);
            info.display_name =
                role_name.empty() ? get_display_name(obj_name, DeviceType::FAN) : role_name;
        }
```

Add `#include "config.h"` if not already present.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[names]" -v`
Expected: All PASS.

- [ ] **Step 5: Wire MacroFanAnalyzer into discovery sequence**

In `src/api/moonraker_discovery_sequence.cpp`, after discovery completes and before `init_fans()` is called, run the macro analyzer. Find where configfile settings are available (the configfile query response handler). Add:

```cpp
#include "macro_fan_analyzer.h"

// After configfile settings are parsed:
helix::MacroFanAnalyzer analyzer;
auto macro_result = analyzer.analyze(config_settings);

// Write default names from macro analysis (only if no custom name exists)
auto* config = Config::get_instance();
if (config) {
    for (const auto& [obj_name, role] : macro_result.role_hints) {
        std::string key = config->df() + "fans/names/" + obj_name;
        if (config->get<std::string>(key, "").empty()) {
            config->set(key, role);
        }
    }
}
```

The exact insertion point depends on where configfile settings are processed — read the discovery sequence to find the right location.

- [ ] **Step 6: Build and run full test suite**

Run: `make test && ./build/bin/helix-tests "[fan]" -v`
Expected: All PASS.

- [ ] **Step 7: Commit**

```bash
git add include/printer_fan_state.h src/printer/printer_fan_state.cpp src/api/moonraker_discovery_sequence.cpp tests/unit/test_printer_fan_char.cpp
git commit -m "feat(fans): load custom display names from config, wire macro analysis"
```

---

### Task 9: Fan Settings Overlay — XML layout and C++ implementation

**Files:**
- Create: `ui_xml/fan_settings_overlay.xml`
- Create: `ui_xml/fan_settings_row.xml`
- Create: `include/ui_settings_fans.h`
- Create: `src/ui/ui_settings_fans.cpp`
- Modify: `src/ui/ui_panel_settings.cpp` (add Fans entry)
- Modify: `main.cpp` (register XML components)

This task is UI-heavy. Follow the `SensorSettingsOverlay` pattern closely. Read `include/ui_settings_sensors.h` and `src/ui/ui_settings_sensors.cpp` as reference before implementing.

- [ ] **Step 1: Create fan_settings_row.xml**

Create `ui_xml/fan_settings_row.xml`. Use semantic widgets per [L008]:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<component name="fan_settings_row">
    <api>
        <prop name="fan_name" type="string" default="Fan"/>
        <prop name="fan_type" type="string" default="fan"/>
        <prop name="fan_object" type="string" default=""/>
    </api>
    <consts>
        <const name="row_height" value="56"/>
    </consts>
    <lv_obj name="fan_row"
        style_width="content" style_min_width="100%"
        style_height="content" style_min_height="#row_height"
        style_pad_left="#space_md" style_pad_right="#space_md"
        style_pad_top="#space_sm" style_pad_bottom="#space_sm"
        style_bg_opa="0"
        style_flex_flow="row" style_flex_main_place="start"
        style_flex_cross_place="center" style_pad_column="#space_md">

        <lv_image name="fan_icon" src="fan"/>

        <lv_obj style_flex_flow="column" style_flex_grow="1"
            style_height="content" style_pad_gap="2"
            style_bg_opa="0" style_border_width="0">
            <text_body name="name_label" text="${fan_name}"/>
            <text_small name="object_label" text="${fan_object}"
                style_text_color="#text_secondary" style_opa="180"/>
        </lv_obj>

        <status_pill name="type_label" text="${fan_type}"/>
        <text_body name="speed_label" text="0%"
            style_min_width="48" style_text_align="right"/>
    </lv_obj>
    <divider_horizontal/>
</component>
```

- [ ] **Step 2: Create fan_settings_overlay.xml**

Create `ui_xml/fan_settings_overlay.xml`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<component name="fan_settings_overlay">
    <subjects>
        <subject name="controllable_fan_count" type="int" default="0"/>
        <subject name="auto_fan_count" type="int" default="0"/>
    </subjects>
    <ui_overlay name="fan_settings_overlay" title="Fans">
        <lv_obj name="fan_settings_content"
            style_width="100%" style_flex_grow="1"
            style_flex_flow="column"
            style_pad_all="0" style_pad_gap="0"
            style_bg_opa="0" style_border_width="0"
            styles="scrollable">

            <!-- Controllable Fans Section -->
            <lv_obj name="controllable_section"
                style_width="100%" style_height="content"
                style_flex_flow="column" style_pad_all="0" style_pad_gap="0"
                style_bg_opa="0" style_border_width="0">
                <bind_flag_if_eq subject="controllable_fan_count" flag="hidden" ref_value="0"/>
                <setting_section_header icon="fan" label="Controllable Fans"
                    badge_subject="controllable_fan_count"/>
                <lv_obj name="controllable_fans_list"
                    style_width="100%" style_height="content"
                    style_flex_flow="column" style_pad_all="0" style_pad_gap="0"
                    style_bg_opa="0" style_border_width="0"/>
            </lv_obj>

            <!-- Auto Fans Section -->
            <lv_obj name="auto_section"
                style_width="100%" style_height="content"
                style_flex_flow="column" style_pad_all="0" style_pad_gap="0"
                style_bg_opa="0" style_border_width="0">
                <bind_flag_if_eq subject="auto_fan_count" flag="hidden" ref_value="0"/>
                <setting_section_header icon="fan" label="Auto Fans"
                    badge_subject="auto_fan_count"/>
                <lv_obj name="auto_fans_list"
                    style_width="100%" style_height="content"
                    style_flex_flow="column" style_pad_all="0" style_pad_gap="0"
                    style_bg_opa="0" style_border_width="0"/>
            </lv_obj>

            <!-- Empty state -->
            <lv_obj name="no_fans_placeholder"
                style_width="100%" style_flex_grow="1"
                style_flex_flow="column"
                style_flex_main_place="center" style_flex_cross_place="center"
                style_bg_opa="0" style_border_width="0" flag="hidden">
                <lv_image src="fan" style_opa="80"/>
                <text_body text="No fans detected" style_text_color="#text_secondary"/>
            </lv_obj>
        </lv_obj>
    </ui_overlay>
</component>
```

- [ ] **Step 3: Create FanSettingsOverlay class**

Create `include/ui_settings_fans.h` following `include/ui_settings_sensors.h` pattern:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "overlay_base.h"
#include "printer_fan_state.h"

namespace helix::settings {

class FanSettingsOverlay : public OverlayBase {
  public:
    FanSettingsOverlay();

    void register_callbacks() override;
    void show(lv_obj_t* parent_screen);

  protected:
    void on_activate() override;

  private:
    void create(lv_obj_t* parent) override;
    void populate_fans();
    void populate_controllable_fans(lv_obj_t* list);
    void populate_auto_fans(lv_obj_t* list);
    void handle_fan_rename(const std::string& object_name, const std::string& current_name);

    lv_obj_t* controllable_list_ = nullptr;
    lv_obj_t* auto_list_ = nullptr;
    lv_obj_t* no_fans_placeholder_ = nullptr;
    bool created_ = false;
};

FanSettingsOverlay& get_fan_settings_overlay();

} // namespace helix::settings
```

Create `src/ui/ui_settings_fans.cpp` — implementation follows the `SensorSettingsOverlay` pattern: lazy-create XML, find containers by name, clear-and-repopulate on activate. Each row is created with `lv_xml_create()` using `fan_settings_row` component. Name label gets an `LV_EVENT_CLICKED` callback that opens a keyboard modal for rename. On rename confirm, save to `config->df() + "fans/names/" + object_name` and call `populate_fans()` to rebuild.

The implementation should read fan data from `get_printer_state().get_fans()`, count controllable vs auto fans, update section subjects, and hide the empty-state placeholder when fans exist.

- [ ] **Step 4: Register XML components in main.cpp**

In `main.cpp`, add after existing component registrations (applying [L014]):

```cpp
lv_xml_component_register_from_file("fan_settings_overlay");
lv_xml_component_register_from_file("fan_settings_row");
```

- [ ] **Step 5: Add Fans entry to Settings panel**

In `src/ui/ui_panel_settings.cpp`, add a callback entry alongside the sensors callback (line 333 area):

```cpp
{"on_fans_settings_clicked", on_fans_settings_clicked},
```

Add the static trampoline and handler following the sensor pattern:

```cpp
void SettingsPanel::on_fans_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_fans_settings_clicked");
    get_global_settings_panel().handle_fans_settings_clicked();
}

void SettingsPanel::handle_fans_settings_clicked() {
    spdlog::debug("[{}] Fans clicked - delegating to FanSettingsOverlay", get_name());
    auto& overlay = helix::settings::get_fan_settings_overlay();
    overlay.show(parent_screen_);
}
```

Register the overlay's callbacks in the settings panel initialization:

```cpp
helix::settings::get_fan_settings_overlay().register_callbacks();
```

- [ ] **Step 6: Add Fans row to settings panel XML**

Add a row in the settings panel XML (in the Printer section, near the sensors row). This will need an XML entry with `event_cb trigger="clicked" callback="on_fans_settings_clicked"`.

- [ ] **Step 7: Build and test manually**

Run: `make -j`
Expected: Clean build.

Then test interactively (applying [L060]):
```bash
./build/bin/helix-screen --test -vv -p settings 2>&1 | tee /tmp/fan-settings-test.log
```
Navigate to Settings > Fans and verify:
- Fan list populates correctly
- Sections show/hide based on fan types
- Tap name to rename works

- [ ] **Step 8: Commit**

```bash
git add ui_xml/fan_settings_overlay.xml ui_xml/fan_settings_row.xml include/ui_settings_fans.h src/ui/ui_settings_fans.cpp src/ui/ui_panel_settings.cpp main.cpp
git commit -m "feat(fans): add Settings > Fans page with editable display names"
```

---

### Task 10: Fan overlay — RPM display and long-press rename

**Files:**
- Modify: `src/ui/ui_fan_control_overlay.cpp` (RPM in status cards, long-press rename)

- [ ] **Step 1: Add RPM display to fan status cards**

In `src/ui/ui_fan_control_overlay.cpp`, in `populate_fans()` second pass (non-controllable fan cards), where the speed label is updated, also show RPM if available:

For the `update_auto_fan_cards()` method (or equivalent periodic update), format the speed string to include RPM:

```cpp
if (fan.rpm.has_value() && fan.rpm.value() > 0) {
    lv_label_set_text_fmt(card.speed_label, "%d%% · %d RPM",
                          fan.speed_percent, fan.rpm.value());
} else {
    lv_label_set_text_fmt(card.speed_label, "%d%%", fan.speed_percent);
}
```

Apply the same RPM display to controllable fan dials if they have RPM data.

- [ ] **Step 2: Add long-press rename to fan overlay**

For each fan name label in the overlay, add a long-press event handler:

```cpp
lv_obj_add_event_cb(name_label, [](lv_event_t* e) {
    auto* data = static_cast<FanRenameData*>(lv_event_get_user_data(e));
    // Open keyboard modal with current name, save on confirm
    // ... (use existing keyboard modal pattern from the codebase)
}, LV_EVENT_LONG_PRESSED, rename_data);
```

Note: This is an exception to the "no lv_obj_add_event_cb" rule — the fan list is dynamically populated in C++, similar to sensor rows.

On rename confirm:
1. Save to `config->df() + "fans/names/" + object_name`
2. Update `FanInfo.display_name`
3. Bump `fans_version_` to trigger rebuild

- [ ] **Step 3: Build and test manually**

Run: `make -j`
Test interactively: open the fan overlay, verify RPM shows for fans with fan_feedback data. Long-press a fan name to test rename flow.

- [ ] **Step 4: Commit**

```bash
git add src/ui/ui_fan_control_overlay.cpp
git commit -m "feat(fans): add RPM display and long-press rename to fan overlay"
```

---

### Task 11: Integration test with K1C

**Files:** None (manual testing)

- [ ] **Step 1: Deploy to test device connected to K1C**

Deploy to the Pi connected to the K1C at 192.168.30.182:

```bash
PI_HOST=192.168.1.113 make pi-test
```

- [ ] **Step 2: Verify fan discovery**

Check logs for:
- `output_pin fan0`, `output_pin fan1`, `output_pin fan2` discovered as fans
- `fan_feedback` detected
- MacroFanAnalyzer output (fan indices, role hints)

- [ ] **Step 3: Verify fan overlay**

Open the fan overlay and verify:
- All 5 fans visible (fan0, fan1, fan2, hotend_fan, chamber_fan)
- fan0/fan1/fan2 are controllable (have dials)
- hotend_fan and chamber_fan are auto (read-only cards)
- RPM data shows for output_pin fans
- Speed updates live when fans are running

- [ ] **Step 4: Verify fan control**

Adjust fan0 speed via the dial. Verify:
- Correct gcode sent (M106 P0 S<value>)
- Fan physically responds
- Speed reading updates

- [ ] **Step 5: Verify Settings > Fans**

Open Settings > Fans and verify:
- All fans listed with correct names
- Tap to rename works
- Renamed names persist after closing and reopening
- Names show in fan overlay after rename

- [ ] **Step 6: Commit any fixes**

```bash
git commit -m "fix(fans): integration fixes from K1C testing"
```
