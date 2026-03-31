# Snapmaker U1 Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a dedicated `AmsBackendSnapmaker` that enables toolchanger support, RFID filament detection, and tool switching for the Snapmaker U1 4-toolhead printer.

**Architecture:** New `AmsBackendSnapmaker` backend class following the existing AMS backend pattern (like CFS, AFC). Detection via `filament_detect` Klipper object. Maps U1's custom per-extruder fields (`state`, `park_pin`, `active_pin`) to ToolState, and RFID tag data to SlotInfo. Tool switching via `T{N}` gcode.

**Tech Stack:** C++17, Catch2 tests, nlohmann::json, LVGL subjects, Moonraker WebSocket API

**Spec:** `docs/devel/specs/2026-03-31-snapmaker-u1-integration-design.md`

---

## File Map

### New Files

| File | Responsibility |
|------|---------------|
| `include/ams_backend_snapmaker.h` | Backend header — class definition, Snapmaker-specific structs |
| `src/printer/ams_backend_snapmaker.cpp` | Backend implementation — status parsing, tool ops, RFID mapping |
| `tests/unit/test_ams_backend_snapmaker.cpp` | Unit tests — enum, detection, parsing, tool state, RFID |

### Modified Files

| File | Change |
|------|--------|
| `include/ams_types.h` | Add `SNAPMAKER = 7` enum, update helpers |
| `include/printer_discovery.h` | Add `has_snapmaker_` flag, detection in `parse_objects()` and finalization |
| `src/printer/ams_backend.cpp` | Add `case AmsType::SNAPMAKER` in factory |
| `src/api/moonraker_discovery_sequence.cpp` | Add Snapmaker object subscriptions |
| `src/printer/tool_state.cpp` | Add Snapmaker-specific `init_tools()` and `update_from_status()` paths |

---

## Task 1: Add SNAPMAKER Enum and Helpers

**Files:**
- Modify: `include/ams_types.h:40-131`
- Test: `tests/unit/test_ams_backend_snapmaker.cpp` (create)

- [ ] **Step 1: Create test file with enum tests**

Create `tests/unit/test_ams_backend_snapmaker.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_types.h"

#include "../catch_amalgamated.hpp"

using namespace helix::printer;

TEST_CASE("Snapmaker type enum", "[ams][snapmaker]") {
    SECTION("SNAPMAKER is a valid AmsType") {
        auto t = AmsType::SNAPMAKER;
        REQUIRE(t != AmsType::NONE);
        REQUIRE(static_cast<int>(t) == 7);
    }

    SECTION("SNAPMAKER is both a tool changer and filament system") {
        REQUIRE(is_tool_changer(AmsType::SNAPMAKER));
        REQUIRE(is_filament_system(AmsType::SNAPMAKER));
    }

    SECTION("ams_type_to_string returns Snapmaker") {
        REQUIRE(std::string(ams_type_to_string(AmsType::SNAPMAKER)) == "Snapmaker");
    }

    SECTION("ams_type_from_string parses Snapmaker variants") {
        REQUIRE(ams_type_from_string("snapmaker") == AmsType::SNAPMAKER);
        REQUIRE(ams_type_from_string("Snapmaker") == AmsType::SNAPMAKER);
        REQUIRE(ams_type_from_string("snapswap") == AmsType::SNAPMAKER);
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[snapmaker]" -v`
Expected: FAIL — `AmsType::SNAPMAKER` not defined

- [ ] **Step 3: Add SNAPMAKER to AmsType enum**

In `include/ams_types.h`, change line 47:

```cpp
    CFS = 6,           ///< Creality Filament System (K2 series, RS-485)
    SNAPMAKER = 7      ///< Snapmaker U1 SnapSwap toolchanger
};
```

- [ ] **Step 4: Add to ams_type_to_string**

In `include/ams_types.h`, add before the `default:` case (around line 68):

```cpp
    case AmsType::SNAPMAKER:
        return "Snapmaker";
```

- [ ] **Step 5: Add to ams_type_from_string**

In `include/ams_types.h`, add before the `return AmsType::NONE;` line (around line 99):

