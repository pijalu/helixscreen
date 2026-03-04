# Proportional Buffer Feedback Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add proportional sync feedback visualization for Happy Hare's buffer sensor, enhance AFC's buffer modal, and unify clog detection + buffer meter into a carousel "Filament Health" dashboard widget.

**Architecture:** Three layers — (1) parse `sync_feedback_bias_modelled`/`sync_feedback_bias_raw` from HH's MMU status into `AmsSystemInfo`, (2) build a reusable `UiBufferMeter` drawing component showing sliding-rectangle buffer visualization, (3) integrate into canvas color, buffer click modal, and carousel dashboard widget.

**Tech Stack:** C++17, LVGL 9.5, XML declarative UI, Catch2 tests, spdlog logging.

**Design doc:** `docs/plans/2026-03-04-proportional-buffer-feedback-design.md`

**Key reference files:**
- Existing clog meter: `include/ui_clog_meter.h`, `src/ui/ui_clog_meter.cpp`
- Clog widget: `src/ui/panel_widgets/clog_detection_widget.h/.cpp`
- HH backend: `src/printer/ams_backend_happy_hare.cpp`
- AMS types: `include/ams_types.h`
- Buffer coil drawing: `src/ui/ui_filament_path_canvas.cpp:1182-1306`
- Buffer click modal: `src/ui/ui_panel_ams.cpp:1106-1177`
- Carousel API: `include/ui_carousel.h`
- Carousel widget example: `src/ui/panel_widgets/fan_stack_widget.cpp`

---

### Task 1: Add sync_feedback_bias fields to AmsSystemInfo

**Files:**
- Modify: `include/ams_types.h:806-813`

**Step 1: Add fields**

In `AmsSystemInfo`, after the existing `sync_feedback_flow_rate` field (line 812), add:

```cpp
    float sync_feedback_flow_rate = -1; ///< Sync feedback flow rate
    float sync_feedback_bias = -2;      ///< Modelled bias [-1.0,1.0], -2=unavailable
    float sync_feedback_bias_raw = -2;  ///< Raw sensor bias [-1.0,1.0], -2=unavailable
```

Using -2 as sentinel since the valid range is [-1.0, 1.0].

**Step 2: Commit**

```bash
git add include/ams_types.h
git commit -m "feat(ams): add sync_feedback_bias fields to AmsSystemInfo"
```

---

### Task 2: Parse sync_feedback_bias from Happy Hare

**Files:**
- Modify: `src/printer/ams_backend_happy_hare.cpp:566-650`
- Test: `tests/unit/test_ams_backend_happy_hare.cpp`

**Step 1: Write the failing test**

Add to `tests/unit/test_ams_backend_happy_hare.cpp`, in the existing `[ams][happy_hare][v4]` test section:

```cpp
TEST_CASE_METHOD(AmsBackendHappyHareFixture,
                 "HappyHare: parses sync_feedback_bias_modelled and raw",
                 "[ams][happy_hare][v4][sync_feedback]") {
    nlohmann::json mmu_data = {
        {"sync_feedback_state", "compressed"},
        {"sync_feedback_bias_modelled", 0.45},
        {"sync_feedback_bias_raw", 0.52},
    };
    helper_.apply_mmu_status(mmu_data);
    auto info = helper_.get_system_info();

    REQUIRE(info.sync_feedback_bias == Catch::Approx(0.45f));
    REQUIRE(info.sync_feedback_bias_raw == Catch::Approx(0.52f));

    SECTION("negative values for tension") {
        mmu_data["sync_feedback_bias_modelled"] = -0.7;
        mmu_data["sync_feedback_bias_raw"] = -0.65;
        helper_.apply_mmu_status(mmu_data);
        info = helper_.get_system_info();
        REQUIRE(info.sync_feedback_bias == Catch::Approx(-0.7f));
        REQUIRE(info.sync_feedback_bias_raw == Catch::Approx(-0.65f));
    }

    SECTION("missing fields remain at sentinel") {
        nlohmann::json minimal = {{"sync_feedback_state", "neutral"}};
        // Reset helper to get clean state
        AmsBackendHappyHareTestHelper fresh;
        fresh.apply_mmu_status(minimal);
        auto fresh_info = fresh.get_system_info();
        REQUIRE(fresh_info.sync_feedback_bias == Catch::Approx(-2.0f));
        REQUIRE(fresh_info.sync_feedback_bias_raw == Catch::Approx(-2.0f));
    }
}
```

**Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[sync_feedback]" -v`
Expected: FAIL — `apply_mmu_status` doesn't parse the new fields yet.

**Step 3: Write implementation**

In `src/printer/ams_backend_happy_hare.cpp`, after the `sync_feedback_state` parsing block (line 570), add:

```cpp
    // Parse sync_feedback_bias_modelled: printer.mmu.sync_feedback_bias_modelled (v4)
    if (mmu_data.contains("sync_feedback_bias_modelled") &&
        mmu_data["sync_feedback_bias_modelled"].is_number()) {
        system_info_.sync_feedback_bias =
            mmu_data["sync_feedback_bias_modelled"].get<float>();
        spdlog::trace("[AMS HappyHare] Sync feedback bias (modelled): {:.3f}",
                      system_info_.sync_feedback_bias);
    }

    // Parse sync_feedback_bias_raw: printer.mmu.sync_feedback_bias_raw (v4)
    if (mmu_data.contains("sync_feedback_bias_raw") &&
        mmu_data["sync_feedback_bias_raw"].is_number()) {
        system_info_.sync_feedback_bias_raw =
            mmu_data["sync_feedback_bias_raw"].get<float>();
        spdlog::trace("[AMS HappyHare] Sync feedback bias (raw): {:.3f}",
                      system_info_.sync_feedback_bias_raw);
    }
```

**Step 4: Run test to verify it passes**

Run: `make test && ./build/bin/helix-tests "[sync_feedback]" -v`
Expected: PASS

**Step 5: Commit**

```bash
git add src/printer/ams_backend_happy_hare.cpp tests/unit/test_ams_backend_happy_hare.cpp
git commit -m "feat(ams): parse sync_feedback_bias_modelled from Happy Hare"
```

---

### Task 3: Proportional color mapping for buffer box on canvas

**Files:**
- Modify: `src/ui/ui_filament_path_canvas.cpp:1182-1210`
- Modify: `include/ui_filament_path_canvas.h` (add bias field to FilamentPathData or function signature)

**Context:** Currently `draw_buffer_coil()` uses 3 discrete states for color (green/orange/red). For Happy Hare, we want to interpolate color based on the continuous bias value. AFC keeps the discrete logic.

**Step 1: Add `buffer_bias` field to the canvas data struct**

Find the struct `FilamentPathData` in `ui_filament_path_canvas.cpp` (around line 94-140). It should have `buffer_state` and `buffer_fault_state`. Add:

```cpp
    float buffer_bias = -2.0f;  ///< Proportional bias [-1.0,1.0], -2=unavailable (use discrete)
```

Also add to the public API in `include/ui_filament_path_canvas.h`:

```cpp
void ui_filament_path_canvas_set_buffer_bias(lv_obj_t* obj, float bias);
```

**Step 2: Implement the setter**

In `ui_filament_path_canvas.cpp`, alongside the existing `ui_filament_path_canvas_set_buffer_info()`:

```cpp
void ui_filament_path_canvas_set_buffer_bias(lv_obj_t* obj, float bias) {
    auto* data = get_data(obj);
    if (data) {
        data->buffer_bias = bias;
        lv_obj_invalidate(obj);
    }
}
```

**Step 3: Modify `draw_buffer_coil()` color logic**

Replace the color selection block at lines 1195-1208 with:

```cpp
    lv_color_t border_color;
    lv_color_t coil_bg = bg_color;

    if (buffer_fault_state >= 2) {
        // Fault — always red regardless of bias
        border_color = lv_color_hex(0xEF4444);
        coil_bg = lv_color_hex(0x3F1111);
    } else if (buffer_bias > -1.5f) {
        // Proportional mode: interpolate green → orange → red based on abs(bias)
        float abs_bias = std::fabs(buffer_bias);
        abs_bias = std::clamp(abs_bias, 0.0f, 1.0f);

        if (abs_bias < 0.3f) {
            // Green zone
            border_color = lv_color_hex(0x22C55E);
        } else if (abs_bias < 0.7f) {
            // Interpolate green → orange
            float t = (abs_bias - 0.3f) / 0.4f;
            border_color = ph_blend(lv_color_hex(0x22C55E), lv_color_hex(0xF59E0B), t);
        } else {
            // Interpolate orange → red
            float t = (abs_bias - 0.7f) / 0.3f;
            border_color = ph_blend(lv_color_hex(0xF59E0B), lv_color_hex(0xEF4444), t);
        }
        if (has_filament) {
            coil_bg = ph_blend(bg_color, filament_color, 0.33f);
        }
    } else if (buffer_fault_state == 1 || buffer_state != 0) {
        // Discrete fallback (AFC or unavailable bias)
        border_color = lv_color_hex(0xF59E0B);
        if (has_filament) {
            coil_bg = ph_blend(bg_color, filament_color, 0.33f);
        }
    } else {
        border_color = lv_color_hex(0x22C55E);
        if (has_filament) {
            coil_bg = ph_blend(bg_color, filament_color, 0.33f);
        }
    }
```