```cpp
    if (str == "snapmaker" || str == "Snapmaker" || str == "snapswap" || str == "SnapSwap") {
        return AmsType::SNAPMAKER;
    }
```

- [ ] **Step 6: Add to is_tool_changer**

In `include/ams_types.h`, change `is_tool_changer()` (line 114-116):

```cpp
inline bool is_tool_changer(AmsType type) {
    return type == AmsType::TOOL_CHANGER || type == AmsType::SNAPMAKER;
}
```

- [ ] **Step 7: Add to is_filament_system**

In `include/ams_types.h`, change `is_filament_system()` (line 128-131):

```cpp
inline bool is_filament_system(AmsType type) {
    return type == AmsType::HAPPY_HARE || type == AmsType::AFC || type == AmsType::ACE ||
           type == AmsType::AD5X_IFS || type == AmsType::CFS || type == AmsType::SNAPMAKER;
}
```

- [ ] **Step 8: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[snapmaker]" -v`
Expected: All PASS

- [ ] **Step 9: Commit**

```bash
git add include/ams_types.h tests/unit/test_ams_backend_snapmaker.cpp
git commit -m "feat(ams): add AmsType::SNAPMAKER enum and helpers"
```

---

## Task 2: Add Snapmaker Detection in PrinterDiscovery

**Files:**
- Modify: `include/printer_discovery.h`
- Test: `tests/unit/test_ams_backend_snapmaker.cpp`

- [ ] **Step 1: Add detection test**

Append to `tests/unit/test_ams_backend_snapmaker.cpp`:

```cpp
#include "printer_discovery.h"

TEST_CASE("Snapmaker detection via filament_detect", "[ams][snapmaker]") {
    helix::PrinterDiscovery discovery;

    SECTION("filament_detect triggers SNAPMAKER detection") {
        // Simulate object list containing filament_detect
        nlohmann::json objects = nlohmann::json::array({
            "extruder", "extruder1", "extruder2", "extruder3",
            "toolchanger", "filament_detect", "toolhead",
            "heater_bed", "print_task_config"
        });
        discovery.parse_objects(objects);
        REQUIRE(discovery.has_snapmaker());
        // filament_detect should take priority over empty toolchanger
        REQUIRE(discovery.mmu_type() == AmsType::SNAPMAKER);
    }

    SECTION("empty toolchanger without filament_detect is TOOL_CHANGER") {
        nlohmann::json objects = nlohmann::json::array({
            "extruder", "toolchanger", "tool T0", "tool T1", "toolhead"
        });
        discovery.parse_objects(objects);
        REQUIRE_FALSE(discovery.has_snapmaker());
        REQUIRE(discovery.has_tool_changer());
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[snapmaker]" -v`
Expected: FAIL — `has_snapmaker()` not defined

- [ ] **Step 3: Add has_snapmaker_ flag and getter**

In `include/printer_discovery.h`, near the `has_tool_changer_` declaration (around line 1132), add:

```cpp
    bool has_snapmaker_ = false;
```

Near the `has_tool_changer()` getter (around line 654-656), add:

```cpp
    [[nodiscard]] bool has_snapmaker() const {
        return has_snapmaker_;
    }
```

- [ ] **Step 4: Add detection in parse_objects**

In `include/printer_discovery.h`, in the `parse_objects()` method near the `"toolchanger"` detection (around line 299), add **before** the toolchanger check:

```cpp
            // Snapmaker U1 detection (must check before generic toolchanger)
            else if (name == "filament_detect") {
                has_snapmaker_ = true;
            }
```

- [ ] **Step 5: Add finalization for SNAPMAKER**

In `include/printer_discovery.h`, in the finalization block (around line 437), add **before** the `has_tool_changer_` check:

```cpp
        if (has_snapmaker_) {
            // Snapmaker U1: has filament_detect object — takes priority over empty toolchanger
            detected_ams_systems_.push_back({AmsType::SNAPMAKER, "Snapmaker"});
            mmu_type_ = AmsType::SNAPMAKER;
        } else if (has_mmu_) {
```

And change the existing `} else if (has_tool_changer_` to just `else if (has_tool_changer_` (removing the first `else`):

This ensures the Snapmaker path runs first. The `has_mmu_` and `has_tool_changer_` paths remain as fallbacks.

- [ ] **Step 6: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[snapmaker]" -v`
Expected: All PASS

- [ ] **Step 7: Commit**

```bash
git add include/printer_discovery.h tests/unit/test_ams_backend_snapmaker.cpp
git commit -m "feat(discovery): detect Snapmaker U1 via filament_detect object"
```

---

## Task 3: Create AmsBackendSnapmaker Header and Stub

**Files:**
- Create: `include/ams_backend_snapmaker.h`
- Create: `src/printer/ams_backend_snapmaker.cpp`
- Test: `tests/unit/test_ams_backend_snapmaker.cpp`

- [ ] **Step 1: Add construction test**

Append to `tests/unit/test_ams_backend_snapmaker.cpp`:

```cpp
#include "ams_backend_snapmaker.h"

TEST_CASE("AmsBackendSnapmaker construction", "[ams][snapmaker]") {
    SECTION("type returns SNAPMAKER") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        REQUIRE(backend.get_type() == AmsType::SNAPMAKER);
    }

    SECTION("topology is PARALLEL") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        REQUIRE(backend.get_topology() == PathTopology::PARALLEL);
    }

    SECTION("name is Snapmaker SnapSwap") {
        AmsBackendSnapmaker backend(nullptr, nullptr);
        auto info = backend.get_system_info();
        REQUIRE(info.name == "Snapmaker SnapSwap");
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[snapmaker]" -v`
Expected: FAIL — `AmsBackendSnapmaker` not defined

- [ ] **Step 3: Create header**

Create `include/ams_backend_snapmaker.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_subscription_backend.h"
#include "async_lifetime_guard.h"

#include <string>
#include <vector>

namespace helix::printer {

/// Snapmaker U1 SnapSwap toolchanger backend
///
/// Maps the U1's proprietary Klipper objects to the AMS abstraction:
/// - 4 physical toolheads (extruder0-3) with custom state/park_pin/active_pin fields
/// - RFID filament detection (filament_detect) per channel
/// - Filament feed modules (filament_feed left/right)
/// - Virtual-to-physical extruder mapping (print_task_config.extruder_map_table)
class AmsBackendSnapmaker : public AmsSubscriptionBackend {
  public:
    AmsBackendSnapmaker(MoonrakerAPI* api, helix::MoonrakerClient* client);

    [[nodiscard]] AmsType get_type() const override { return AmsType::SNAPMAKER; }

    // State queries
    [[nodiscard]] AmsSystemInfo get_system_info() const override;
    [[nodiscard]] SlotInfo get_slot_info(int slot_index) const override;

    // Path visualization
    [[nodiscard]] PathTopology get_topology() const override { return PathTopology::PARALLEL; }
    [[nodiscard]] PathSegment get_filament_segment() const override;
    [[nodiscard]] PathSegment get_slot_filament_segment(int slot_index) const override;
    [[nodiscard]] PathSegment infer_error_segment() const override;

    // Operations
    AmsError load_filament(int slot_index) override;
    AmsError unload_filament(int slot_index = -1) override;
    AmsError select_slot(int slot_index) override;
    AmsError change_tool(int tool_number) override;
    AmsError reset() override;
    AmsError recover() override;
    AmsError cancel() override;

    // Slot management
    AmsError set_slot_info(int slot_index, const SlotInfo& info, bool persist = true) override;
    AmsError set_tool_mapping(int tool_number, int slot_index) override;

    // Bypass (not applicable — parallel topology)
    AmsError enable_bypass() override;
    AmsError disable_bypass() override;
    [[nodiscard]] bool is_bypass_active() const override { return false; }

    // Capabilities
    [[nodiscard]] helix::printer::ToolMappingCapabilities
    get_tool_mapping_capabilities() const override;
    [[nodiscard]] bool supports_auto_heat_on_load() const override { return false; }
    [[nodiscard]] bool has_environment_sensors() const override { return false; }
    [[nodiscard]] bool tracks_weight_locally() const override { return false; }
    [[nodiscard]] bool manages_active_spool() const override { return false; }

    /// Parse extruder status JSON with Snapmaker-custom fields (public for testing)
    static SlotInfo parse_rfid_info(const nlohmann::json& rfid_entry);

    /// Parse extruder tool state from status JSON (public for testing)
    struct ExtruderToolState {
        std::string state;    // "PARKED", etc.
        bool park_pin = false;
        bool active_pin = false;
        bool activating_move = false;
        std::array<double, 3> extruder_offset = {0, 0, 0};
        int switch_count = 0;
        int retry_count = 0;
        int error_count = 0;
    };
    static ExtruderToolState parse_extruder_state(const nlohmann::json& extruder_json);

    static constexpr int NUM_PHYSICAL_TOOLS = 4;

  protected:
    void handle_status_update(const nlohmann::json& notification) override;
    const char* backend_log_tag() const override { return "[AMS Snapmaker]"; }
    void on_started() override;

  private:
    int active_tool_ = -1;
    std::array<ExtruderToolState, NUM_PHYSICAL_TOOLS> tool_states_;
    std::array<SlotInfo, NUM_PHYSICAL_TOOLS> slot_infos_;

    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix::printer
```