Note: Need to pass `buffer_bias` into `draw_buffer_coil()`. Add it as a parameter after `buffer_fault_state`:

```cpp
static void draw_buffer_coil(lv_layer_t* layer, int32_t cx, int32_t cy, int32_t hub_w,
                             int32_t hub_h, int buffer_state, int buffer_fault_state,
                             float buffer_bias,
                             lv_color_t bg_color, int32_t radius, bool has_filament,
                             lv_color_t filament_color)
```

Update the call site at line 2018 to pass `data->buffer_bias`.

**Step 4: Wire bias into canvas from ui_ams_detail.cpp**

In `ui_ams_detail.cpp`, where buffer state is set on the canvas (search for `ui_filament_path_canvas_set_buffer_info`), add after it:

```cpp
// Set proportional bias for Happy Hare
if (info.type == AmsType::HAPPY_HARE && info.sync_feedback_bias > -1.5f) {
    ui_filament_path_canvas_set_buffer_bias(path_canvas_, info.sync_feedback_bias);
} else {
    ui_filament_path_canvas_set_buffer_bias(path_canvas_, -2.0f); // discrete mode
}
```

**Step 5: Build and verify**

Run: `make -j`
Expected: Compiles cleanly. Visual change only testable in running app with HH mock data.

**Step 6: Commit**

```bash
git add src/ui/ui_filament_path_canvas.cpp include/ui_filament_path_canvas.h src/ui/ui_ams_detail.cpp
git commit -m "feat(ams): proportional color mapping for buffer box on canvas"
```

---

### Task 4: Enhance buffer click modal — Happy Hare

**Files:**
- Modify: `src/ui/ui_panel_ams.cpp:1146-1171`

**Step 1: Update the Happy Hare section of `handle_buffer_click()`**

Replace the Happy Hare block (lines 1146-1171) with:

```cpp
    } else if (info.type == AmsType::HAPPY_HARE) {
        title = lv_tr("Sync Feedback");

        // Proportional bias display
        if (info.sync_feedback_bias > -1.5f) {
            float abs_bias = std::fabs(info.sync_feedback_bias);
            int pct = static_cast<int>(abs_bias * 100.0f);
            const char* direction;
            if (abs_bias < 0.02f) {
                direction = "N"; // i18n: do not translate (Neutral indicator)
            } else if (info.sync_feedback_bias < 0) {
                direction = "T"; // i18n: do not translate (Tension indicator)
            } else {
                direction = "C"; // i18n: do not translate (Compression indicator)
            }
            message += fmt::format("{}: {} {}%\n", lv_tr("Buffer Position"),
                                   direction, pct);
            message += fmt::format("{}: {:.3f}\n", lv_tr("Bias (modelled)"),
                                   info.sync_feedback_bias);
        }

        // Existing fields
        if (!info.sync_feedback_state.empty() && info.sync_feedback_state != "disabled") {
            message += fmt::format("{}: {}\n", lv_tr("Sync Feedback"),
                                   info.sync_feedback_state);
        }
        if (!info.espooler_state.empty()) {
            message += fmt::format("{}: {}\n", lv_tr("eSpooler"), info.espooler_state);
        }
        message += fmt::format("{}: {}\n", lv_tr("Sync Drive"),
                               info.sync_drive ? lv_tr("Active") : lv_tr("Inactive"));
        if (info.clog_detection > 0) {
            message += fmt::format("{}: {}\n", lv_tr("Clog Detection"),
                                   info.clog_detection == 2 ? lv_tr("Auto") : lv_tr("Manual"));
        }
        if (info.sync_feedback_flow_rate >= 0) {
            message += fmt::format("{}: {:.0f}%", lv_tr("Flow Rate"),
                                   info.sync_feedback_flow_rate);
        } else if (info.encoder_flow_rate >= 0) {
            message += fmt::format("{}: {}%", lv_tr("Flow Rate"), info.encoder_flow_rate);
        }

        // Trim trailing newline
        if (!message.empty() && message.back() == '\n') {
            message.pop_back();
        }
        if (message.empty()) {
            message = lv_tr("No sync feedback data available");
        }
```

Note: Applying [L070] — T/C/N direction indicators are not wrapped in `lv_tr()` since they are universal abbreviations.