- [ ] **Step 4: Create stub implementation**

Create `src/printer/ams_backend_snapmaker.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_backend_snapmaker.h"

#include "moonraker_error.h"
#include "ui_update_queue.h"

#include "hv/json.hpp"

#include <spdlog/spdlog.h>

namespace helix::printer {

using json = nlohmann::json;

AmsBackendSnapmaker::AmsBackendSnapmaker(MoonrakerAPI* api, helix::MoonrakerClient* client)
    : AmsSubscriptionBackend(api, client)
{
    // Initialize slot infos with defaults
    for (int i = 0; i < NUM_PHYSICAL_TOOLS; ++i) {
        slot_infos_[i].slot_index = i;
        slot_infos_[i].global_index = i;
        slot_infos_[i].status = SlotStatus::UNKNOWN;
    }
}

AmsSystemInfo AmsBackendSnapmaker::get_system_info() const
{
    AmsSystemInfo info;
    info.name = "Snapmaker SnapSwap";
    info.units.resize(1);
    info.units[0].unit_index = 0;
    info.units[0].slot_count = NUM_PHYSICAL_TOOLS;
    return info;
}

SlotInfo AmsBackendSnapmaker::get_slot_info(int slot_index) const
{
    if (slot_index >= 0 && slot_index < NUM_PHYSICAL_TOOLS) {
        return slot_infos_[slot_index];
    }
    return {};
}

PathSegment AmsBackendSnapmaker::get_filament_segment() const { return {}; }
PathSegment AmsBackendSnapmaker::get_slot_filament_segment(int) const { return {}; }
PathSegment AmsBackendSnapmaker::infer_error_segment() const { return {}; }

AmsError AmsBackendSnapmaker::load_filament(int slot_index)
{
    if (!api_)
        return AmsError::NOT_CONNECTED;
    if (slot_index < 0 || slot_index >= NUM_PHYSICAL_TOOLS)
        return AmsError::INVALID_SLOT;
    spdlog::info("{} Loading filament for slot {}", backend_log_tag(), slot_index);
    api_->send_gcode("AUTO_FEEDING");
    return AmsError::OK;
}

AmsError AmsBackendSnapmaker::unload_filament(int slot_index)
{
    if (!api_)
        return AmsError::NOT_CONNECTED;
    spdlog::info("{} Unloading filament for slot {}", backend_log_tag(), slot_index);
    api_->send_gcode("INNER_FILAMENT_UNLOAD");
    return AmsError::OK;
}

AmsError AmsBackendSnapmaker::select_slot(int slot_index)
{
    return change_tool(slot_index);
}

AmsError AmsBackendSnapmaker::change_tool(int tool_number)
{
    if (!api_)
        return AmsError::NOT_CONNECTED;
    if (tool_number < 0 || tool_number >= NUM_PHYSICAL_TOOLS)
        return AmsError::INVALID_SLOT;
    spdlog::info("{} Changing to tool T{}", backend_log_tag(), tool_number);
    api_->send_gcode(fmt::format("T{}", tool_number));
    return AmsError::OK;
}

AmsError AmsBackendSnapmaker::reset() { return AmsError::NOT_SUPPORTED; }
AmsError AmsBackendSnapmaker::recover() { return AmsError::NOT_SUPPORTED; }
AmsError AmsBackendSnapmaker::cancel() { return AmsError::NOT_SUPPORTED; }

AmsError AmsBackendSnapmaker::set_slot_info(int, const SlotInfo&, bool)
{
    return AmsError::NOT_SUPPORTED;
}

AmsError AmsBackendSnapmaker::set_tool_mapping(int, int)
{
    return AmsError::NOT_SUPPORTED;
}

AmsError AmsBackendSnapmaker::enable_bypass() { return AmsError::NOT_SUPPORTED; }
AmsError AmsBackendSnapmaker::disable_bypass() { return AmsError::NOT_SUPPORTED; }

ToolMappingCapabilities AmsBackendSnapmaker::get_tool_mapping_capabilities() const
{
    return {
        .supported = true,
        .max_tools = NUM_PHYSICAL_TOOLS,
        .tool_names = {"T0", "T1", "T2", "T3"},
    };
}

// --- Static parsers (public for testing) ---

AmsBackendSnapmaker::ExtruderToolState
AmsBackendSnapmaker::parse_extruder_state(const json& j)
{
    ExtruderToolState state;
    state.state = j.value("state", "");
    state.park_pin = j.value("park_pin", false);
    state.active_pin = j.value("active_pin", false);
    state.activating_move = j.value("activating_move", false);
    state.switch_count = j.value("switch_count", 0);
    state.retry_count = j.value("retry_count", 0);
    state.error_count = j.value("error_count", 0);

    if (j.contains("extruder_offset") && j["extruder_offset"].is_array() &&
        j["extruder_offset"].size() >= 3) {
        state.extruder_offset[0] = j["extruder_offset"][0].get<double>();
        state.extruder_offset[1] = j["extruder_offset"][1].get<double>();
        state.extruder_offset[2] = j["extruder_offset"][2].get<double>();
    }
    return state;
}

SlotInfo AmsBackendSnapmaker::parse_rfid_info(const json& rfid)
{
    SlotInfo slot;
    slot.material = rfid.value("MAIN_TYPE", "");
    slot.brand = rfid.value("MANUFACTURER", "");
    if (slot.brand.empty()) {
        slot.brand = rfid.value("VENDOR", "");
    }
    slot.nozzle_temp_min = rfid.value("HOTEND_MIN_TEMP", 0);
    slot.nozzle_temp_max = rfid.value("HOTEND_MAX_TEMP", 0);
    slot.bed_temp = rfid.value("BED_TEMP", 0);
    slot.total_weight_g = static_cast<float>(rfid.value("WEIGHT", 0));

    // Parse ARGB color: upper 8 bits are alpha, lower 24 are RGB
    if (rfid.contains("ARGB_COLOR")) {
        auto argb = rfid["ARGB_COLOR"].get<uint32_t>();
        slot.color_rgb = argb & 0x00FFFFFF;
    }

    // Use SUB_TYPE as color_name hint (e.g., "SnapSpeed", "Silk")
    slot.color_name = rfid.value("SUB_TYPE", "");

    slot.status = SlotStatus::AVAILABLE;
    return slot;
}

// --- Status update handler ---

void AmsBackendSnapmaker::handle_status_update(const json& notification)
{
    bool changed = false;

    // Parse per-extruder state
    static const std::string extruder_keys[] = {"extruder", "extruder1", "extruder2", "extruder3"};
    for (int i = 0; i < NUM_PHYSICAL_TOOLS; ++i) {
        if (notification.contains(extruder_keys[i])) {
            auto prev_state = tool_states_[i].state;
            tool_states_[i] = parse_extruder_state(notification[extruder_keys[i]]);
            if (tool_states_[i].state != prev_state) {
                changed = true;
            }
        }
    }

    // Detect active tool from state field
    for (int i = 0; i < NUM_PHYSICAL_TOOLS; ++i) {
        if (tool_states_[i].state != "PARKED" && !tool_states_[i].state.empty()) {
            if (active_tool_ != i) {
                active_tool_ = i;
                changed = true;
            }
            break;
        }
    }

    // Parse RFID filament data
    if (notification.contains("filament_detect")) {
        const auto& fd = notification["filament_detect"];
        if (fd.contains("info") && fd["info"].is_array()) {
            const auto& info_arr = fd["info"];
            for (int i = 0; i < NUM_PHYSICAL_TOOLS && i < static_cast<int>(info_arr.size()); ++i) {
                slot_infos_[i] = parse_rfid_info(info_arr[i]);
                slot_infos_[i].slot_index = i;
                slot_infos_[i].global_index = i;
            }
            changed = true;
        }

        // Parse filament_detect.state array for presence
        if (fd.contains("state") && fd["state"].is_array()) {
            const auto& state_arr = fd["state"];
            for (int i = 0; i < NUM_PHYSICAL_TOOLS && i < static_cast<int>(state_arr.size()); ++i) {
                int state_val = state_arr[i].get<int>();
                if (state_val != 0) {
                    slot_infos_[i].status = SlotStatus::EMPTY;
                    slot_infos_[i].material.clear();
                }
            }
        }
    }

    // Parse filament_feed left/right for presence detection
    for (const auto& side : {"filament_feed left", "filament_feed right"}) {
        if (notification.contains(side)) {
            const auto& feed = notification[side];
            for (int i = 0; i < NUM_PHYSICAL_TOOLS; ++i) {
                std::string key = "extruder" + std::to_string(i);
                if (i == 0) key = "extruder0";
                if (feed.contains(key)) {
                    bool detected = feed[key].value("filament_detected", false);
                    if (!detected && slot_infos_[i].status == SlotStatus::AVAILABLE) {
                        slot_infos_[i].status = SlotStatus::EMPTY;
                        changed = true;
                    }
                }
            }
        }
    }

    if (changed) {
        notify_update();
    }
}

void AmsBackendSnapmaker::on_started()
{
    spdlog::info("{} Backend started — monitoring 4 toolheads", backend_log_tag());
}

} // namespace helix::printer
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[snapmaker]" -v`
Expected: All PASS