**Step 2: Build and verify**

Run: `make -j`

**Step 3: Commit**

```bash
git add src/ui/ui_panel_ams.cpp
git commit -m "feat(ams): show proportional bias in Happy Hare buffer click modal"
```

---

### Task 5: Enhance buffer click modal — AFC

**Files:**
- Modify: `src/ui/ui_panel_ams.cpp:1126-1145`

**Step 1: Update the AFC section of `handle_buffer_click()`**

Replace the AFC block with enhanced distance-to-fault display:

```cpp
    if (info.type == AmsType::AFC) {
        // AFC: Show buffer health from the unit
        bool found_health = false;
        if (effective_unit >= 0 && effective_unit < static_cast<int>(info.units.size())) {
            const auto& unit = info.units[effective_unit];
            if (unit.buffer_health.has_value()) {
                const auto& bh = unit.buffer_health.value();
                message += fmt::format("{}: {}\n", lv_tr("State"),
                                       bh.state.empty() ? lv_tr("Unknown") : bh.state.c_str());
                if (bh.fault_detection_enabled) {
                    if (bh.distance_to_fault >= 0) {
                        message += fmt::format("{}: {:.1f} mm\n", lv_tr("Distance to Fault"),
                                               bh.distance_to_fault);
                        message += fmt::format("  {}\n",
                                               lv_tr("(extrusion remaining before clog fault triggers)"));
                    }
                    message += fmt::format("{}: {}", lv_tr("Fault Detection"), lv_tr("Enabled"));
                } else {
                    message += fmt::format("{}: {}", lv_tr("Fault Detection"), lv_tr("Disabled"));
                }
                found_health = true;
            }
        }
        if (!found_health) {
            message = lv_tr("No buffer data available");
        }
```

**Step 2: Build and verify**

Run: `make -j`

**Step 3: Commit**

```bash
git add src/ui/ui_panel_ams.cpp
git commit -m "feat(ams): enhance AFC buffer modal with distance-to-fault explanation"
```

---

### Task 6: Create UiBufferMeter drawing component

**Files:**
- Create: `include/ui_buffer_meter.h`
- Create: `src/ui/ui_buffer_meter.cpp`

**Context:** This is the sliding-rectangle visualization. Follows `UiClogMeter` pattern: takes a parent `lv_obj_t*`, finds named children from XML, draws custom content. The meter renders two nested rectangles whose overlap varies with the bias value.