- [ ] **Step 6: Commit**

```bash
git add include/ams_backend_snapmaker.h src/printer/ams_backend_snapmaker.cpp tests/unit/test_ams_backend_snapmaker.cpp
git commit -m "feat(ams): add AmsBackendSnapmaker class with RFID and tool state parsing"
```

---

## Task 4: Add Parsing Tests

**Files:**
- Modify: `tests/unit/test_ams_backend_snapmaker.cpp`

- [ ] **Step 1: Add extruder state parsing tests**

Append to `tests/unit/test_ams_backend_snapmaker.cpp`:

```cpp
TEST_CASE("Snapmaker extruder state parsing", "[ams][snapmaker]") {
    using ES = AmsBackendSnapmaker::ExtruderToolState;

    SECTION("parses parked extruder") {
        auto j = nlohmann::json::parse(R"({
            "state": "PARKED",
            "park_pin": true,
            "active_pin": false,
            "grab_valid_pin": false,
            "activating_move": false,
            "extruder_offset": [0.073, -0.037, 0.0],
            "switch_count": 86,
            "retry_count": 0,
            "error_count": 1
        })");
        ES state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state == "PARKED");
        REQUIRE(state.park_pin == true);
        REQUIRE(state.active_pin == false);
        REQUIRE(state.activating_move == false);
        REQUIRE(state.extruder_offset[0] == Catch::Approx(0.073));
        REQUIRE(state.extruder_offset[1] == Catch::Approx(-0.037));
        REQUIRE(state.extruder_offset[2] == Catch::Approx(0.0));
        REQUIRE(state.switch_count == 86);
        REQUIRE(state.error_count == 1);
    }

    SECTION("handles missing fields gracefully") {
        auto j = nlohmann::json::parse("{}");
        ES state = AmsBackendSnapmaker::parse_extruder_state(j);
        REQUIRE(state.state.empty());
        REQUIRE(state.park_pin == false);
        REQUIRE(state.switch_count == 0);
    }
}

TEST_CASE("Snapmaker RFID info parsing", "[ams][snapmaker]") {
    SECTION("parses full RFID tag data") {
        auto j = nlohmann::json::parse(R"({
            "VERSION": 1,
            "VENDOR": "Snapmaker",
            "MANUFACTURER": "Polymaker",
            "MAIN_TYPE": "PLA",
            "SUB_TYPE": "SnapSpeed",
            "ARGB_COLOR": 4278716941,
            "DIAMETER": 175,
            "WEIGHT": 500,
            "HOTEND_MAX_TEMP": 230,
            "HOTEND_MIN_TEMP": 190,
            "BED_TEMP": 60,
            "OFFICIAL": true
        })");
        SlotInfo slot = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(slot.material == "PLA");
        REQUIRE(slot.brand == "Polymaker");
        REQUIRE(slot.color_name == "SnapSpeed");
        REQUIRE(slot.nozzle_temp_min == 190);
        REQUIRE(slot.nozzle_temp_max == 230);
        REQUIRE(slot.bed_temp == 60);
        REQUIRE(slot.total_weight_g == 500.0f);
        REQUIRE(slot.status == SlotStatus::AVAILABLE);
        // ARGB 0xFF080A0D -> RGB 0x080A0D
        REQUIRE(slot.color_rgb == 0x080A0D);
    }

    SECTION("falls back to VENDOR when MANUFACTURER empty") {
        auto j = nlohmann::json::parse(R"({
            "VENDOR": "Generic",
            "MANUFACTURER": "",
            "MAIN_TYPE": "PETG"
        })");
        SlotInfo slot = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(slot.brand == "Generic");
        REQUIRE(slot.material == "PETG");
    }

    SECTION("handles missing RFID fields") {
        auto j = nlohmann::json::parse("{}");
        SlotInfo slot = AmsBackendSnapmaker::parse_rfid_info(j);
        REQUIRE(slot.material.empty());
        REQUIRE(slot.brand.empty());
        REQUIRE(slot.nozzle_temp_min == 0);
    }
}
```

- [ ] **Step 2: Run tests**

Run: `make test && ./build/bin/helix-tests "[snapmaker]" -v`
Expected: All PASS

- [ ] **Step 3: Commit**

```bash
git add tests/unit/test_ams_backend_snapmaker.cpp
git commit -m "test(ams): add Snapmaker extruder state and RFID parsing tests"
```

---

## Task 5: Wire Up Factory, Subscriptions, and ToolState

**Files:**
- Modify: `src/printer/ams_backend.cpp`
- Modify: `src/api/moonraker_discovery_sequence.cpp`
- Modify: `src/printer/tool_state.cpp`

- [ ] **Step 1: Add SNAPMAKER case to factory**

In `src/printer/ams_backend.cpp`, add `#include "ams_backend_snapmaker.h"` near the other backend includes at the top of the file.

Then add a new case before `case AmsType::NONE:` in the `create()` switch (around line 280):

```cpp
    case AmsType::SNAPMAKER:
        if (!api || !client) {
            spdlog::error("[AMS Backend] Snapmaker requires MoonrakerAPI and MoonrakerClient");
            return nullptr;
        }
        spdlog::debug("[AMS Backend] Creating Snapmaker SnapSwap backend");
        return std::make_unique<AmsBackendSnapmaker>(api, client);
```