**Step 1: Create header**

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace helix::ui {

/**
 * @brief Sliding-rectangle buffer meter for proportional sync feedback.
 *
 * Draws two nested rectangles representing the physical buffer plunger:
 * - Neutral (bias≈0): 50% overlap
 * - Tension (bias<0): inner slides up, minimal overlap
 * - Compression (bias>0): inner slides down, near-complete overlap
 *
 * Happy Hare only — AFC has no proportional sensor data.
 */
class UiBufferMeter {
  public:
    explicit UiBufferMeter(lv_obj_t* parent);
    ~UiBufferMeter();

    UiBufferMeter(const UiBufferMeter&) = delete;
    UiBufferMeter& operator=(const UiBufferMeter&) = delete;

    [[nodiscard]] lv_obj_t* get_root() const { return root_; }
    [[nodiscard]] bool is_valid() const { return root_ != nullptr; }

    /// Set bias value directly (for modal usage without subject binding)
    void set_bias(float bias);

    /// Resize drawing to fit container
    void resize();

  private:
    static void on_draw(lv_event_t* e);
    static void on_size_changed(lv_event_t* e);

    void draw(lv_layer_t* layer);
    void update_labels();

    lv_obj_t* root_ = nullptr;         // Container for the drawing
    lv_obj_t* canvas_obj_ = nullptr;    // The object we draw on
    lv_obj_t* direction_label_ = nullptr;
    lv_obj_t* pct_label_ = nullptr;
    lv_obj_t* neutral_label_ = nullptr;

    float bias_ = 0.0f;
};

} // namespace helix::ui
```

**Step 2: Create implementation**

Create `src/ui/ui_buffer_meter.cpp`. Key drawing logic:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_buffer_meter.h"

#include "theme_manager.h"

#include "lvgl/lvgl.h"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace helix::ui {

// Outer rectangle proportions relative to drawing area
constexpr float RECT_WIDTH_RATIO = 0.25f;   // Width of rectangles as fraction of area width
constexpr float RECT_HEIGHT_RATIO = 0.45f;  // Height of each rectangle as fraction of area height
constexpr int32_t MIN_RECT_W = 20;
constexpr int32_t MIN_RECT_H = 30;
constexpr int32_t RECT_RADIUS = 3;

// Filament line extends above and below the rectangles
constexpr float FILAMENT_EXTEND = 0.1f;

UiBufferMeter::UiBufferMeter(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[BufferMeter] NULL parent");
        return;
    }

    root_ = parent;

    // Create drawing object
    canvas_obj_ = lv_obj_create(root_);
    lv_obj_remove_style_all(canvas_obj_);
    lv_obj_set_size(canvas_obj_, LV_PCT(60), LV_PCT(80));
    lv_obj_align(canvas_obj_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(canvas_obj_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(canvas_obj_, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(canvas_obj_, on_draw, LV_EVENT_DRAW_MAIN, this);

    // Direction + percentage label (right side)
    direction_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(direction_label_, ui_theme_get_font("text_small"), 0);
    lv_obj_set_style_text_color(direction_label_, ui_theme_get_color("text_muted"), 0);
    lv_obj_align(direction_label_, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_label_set_text(direction_label_, "N 0%");

    // Neutral reference label
    neutral_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(neutral_label_, ui_theme_get_font("text_tiny"), 0);
    lv_obj_set_style_text_color(neutral_label_, ui_theme_get_color("text_muted"), 0);
    lv_obj_align(neutral_label_, LV_ALIGN_LEFT_MID, 4, 0);
    lv_label_set_text(neutral_label_, "—"); // Neutral marker

    // SIZE_CHANGED callback for responsive layout
    lv_obj_add_event_cb(root_, on_size_changed, LV_EVENT_SIZE_CHANGED, this);

    update_labels();
}

UiBufferMeter::~UiBufferMeter() {
    // Labels and canvas_obj_ are children of root_ — LVGL handles cleanup
}

void UiBufferMeter::set_bias(float bias) {
    bias_ = std::clamp(bias, -1.0f, 1.0f);
    update_labels();
    if (canvas_obj_)
        lv_obj_invalidate(canvas_obj_);
}

void UiBufferMeter::resize() {
    if (canvas_obj_)
        lv_obj_invalidate(canvas_obj_);
}

void UiBufferMeter::on_draw(lv_event_t* e) {
    auto* self = static_cast<UiBufferMeter*>(lv_event_get_user_data(e));
    if (!self) return;
    auto* layer = lv_event_get_layer(e);
    if (!layer) return;
    self->draw(layer);
}

void UiBufferMeter::on_size_changed(lv_event_t* e) {
    auto* self = static_cast<UiBufferMeter*>(lv_event_get_user_data(e));
    if (self) self->resize();
}

void UiBufferMeter::update_labels() {
    if (!direction_label_) return;

    float abs_bias = std::fabs(bias_);
    int pct = static_cast<int>(abs_bias * 100.0f);
    const char* dir = (abs_bias < 0.02f) ? "N" : (bias_ < 0 ? "T" : "C");

    char buf[16];
    lv_snprintf(buf, sizeof(buf), "%s %d%%", dir, pct);
    lv_label_set_text(direction_label_, buf);
}

void UiBufferMeter::draw(lv_layer_t* layer) {
    if (!canvas_obj_) return;

    lv_area_t area;
    lv_obj_get_coords(canvas_obj_, &area);
    int32_t w = lv_area_get_width(&area);
    int32_t h = lv_area_get_height(&area);
    int32_t cx = area.x1 + w / 2;
    int32_t cy = area.y1 + h / 2;

    // Rectangle dimensions
    int32_t rect_w = std::max(MIN_RECT_W, (int32_t)(w * RECT_WIDTH_RATIO));
    int32_t rect_h = std::max(MIN_RECT_H, (int32_t)(h * RECT_HEIGHT_RATIO));

    // Bias maps to vertical offset of inner rect from center
    // At bias=0 (neutral): inner centered on outer → 50% overlap
    // At bias=-1 (full tension): inner shifts up by rect_h/2 → minimal overlap
    // At bias=+1 (full compression): inner shifts down by rect_h/2 → full overlap
    // Note: negative bias = tension = UP, positive = compression = DOWN
    float max_offset = rect_h * 0.5f;
    int32_t inner_offset = (int32_t)(-bias_ * max_offset); // negate: tension=up=negative screen Y

    // Outer rectangle (stationary, centered)
    int32_t outer_top = cy - rect_h / 2;
    int32_t outer_bot = cy + rect_h / 2;

    // Inner rectangle (slides vertically)
    int32_t inner_cy = cy + inner_offset;
    int32_t inner_top = inner_cy - rect_h / 2;
    int32_t inner_bot = inner_cy + rect_h / 2;

    // Color based on abs(bias)
    float abs_bias = std::fabs(bias_);
    lv_color_t rect_color;
    if (abs_bias < 0.3f) {
        rect_color = lv_color_hex(0x22C55E); // Green
    } else if (abs_bias < 0.7f) {
        float t = (abs_bias - 0.3f) / 0.4f;
        // Blend green → orange
        uint8_t r = (uint8_t)(0x22 + t * (0xF5 - 0x22));
        uint8_t g = (uint8_t)(0xC5 + t * (0x9E - 0xC5));
        uint8_t b = (uint8_t)(0x5E + t * (0x0B - 0x5E));
        rect_color = lv_color_make(r, g, b);
    } else {
        float t = (abs_bias - 0.7f) / 0.3f;
        // Blend orange → red
        uint8_t r = (uint8_t)(0xF5 + t * (0xEF - 0xF5));
        uint8_t g = (uint8_t)(0x9E + t * (0x44 - 0x9E));
        uint8_t b = (uint8_t)(0x0B + t * (0x44 - 0x0B));
        rect_color = lv_color_make(r, g, b);
    }

    // Draw filament line (vertical, through both rectangles and extending beyond)
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(0x4ADE80); // Light green filament
    line_dsc.width = 3;
    line_dsc.round_start = true;
    line_dsc.round_end = true;
    int32_t fil_top = std::min(outer_top, inner_top) - (int32_t)(h * FILAMENT_EXTEND);
    int32_t fil_bot = std::max(outer_bot, inner_bot) + (int32_t)(h * FILAMENT_EXTEND);
    line_dsc.p1 = {cx, fil_top};
    line_dsc.p2 = {cx, fil_bot};
    lv_draw_line(layer, &line_dsc);

    // Draw outer rectangle (housing — semi-transparent border)
    lv_draw_border_dsc_t border_dsc;
    lv_draw_border_dsc_init(&border_dsc);
    border_dsc.color = rect_color;
    border_dsc.opa = LV_OPA_70;
    border_dsc.width = 2;
    border_dsc.radius = RECT_RADIUS;
    lv_area_t outer_area = {cx - rect_w / 2, outer_top, cx + rect_w / 2, outer_bot};
    lv_draw_border(layer, &border_dsc, &outer_area);

    // Outer fill (very subtle)
    lv_draw_fill_dsc_t fill_dsc;
    lv_draw_fill_dsc_init(&fill_dsc);
    fill_dsc.color = rect_color;
    fill_dsc.opa = LV_OPA_10;
    fill_dsc.radius = RECT_RADIUS;
    lv_draw_fill(layer, &fill_dsc, &outer_area);

    // Draw inner rectangle (plunger — solid fill)
    lv_area_t inner_area = {cx - rect_w / 2 + 3, inner_top, cx + rect_w / 2 - 3, inner_bot};
    fill_dsc.color = rect_color;
    fill_dsc.opa = LV_OPA_40;
    lv_draw_fill(layer, &fill_dsc, &inner_area);

    border_dsc.color = rect_color;
    border_dsc.opa = LV_OPA_COVER;
    border_dsc.width = 1;
    lv_draw_border(layer, &border_dsc, &inner_area);

    // Neutral reference line (dashed effect: short line segments)
    lv_draw_line_dsc_t neutral_dsc;
    lv_draw_line_dsc_init(&neutral_dsc);
    neutral_dsc.color = ui_theme_get_color("text_muted");
    neutral_dsc.width = 1;
    neutral_dsc.dash_gap = 3;
    neutral_dsc.dash_width = 3;
    neutral_dsc.p1 = {cx - rect_w / 2 - 6, cy};
    neutral_dsc.p2 = {cx + rect_w / 2 + 6, cy};
    lv_draw_line(layer, &neutral_dsc);
}

} // namespace helix::ui
```