- [ ] **Step 2: Add Snapmaker subscriptions**

In `src/api/moonraker_discovery_sequence.cpp`, in `complete_discovery_subscription()`, add after the CFS subscription block (around line 846):

```cpp
    // Snapmaker U1 SnapSwap — RFID filament, feed modules, task config
    if (hardware_.mmu_type() == AmsType::SNAPMAKER) {
        subscription_objects["filament_detect"] = nullptr;
        subscription_objects["filament_feed left"] = nullptr;
        subscription_objects["filament_feed right"] = nullptr;
        subscription_objects["print_task_config"] = nullptr;
        subscription_objects["machine_state_manager"] = nullptr;
        for (int i = 0; i < 4; ++i) {
            subscription_objects[fmt::format("filament_motion_sensor e{}_filament", i)] = nullptr;
        }
        spdlog::info("[Moonraker Client] Subscribing to Snapmaker filament + feed objects");
    }
```

- [ ] **Step 3: Add Snapmaker tool init path**

In `src/printer/tool_state.cpp`, in `init_tools()`, add a new branch **before** the existing `if (hardware.has_tool_changer() && !hardware.tool_names().empty())` check (around line 82):

```cpp
    if (hardware.has_snapmaker()) {
        // Snapmaker U1: 4 fixed toolheads, not using viesturz tool objects
        static const std::string extruder_names[] = {"extruder", "extruder1", "extruder2", "extruder3"};
        for (int i = 0; i < 4; ++i) {
            ToolInfo tool;
            tool.index = i;
            tool.name = fmt::format("T{}", i);
            tool.extruder_name = extruder_names[i];
            tool.heater_name = extruder_names[i]; // Extruder is also the heater
            tool.fan_name = (i == 0) ? std::optional<std::string>("fan")
                                     : std::optional<std::string>(fmt::format("fan_generic e{}_fan", i));
            spdlog::debug("[ToolState] Snapmaker tool {}: extruder={}, fan={}", i,
                          tool.extruder_name.value_or("none"), tool.fan_name.value_or("none"));
            tools_.push_back(std::move(tool));
        }
    } else if (hardware.has_tool_changer() && !hardware.tool_names().empty()) {
```