**Step 3: Add to build system**

Check `Makefile` or CMakeLists to ensure `src/ui/ui_buffer_meter.cpp` is picked up. If using glob/wildcard for `src/ui/*.cpp`, it should be automatic. Otherwise add it.

**Step 4: Build and verify**

Run: `make -j`
Expected: Compiles cleanly.

**Step 5: Commit**

```bash
git add include/ui_buffer_meter.h src/ui/ui_buffer_meter.cpp
git commit -m "feat(ams): add UiBufferMeter sliding-rectangle visualization component"
```

---

### Task 7: Upgrade clog detection widget to carousel with buffer meter page

**Files:**
- Modify: `src/ui/panel_widgets/clog_detection_widget.h`
- Modify: `src/ui/panel_widgets/clog_detection_widget.cpp`
- Modify: `ui_xml/components/panel_widget_clog_detection.xml`

**Context:** The clog detection widget currently shows a single clog arc. We need to wrap it in a carousel and conditionally add a buffer meter page when Happy Hare sync feedback data is available.

Ref: Follow the FanStackWidget/TempStackWidget carousel pattern.

**Step 1: Update XML to include carousel container**

Replace `ui_xml/components/panel_widget_clog_detection.xml` contents. The key change: wrap the existing `clog_meter` view in a `ui_carousel`:

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<component>
  <!--
    Filament Health Panel Widget - Carousel with clog arc + buffer meter pages.

    Page 1 (clog): Arc meter for encoder/flowguard/AFC clog detection
    Page 2 (buffer): Sliding-rectangle buffer meter (Happy Hare only)

    Pages are conditionally added in C++ based on available data.
  -->
  <view name="panel_widget_clog_detection" extends="lv_obj"
        width="100%" height="100%" flex_grow="1"
        style_pad_all="#space_sm" style_pad_row="0" style_pad_column="0"
        flex_flow="column"
        style_flex_main_place="center" style_flex_cross_place="center"
        style_flex_track_place="center" scrollable="false">
    <ui_carousel name="filament_health_carousel" width="100%" height="100%"
                 wrap="true" show_indicators="true"/>
  </view>