You also need to add `#include "printer_discovery.h"` if not already present, and ensure `has_snapmaker()` is accessible.

- [ ] **Step 4: Build and run all tests**

Run: `make test && ./build/bin/helix-tests "[snapmaker]" -v`
Then: `make test-run` (run full test suite to check for regressions)
Expected: All PASS, no regressions

- [ ] **Step 5: Commit**

```bash
git add src/printer/ams_backend.cpp src/api/moonraker_discovery_sequence.cpp src/printer/tool_state.cpp
git commit -m "feat(ams): wire Snapmaker backend into factory, subscriptions, and ToolState"
```

---

## Task 6: Build and Deploy to U1

**Files:** None (deployment task)

- [ ] **Step 1: Build the binary**

Run: `make snapmaker-u1-docker`
Expected: Successful build producing `build/snapmaker-u1/bin/helix-screen`

- [ ] **Step 2: Deploy and run in foreground**

Run: `make deploy-snapmaker-u1-fg SNAPMAKER_U1_HOST=192.168.30.103`
Expected: HelixScreen starts, renders to 480x320 display, connects to Moonraker

- [ ] **Step 3: Verify on hardware**

Check:
- [ ] Display renders at 480x320
- [ ] Touch input works
- [ ] Printer is detected as Snapmaker U1
- [ ] 4 tools shown with temperatures
- [ ] RFID filament data visible (material, color)
- [ ] Tool switcher widget displays T0-T3

- [ ] **Step 4: Commit any fixes**

```bash
git add -A
git commit -m "fix(snapmaker): hardware validation fixes"
```

---

## Summary

| Task | Description | TDD? |
|------|-------------|------|
| 1 | SNAPMAKER enum + helpers | Yes |
| 2 | PrinterDiscovery detection | Yes |
| 3 | Backend header + stub implementation | Yes |
| 4 | Parsing unit tests | Yes |
| 5 | Wire factory, subscriptions, ToolState | Build + test |
| 6 | Build and deploy to U1 hardware | Hardware validation |