</component>
```

**Step 2: Update widget header**

Add carousel support fields to `clog_detection_widget.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"

#include <memory>
#include <string>
#include <vector>

class ClogDetectionConfigModal;

namespace helix {
namespace ui {
class UiClogMeter;
class UiBufferMeter;
}

class ClogDetectionWidget : public PanelWidget {
  public:
    ClogDetectionWidget() = default;
    ~ClogDetectionWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override { return "clog_detection"; }

    bool has_edit_configure() const override { return true; }
    bool on_edit_configure() override;

  private:
    void apply_config();
    void build_carousel_pages();
    void update_buffer_meter();

    nlohmann::json config_;
    lv_obj_t* widget_obj_ = nullptr;
    lv_obj_t* carousel_ = nullptr;

    // Page 1: Clog arc
    lv_obj_t* clog_page_ = nullptr;
    std::unique_ptr<ui::UiClogMeter> clog_meter_;

    // Page 2: Buffer meter (Happy Hare only)
    lv_obj_t* buffer_page_ = nullptr;
    std::unique_ptr<ui::UiBufferMeter> buffer_meter_;

    std::unique_ptr<ClogDetectionConfigModal> config_modal_;
    ObserverGuard bias_obs_;
};

} // namespace helix
```

**Step 3: Update widget implementation**

Replace `clog_detection_widget.cpp` with carousel-aware implementation. The key logic:

- In `attach()`: find the carousel, call `build_carousel_pages()`
- `build_carousel_pages()`: always add clog arc page; conditionally add buffer meter page if HH sync feedback is available
- `update_buffer_meter()`: called by observer when bias changes
- `on_activate()`: refresh buffer data (bias may have changed while widget was off-screen)

Reference the existing `fan_stack_widget.cpp` pattern for carousel page creation and the freeze+drain+clean rebuild pattern.

The clog page needs to host the arc widget programmatically since it's now inside a carousel page rather than directly in the XML. Create the arc elements with `lv_xml_create()` or programmatically, matching the existing XML structure.

**Important:** The `clog_meter_mode` subject binding (which auto-hides the arc when mode=0) needs to work within the carousel page context. The clog page itself should be hidden/shown based on whether clog data is available, using C++ logic rather than XML binding (since pages are added dynamically).

**Step 4: Build and test**

Run: `make -j`
Test visually: `./build/bin/helix-screen --test -vv` — navigate to home panel, verify clog widget shows carousel dots when HH mock is active.

**Step 5: Commit**

```bash
git add src/ui/panel_widgets/clog_detection_widget.h src/ui/panel_widgets/clog_detection_widget.cpp ui_xml/components/panel_widget_clog_detection.xml
git commit -m "feat(ams): upgrade clog detection widget to filament health carousel"
```

---

### Task 8: Wire UiBufferMeter into buffer click modal (Happy Hare)

**Files:**
- Modify: `src/ui/ui_panel_ams.cpp:1146-1177`

**Context:** Currently the HH buffer click shows a text-only `modal_show_alert`. We want to upgrade this to a modal that embeds the `UiBufferMeter` visualization above the text info. For this initial version, we can use a custom modal with the buffer meter + text, or embed the meter in a standard modal's content area.

**Step 1: Create a helper function that builds a modal with buffer meter**

In `ui_panel_ams.cpp`, add a private helper (or free function):

```cpp
static void show_buffer_detail_modal(const AmsSystemInfo& info) {
    // Build the text message (same as Task 4)
    std::string message;
    // ... (reuse the text from Task 4's implementation)

    // For now, use modal_show_alert with the enhanced text.
    // The UiBufferMeter in the modal is a future enhancement —
    // the carousel widget already provides the graphical view.
    helix::ui::modal_show_alert(lv_tr("Sync Feedback"), message.c_str(),
                                ModalSeverity::Info);
}
```

**Note:** A full custom modal with embedded `UiBufferMeter` drawing requires creating a new modal XML component and Modal subclass. This is significant additional work. For the initial implementation, the text-based modal with bias/percentage data (from Task 4) is sufficient — users get the graphical view from the dashboard widget carousel. The custom modal can be a follow-up task.

**Step 2: Commit**

If no additional changes beyond Task 4, this task can be deferred or merged into Task 4.

---

### Task 9: Audit and fix existing buffer/clog logic

**Files:**
- Review: `src/printer/ams_backend_happy_hare.cpp` (sync_feedback parsing)
- Review: `src/ui/ui_ams_detail.cpp` (buffer state mapping)
- Review: `src/ui/panel_widgets/clog_detection_widget.cpp` (source selection)
- Review: `src/ui/ui_filament_path_canvas.cpp` (buffer hit detection)

**Step 1: Verify HH sync_feedback parsing against current HH API**

Read `ams_backend_happy_hare.cpp` lines 560-650. Cross-reference with Happy Hare source at `/home/pbrown/Code/Printing/Happy-Hare/extras/mmu/mmu_sync_feedback_manager.py` `get_status()` (lines 398-407).

Check that:
- `sync_feedback_state` is parsed from top-level (not nested) ✓
- `sync_feedback.flow_rate` is parsed from nested `sync_feedback` dict — but HH exposes `sync_feedback_flow_rate` at top-level too. Verify which one we should use.
- `flowguard` is parsed as nested dict ✓
- `encoder` is parsed as nested dict ✓
- NEW: `sync_feedback_bias_modelled` and `sync_feedback_bias_raw` are at top-level (not nested)

**Step 2: Verify AFC buffer health parsing**

Read `ui_ams_detail.cpp` buffer sections. Cross-reference with AFC source at `/home/pbrown/Code/Printing/AFC-Klipper-Add-On/extras/AFC_buffer.py` `get_status()`.

Check that:
- `distance_to_fault` can be null (when fault detection disabled or no active tracking)
- `state` is "Trailing" or "Advancing" (not lowercase)
- `fault_detection_enabled` is a bool

**Step 3: Fix any discrepancies found**

**Step 4: Run full test suite**

Run: `make test-run`
Expected: All existing tests pass.

**Step 5: Commit any fixes**

```bash
git commit -m "fix(ams): audit and correct buffer/clog data parsing"
```

---

### Task 10: Integration test — full visual verification

**Step 1: Start the app with HH mock**

```bash
./build/bin/helix-screen --test -vv 2>&1 | tee /tmp/test.log
```

(Use `run_in_background: true` with Bash tool)

**Step 2: User verifies:**
- Navigate to AMS panel → click buffer coil → verify modal shows bias percentage and direction
- Navigate to home panel → verify filament health widget shows carousel with clog arc + buffer meter pages
- Check buffer box color on canvas changes based on mock bias value
- Switch between carousel pages with swipe

**Step 3: Read logs**

Check `/tmp/test.log` for any errors or warnings related to buffer meter, carousel, or sync feedback parsing.

**Step 4: Final commit if any tweaks needed**

---

## Summary of Commits

1. `feat(ams): add sync_feedback_bias fields to AmsSystemInfo`
2. `feat(ams): parse sync_feedback_bias_modelled from Happy Hare`
3. `feat(ams): proportional color mapping for buffer box on canvas`
4. `feat(ams): show proportional bias in Happy Hare buffer click modal`
5. `feat(ams): enhance AFC buffer modal with distance-to-fault explanation`
6. `feat(ams): add UiBufferMeter sliding-rectangle visualization component`
7. `feat(ams): upgrade clog detection widget to filament health carousel`
8. (deferred or merged with #4 — custom modal with embedded meter)
9. `fix(ams): audit and correct buffer/clog data parsing`
10. Visual verification (no commit unless tweaks needed)
