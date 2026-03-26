# Brother PT (P-Touch) Label Printer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Brother PT-series (P-Touch Cube) Bluetooth label printer support for spool labeling.

**Architecture:** New protocol module (`brother_pt_protocol`) handles PTCBP raster encoding for 128-pin PT printers (16 bytes/row, 180 dpi, TZe tape). New BT printer class (`BrotherPTBluetoothPrinter`) auto-detects loaded tape via RFCOMM status query, then renders and prints. Dispatch integrated into existing `label_printer_utils.cpp`.

**Tech Stack:** C++17, LVGL, Catch2, Bluetooth RFCOMM via BT plugin

**Spec:** `docs/plans/2026-03-26-brother-pt-label-printer-design.md`

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `include/brother_pt_protocol.h` | Create | PT protocol types, pure-function API |
| `src/system/brother_pt_protocol.cpp` | Create | PTCBP raster builder, PackBits compression, status parsing, tape info lookup |
| `tests/unit/test_brother_pt_protocol.cpp` | Create | Unit tests for protocol functions |
| `include/brother_pt_bt_printer.h` | Create | BT printer class header |
| `src/system/brother_pt_bt_printer.cpp` | Create | RFCOMM print flow with auto-detect |
| `include/bt_discovery_utils.h` | Modify | Add `is_brother_pt_printer()` helper |
| `src/system/label_printer_utils.cpp` | Modify | PT size selection + print dispatch |

Source files in `src/system/` and headers in `include/` are auto-discovered by the Makefile wildcard — no Makefile edits needed.

---

### Task 1: Protocol Types and Tape Info

**Files:**
- Create: `include/brother_pt_protocol.h`
- Create: `src/system/brother_pt_protocol.cpp`
- Test: `tests/unit/test_brother_pt_protocol.cpp`

- [ ] **Step 1: Write the tape info test**

```cpp
// tests/unit/test_brother_pt_protocol.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../catch_amalgamated.hpp"
#include "brother_pt_protocol.h"

using namespace helix::label;

TEST_CASE("Brother PT tape info - valid widths", "[label][brother-pt]") {
    // 3.5mm tape (status byte = 4)
    auto* info = brother_pt_get_tape_info(4);
    REQUIRE(info != nullptr);
    REQUIRE(info->printable_pins == 24);
    REQUIRE(info->left_margin_pins == 52);

    // 12mm tape
    info = brother_pt_get_tape_info(12);
    REQUIRE(info != nullptr);
    REQUIRE(info->printable_pins == 70);
    REQUIRE(info->left_margin_pins == 29);

    // 24mm tape
    info = brother_pt_get_tape_info(24);
    REQUIRE(info != nullptr);
    REQUIRE(info->printable_pins == 128);
    REQUIRE(info->left_margin_pins == 0);
}

TEST_CASE("Brother PT tape info - invalid width", "[label][brother-pt]") {
    REQUIRE(brother_pt_get_tape_info(0) == nullptr);
    REQUIRE(brother_pt_get_tape_info(15) == nullptr);
    REQUIRE(brother_pt_get_tape_info(36) == nullptr);  // 256-pin model, not supported
}

TEST_CASE("Brother PT label size for tape", "[label][brother-pt]") {
    auto size = brother_pt_label_size_for_tape(12);
    REQUIRE(size.has_value());
    REQUIRE(size->width_px == 70);
    REQUIRE(size->height_px == 0);  // continuous
    REQUIRE(size->dpi == 180);
    REQUIRE(size->width_mm == 12);

    // Invalid width
    REQUIRE_FALSE(brother_pt_label_size_for_tape(15).has_value());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "[brother-pt]" -v`
Expected: Compilation error — `brother_pt_protocol.h` not found

- [ ] **Step 3: Write the header and implementation**

```cpp
// include/brother_pt_protocol.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_printer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace helix::label {

/// PT raster row: 128 pins = 16 bytes
static constexpr int BROTHER_PT_RASTER_ROW_BYTES = 16;

/// Parsed status from a Brother PT printer (32-byte response)
struct BrotherPTMedia {
    uint8_t media_type = 0;    ///< 0x01=laminated, 0x03=non-laminated, 0x11=heat-shrink
    uint8_t width_mm = 0;      ///< Raw status byte: 0=none, 4=3.5mm, 6, 9, 12, 18, 24
    uint8_t error_info_1 = 0;  ///< Byte 8: 0x01=no media, 0x04=cutter jam, 0x08=weak battery
    uint8_t error_info_2 = 0;  ///< Byte 9: 0x01=wrong media, 0x10=cover open, 0x20=overheating
    uint8_t status_type = 0;   ///< Byte 18: 0x00=ready, 0x01=complete, 0x02=error
    bool valid = false;
};

/// Tape geometry for a given width (128-pin models only)
struct BrotherPTTapeInfo {
    int width_mm;           ///< Status byte value (4 means 3.5mm physical)
    int printable_pins;     ///< Number of usable pins for this tape
    int left_margin_pins;   ///< Zero-fill pins before printable area
};

/// Get tape geometry for a status-byte width, or nullptr if unsupported.
const BrotherPTTapeInfo* brother_pt_get_tape_info(int width_mm);

/// Create a LabelSize suitable for the renderer from detected tape width.
std::optional<LabelSize> brother_pt_label_size_for_tape(int width_mm);

/// Build a status request command (invalidate + init + ESC i S)
std::vector<uint8_t> brother_pt_build_status_request();

/// Parse a 32-byte status response
BrotherPTMedia brother_pt_parse_status(const uint8_t* data, size_t len);

/// Human-readable error string, or empty if no error
std::string brother_pt_error_string(const BrotherPTMedia& media);

/// Build complete PTCBP raster command sequence.
/// The bitmap should be the label content at 180 dpi (not yet rotated/padded).
/// This function handles rotation, flip, centering, and PackBits compression.
std::vector<uint8_t> brother_pt_build_raster(const LabelBitmap& bitmap,
                                              int tape_width_mm);

}  // namespace helix::label
```

```cpp
// src/system/brother_pt_protocol.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "brother_pt_protocol.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace helix::label {

// 128-pin print head tape geometry table
static constexpr std::array<BrotherPTTapeInfo, 6> TAPE_TABLE = {{
    {4,  24, 52},   // 3.5mm (status byte = 4)
    {6,  32, 48},   // 6mm
    {9,  50, 39},   // 9mm
    {12, 70, 29},   // 12mm
    {18, 112, 8},   // 18mm
    {24, 128, 0},   // 24mm
}};

const BrotherPTTapeInfo* brother_pt_get_tape_info(int width_mm) {
    for (const auto& t : TAPE_TABLE) {
        if (t.width_mm == width_mm)
            return &t;
    }
    return nullptr;
}

std::optional<LabelSize> brother_pt_label_size_for_tape(int width_mm) {
    const auto* info = brother_pt_get_tape_info(width_mm);
    if (!info)
        return std::nullopt;
    // Physical width string (status byte 4 → "3.5mm", others are just Nmm)
    std::string name = (width_mm == 4) ? "3.5mm" : std::to_string(width_mm) + "mm";
    return LabelSize{name, info->printable_pins, 0, 180,
                     0x01,  // media_type: laminated TZe (default)
                     static_cast<uint8_t>(width_mm), 0};
}

}  // namespace helix::label
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test && ./build/bin/helix-tests "[brother-pt]" -v`
Expected: All 3 tests PASS

- [ ] **Step 5: Commit**

```bash
git add include/brother_pt_protocol.h src/system/brother_pt_protocol.cpp tests/unit/test_brother_pt_protocol.cpp
git commit -m "feat(label): add Brother PT protocol types and tape info lookup"
```

---

### Task 2: Status Request/Parse and Error Strings

**Files:**
- Modify: `src/system/brother_pt_protocol.cpp`
- Modify: `tests/unit/test_brother_pt_protocol.cpp`

- [ ] **Step 1: Write status parsing tests**

```cpp
TEST_CASE("Brother PT status request command", "[label][brother-pt]") {
    auto cmd = brother_pt_build_status_request();
    // 100 bytes invalidation + ESC @ (2) + ESC i S (3) = 105
    REQUIRE(cmd.size() == 105);
    // First 100 bytes are 0x00
    for (int i = 0; i < 100; i++) {
        REQUIRE(cmd[i] == 0x00);
    }
    // ESC @
    REQUIRE(cmd[100] == 0x1B);
    REQUIRE(cmd[101] == 0x40);
    // ESC i S
    REQUIRE(cmd[102] == 0x1B);
    REQUIRE(cmd[103] == 0x69);
    REQUIRE(cmd[104] == 0x53);
}

TEST_CASE("Brother PT parse status - valid 12mm laminated", "[label][brother-pt]") {
    uint8_t response[32] = {};
    response[0] = 0x80;   // print head mark
    response[10] = 12;    // tape width mm
    response[11] = 0x01;  // laminated
    response[18] = 0x00;  // status: ready

    auto media = brother_pt_parse_status(response, 32);
    REQUIRE(media.valid);
    REQUIRE(media.width_mm == 12);
    REQUIRE(media.media_type == 0x01);
    REQUIRE(media.status_type == 0x00);
    REQUIRE(media.error_info_1 == 0);
    REQUIRE(media.error_info_2 == 0);
}

TEST_CASE("Brother PT parse status - no tape", "[label][brother-pt]") {
    uint8_t response[32] = {};
    response[0] = 0x80;
    response[8] = 0x01;   // error: no media
    response[10] = 0;     // no tape
    response[18] = 0x02;  // error status

    auto media = brother_pt_parse_status(response, 32);
    REQUIRE(media.valid);
    REQUIRE(media.width_mm == 0);
    REQUIRE(media.error_info_1 == 0x01);
    REQUIRE(media.status_type == 0x02);
}

TEST_CASE("Brother PT parse status - truncated data", "[label][brother-pt]") {
    uint8_t response[10] = {};
    auto media = brother_pt_parse_status(response, 10);
    REQUIRE_FALSE(media.valid);
}

TEST_CASE("Brother PT parse status - wrong header", "[label][brother-pt]") {
    uint8_t response[32] = {};
    response[0] = 0x00;  // wrong print head mark
    auto media = brother_pt_parse_status(response, 32);
    REQUIRE_FALSE(media.valid);
}

TEST_CASE("Brother PT error string", "[label][brother-pt]") {
    BrotherPTMedia media{};
    media.valid = true;

    // No error
    REQUIRE(brother_pt_error_string(media).empty());

    // No media
    media.error_info_1 = 0x01;
    REQUIRE_FALSE(brother_pt_error_string(media).empty());
    REQUIRE(brother_pt_error_string(media).find("media") != std::string::npos);

    // Cover open
    media.error_info_1 = 0;
    media.error_info_2 = 0x10;
    REQUIRE(brother_pt_error_string(media).find("cover") != std::string::npos);

    // Weak battery
    media.error_info_2 = 0;
    media.error_info_1 = 0x08;
    REQUIRE(brother_pt_error_string(media).find("battery") != std::string::npos);

    // Cutter jam
    media.error_info_1 = 0x04;
    REQUIRE(brother_pt_error_string(media).find("cutter") != std::string::npos);

    // Overheating
    media.error_info_1 = 0;
    media.error_info_2 = 0x20;
    REQUIRE(brother_pt_error_string(media).find("over") != std::string::npos);

    // Wrong media
    media.error_info_2 = 0x01;
    REQUIRE(brother_pt_error_string(media).find("media") != std::string::npos);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[brother-pt]" -v`
Expected: Link errors — functions not defined

- [ ] **Step 3: Implement status functions**

Add to `src/system/brother_pt_protocol.cpp`:

```cpp
std::vector<uint8_t> brother_pt_build_status_request() {
    std::vector<uint8_t> cmd;
    // 1. Invalidate — 100 bytes of 0x00 (PT uses fewer than QL's 200)
    cmd.insert(cmd.end(), 100, 0x00);
    // 2. Initialize — ESC @
    cmd.push_back(0x1B);
    cmd.push_back(0x40);
    // 3. Request status — ESC i S
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x53);
    return cmd;
}

BrotherPTMedia brother_pt_parse_status(const uint8_t* data, size_t len) {
    BrotherPTMedia media{};
    if (!data || len < 32)
        return media;
    if (data[0] != 0x80)
        return media;
    media.error_info_1 = data[8];
    media.error_info_2 = data[9];
    media.width_mm = data[10];
    media.media_type = data[11];
    media.status_type = data[18];
    media.valid = true;
    return media;
}

std::string brother_pt_error_string(const BrotherPTMedia& media) {
    if (media.error_info_1 & 0x01) return "No media installed";
    if (media.error_info_1 & 0x04) return "Cutter jam";
    if (media.error_info_1 & 0x08) return "Weak battery";
    if (media.error_info_2 & 0x01) return "Wrong media";
    if (media.error_info_2 & 0x10) return "Cover open";
    if (media.error_info_2 & 0x20) return "Overheating";
    return "";
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[brother-pt]" -v`
Expected: All status tests PASS

- [ ] **Step 5: Commit**

```bash
git add src/system/brother_pt_protocol.cpp tests/unit/test_brother_pt_protocol.cpp
git commit -m "feat(label): add Brother PT status request/parse and error strings"
```

---

### Task 3: PackBits Compression

**Files:**
- Modify: `src/system/brother_pt_protocol.cpp`
- Modify: `tests/unit/test_brother_pt_protocol.cpp`

- [ ] **Step 1: Write PackBits tests**

```cpp
TEST_CASE("Brother PT PackBits - all zeros", "[label][brother-pt]") {
    // 16 bytes of 0x00 should compress to a repeat run
    std::vector<uint8_t> row(16, 0x00);
    auto compressed = brother_pt_packbits_compress(row.data(), row.size());
    // Repeat 16 of 0x00: control byte = -(16-1) = -15 = 0xF1, data = 0x00
    REQUIRE(compressed.size() == 2);
    REQUIRE(compressed[0] == 0xF1);
    REQUIRE(compressed[1] == 0x00);
}

TEST_CASE("Brother PT PackBits - all unique bytes", "[label][brother-pt]") {
    std::vector<uint8_t> row;
    for (int i = 0; i < 16; i++) row.push_back(static_cast<uint8_t>(i));
    auto compressed = brother_pt_packbits_compress(row.data(), row.size());
    // Literal run: control = 16-1 = 15, then 16 data bytes
    REQUIRE(compressed.size() == 17);
    REQUIRE(compressed[0] == 15);
}

TEST_CASE("Brother PT PackBits - mixed run", "[label][brother-pt]") {
    // 4 unique bytes + 12 repeated 0xFF
    std::vector<uint8_t> row = {0x01, 0x02, 0x03, 0x04};
    row.insert(row.end(), 12, 0xFF);
    auto compressed = brother_pt_packbits_compress(row.data(), row.size());
    // Should have literal run for 4 bytes + repeat run for 12 bytes
    REQUIRE(compressed.size() > 2);
    REQUIRE(compressed.size() < 16);  // must actually compress
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[brother-pt]" -v`
Expected: Link error — `brother_pt_packbits_compress` not found

- [ ] **Step 3: Implement PackBits compression**

Add declaration to `include/brother_pt_protocol.h`:

```cpp
/// TIFF PackBits compression for a single raster row.
/// Input: raw row bytes. Output: compressed bytes.
std::vector<uint8_t> brother_pt_packbits_compress(const uint8_t* data, size_t len);
```

Add implementation to `src/system/brother_pt_protocol.cpp`:

```cpp
std::vector<uint8_t> brother_pt_packbits_compress(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < len) {
        // Count repeated bytes
        size_t run = 1;
        while (i + run < len && run < 128 && data[i + run] == data[i])
            run++;

        if (run >= 2) {
            // Repeat run: control = -(run-1), then single byte
            out.push_back(static_cast<uint8_t>(-(static_cast<int>(run) - 1)));
            out.push_back(data[i]);
            i += run;
        } else {
            // Literal run: collect non-repeating bytes
            size_t lit_start = i;
            size_t lit_len = 1;
            i++;
            while (i < len && lit_len < 128) {
                // Check if next starts a repeat of 2+
                if (i + 1 < len && data[i] == data[i + 1])
                    break;
                lit_len++;
                i++;
            }
            out.push_back(static_cast<uint8_t>(lit_len - 1));
            out.insert(out.end(), data + lit_start, data + lit_start + lit_len);
        }
    }
    return out;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[brother-pt]" -v`
Expected: All PackBits tests PASS

- [ ] **Step 5: Commit**

```bash
git add include/brother_pt_protocol.h src/system/brother_pt_protocol.cpp tests/unit/test_brother_pt_protocol.cpp
git commit -m "feat(label): add TIFF PackBits compression for Brother PT"
```

---

### Task 4: Raster Builder

**Files:**
- Modify: `src/system/brother_pt_protocol.cpp`
- Modify: `tests/unit/test_brother_pt_protocol.cpp`

- [ ] **Step 1: Write raster builder tests**

```cpp
TEST_CASE("Brother PT raster - invalidation header", "[label][brother-pt]") {
    LabelBitmap bitmap(70, 100);
    auto data = brother_pt_build_raster(bitmap, 12);

    // First 100 bytes are 0x00
    REQUIRE(data.size() > 100);
    for (int i = 0; i < 100; i++) {
        REQUIRE(data[i] == 0x00);
    }
}

TEST_CASE("Brother PT raster - init and raster mode", "[label][brother-pt]") {
    LabelBitmap bitmap(70, 100);
    auto data = brother_pt_build_raster(bitmap, 12);

    // ESC @ at offset 100
    REQUIRE(data[100] == 0x1B);
    REQUIRE(data[101] == 0x40);
    // ESC i a 01 at offset 102
    REQUIRE(data[102] == 0x1B);
    REQUIRE(data[103] == 0x69);
    REQUIRE(data[104] == 0x61);
    REQUIRE(data[105] == 0x01);
}

TEST_CASE("Brother PT raster - media info for 12mm", "[label][brother-pt]") {
    LabelBitmap bitmap(70, 100);
    auto data = brother_pt_build_raster(bitmap, 12);

    // ESC i z at offset 106
    REQUIRE(data[106] == 0x1B);
    REQUIRE(data[107] == 0x69);
    REQUIRE(data[108] == 0x7A);
    REQUIRE(data[110] == 0x01);  // media type (laminated TZe)
    REQUIRE(data[111] == 12);    // width mm
    REQUIRE(data[112] == 0);     // length mm (continuous)
}

TEST_CASE("Brother PT raster - compression enabled", "[label][brother-pt]") {
    LabelBitmap bitmap(70, 100);
    auto data = brother_pt_build_raster(bitmap, 12);

    // Find compression command: 4D 02 (TIFF PackBits)
    bool found = false;
    for (size_t i = 0; i + 1 < data.size(); i++) {
        if (data[i] == 0x4D && data[i + 1] == 0x02) {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("Brother PT raster - ends with print command", "[label][brother-pt]") {
    LabelBitmap bitmap(70, 100);
    auto data = brother_pt_build_raster(bitmap, 12);
    REQUIRE(data.back() == 0x1A);
}

TEST_CASE("Brother PT raster - blank rows use 5A", "[label][brother-pt]") {
    // All-white bitmap should produce 5A blank row markers
    LabelBitmap bitmap(70, 10);  // After 90° CW rotation: 10 wide, 70 tall → 70 raster rows
    auto data = brother_pt_build_raster(bitmap, 12);

    // Find compression command (4D 02) to skip past header
    size_t raster_start = 0;
    for (size_t i = 0; i + 1 < data.size(); i++) {
        if (data[i] == 0x4D && data[i + 1] == 0x02) {
            raster_start = i + 2;
            break;
        }
    }
    REQUIRE(raster_start > 0);

    // Count 5A bytes from raster_start to end-1 (last byte is 0x1A print cmd)
    int blank_count = 0;
    for (size_t i = raster_start; i < data.size() - 1; i++) {
        if (data[i] == 0x5A)
            blank_count++;
    }
    // After rotation: 70 raster rows, all blank
    REQUIRE(blank_count == 70);
}

TEST_CASE("Brother PT raster - deterministic output", "[label][brother-pt]") {
    LabelBitmap bitmap(70, 100);
    bitmap.set_pixel(10, 5, true);
    auto data1 = brother_pt_build_raster(bitmap, 12);
    auto data2 = brother_pt_build_raster(bitmap, 12);
    REQUIRE(data1 == data2);
}

TEST_CASE("Brother PT raster - invalid tape width returns empty", "[label][brother-pt]") {
    LabelBitmap bitmap(70, 100);
    auto data = brother_pt_build_raster(bitmap, 15);  // unsupported
    REQUIRE(data.empty());
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make test && ./build/bin/helix-tests "[brother-pt]" -v`
Expected: Link error — `brother_pt_build_raster` not defined

- [ ] **Step 3: Implement the raster builder**

Add to `src/system/brother_pt_protocol.cpp`:

```cpp
std::vector<uint8_t> brother_pt_build_raster(const LabelBitmap& bitmap,
                                              int tape_width_mm) {
    const auto* tape = brother_pt_get_tape_info(tape_width_mm);
    if (!tape)
        return {};

    // Image preparation: rotate 90° CW, then horizontal flip
    // After rotation: original width becomes height, original height becomes width
    auto rotated = bitmap.rotate_90_cw();

    // Horizontal flip each row (print head orientation — verify against hardware)
    int rot_w = rotated.width();
    int rot_h = rotated.height();

    std::vector<uint8_t> cmd;
    cmd.reserve(256 + static_cast<size_t>(rot_h) * (3 + BROTHER_PT_RASTER_ROW_BYTES) + 1);

    // 1. Invalidate — 100 bytes of 0x00
    cmd.insert(cmd.end(), 100, 0x00);

    // 2. Initialize — ESC @
    cmd.push_back(0x1B);
    cmd.push_back(0x40);

    // 3. Enter raster mode — ESC i a 01
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x61);
    cmd.push_back(0x01);

    // 4. Print info — ESC i z
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x7A);
    cmd.push_back(0x86);  // valid flags: media type + width + quality
    cmd.push_back(0x01);  // media type: laminated TZe
    cmd.push_back(static_cast<uint8_t>(tape_width_mm));
    cmd.push_back(0x00);  // length (continuous)
    // Raster line count = number of rows after rotation (LE 32-bit)
    cmd.push_back(static_cast<uint8_t>(rot_h & 0xFF));
    cmd.push_back(static_cast<uint8_t>((rot_h >> 8) & 0xFF));
    cmd.push_back(static_cast<uint8_t>((rot_h >> 16) & 0xFF));
    cmd.push_back(static_cast<uint8_t>((rot_h >> 24) & 0xFF));
    cmd.push_back(0x00);  // page number
    cmd.push_back(0x00);  // reserved

    // 5. Auto-cut — ESC i M
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x4D);
    cmd.push_back(0x40);  // auto-cut on

    // 6. Advanced mode — ESC i K (no chain printing)
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x4B);
    cmd.push_back(0x08);  // bit 3 = no chain printing

    // 7. Margin — ESC i d (14 dots = 2mm at 180dpi)
    cmd.push_back(0x1B);
    cmd.push_back(0x69);
    cmd.push_back(0x64);
    cmd.push_back(0x0E);  // 14 low byte
    cmd.push_back(0x00);  // 14 high byte

    // 8. Compression — M 02 (TIFF PackBits)
    cmd.push_back(0x4D);
    cmd.push_back(0x02);

    // 9. Raster rows
    // Each row must be 16 bytes (128 pins), zero-padded with margins.
    // Margins are pin-precise (not byte-aligned) for correct centering.
    std::vector<uint8_t> padded_row(BROTHER_PT_RASTER_ROW_BYTES, 0x00);
    int margin_pins = tape->left_margin_pins;

    for (int y = 0; y < rot_h; y++) {
        const uint8_t* row = rotated.row_data(y);
        int row_bytes = rotated.row_byte_width();

        // Build padded 16-byte row: [margin zeros] [data] [trailing zeros]
        std::fill(padded_row.begin(), padded_row.end(), 0x00);

        // Horizontal flip the printable portion into the padded row
        // with pin-precise margin offset
        for (int x = 0; x < tape->printable_pins && x < rot_w; x++) {
            int src_byte = x / 8;
            int src_bit = 7 - (x % 8);
            if (src_byte < row_bytes && (row[src_byte] & (1 << src_bit))) {
                int dst_x = tape->printable_pins - 1 - x;
                int dst_pin = margin_pins + dst_x;
                int dst_byte = dst_pin / 8;
                int dst_bit = 7 - (dst_pin % 8);
                if (dst_byte < BROTHER_PT_RASTER_ROW_BYTES)
                    padded_row[dst_byte] |= (1 << dst_bit);
            }
        }

        // Check if all zeros
        bool all_white = true;
        for (int b = 0; b < BROTHER_PT_RASTER_ROW_BYTES; b++) {
            if (padded_row[b] != 0x00) {
                all_white = false;
                break;
            }
        }

        if (all_white) {
            cmd.push_back(0x5A);
        } else {
            auto compressed = brother_pt_packbits_compress(padded_row.data(),
                                                            BROTHER_PT_RASTER_ROW_BYTES);
            cmd.push_back(0x47);
            cmd.push_back(static_cast<uint8_t>(compressed.size() & 0xFF));
            cmd.push_back(static_cast<uint8_t>((compressed.size() >> 8) & 0xFF));
            cmd.insert(cmd.end(), compressed.begin(), compressed.end());
        }
    }

    // 10. Print + feed
    cmd.push_back(0x1A);

    return cmd;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `make test && ./build/bin/helix-tests "[brother-pt]" -v`
Expected: All raster tests PASS

- [ ] **Step 5: Commit**

```bash
git add include/brother_pt_protocol.h src/system/brother_pt_protocol.cpp tests/unit/test_brother_pt_protocol.cpp
git commit -m "feat(label): add Brother PT PTCBP raster builder with PackBits compression"
```

---

### Task 5: Discovery Helper

**Files:**
- Modify: `include/bt_discovery_utils.h`
- Modify: `tests/unit/test_bt_discovery_utils.cpp`

- [ ] **Step 1: Write discovery test**

Add to `tests/unit/test_bt_discovery_utils.cpp`:

```cpp
TEST_CASE("is_brother_pt_printer identifies PT models", "[bluetooth][discovery]") {
    REQUIRE(is_brother_pt_printer("PT-P300BT"));
    REQUIRE(is_brother_pt_printer("PT-P710BT"));
    REQUIRE(is_brother_pt_printer("pt-p300bt"));  // case insensitive
    REQUIRE(is_brother_pt_printer("PT-P900W"));

    REQUIRE_FALSE(is_brother_pt_printer("QL-820NWB"));
    REQUIRE_FALSE(is_brother_pt_printer(""));
    REQUIRE_FALSE(is_brother_pt_printer(nullptr));
    REQUIRE_FALSE(is_brother_pt_printer("Phomemo-M110"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make test && ./build/bin/helix-tests "is_brother_pt_printer" -v`
Expected: Compilation error — function not found

- [ ] **Step 3: Add helper to bt_discovery_utils.h**

Add after `is_brother_printer()`:

```cpp
/// Returns true if the device name matches a Brother PT-series (P-Touch) printer
inline bool is_brother_pt_printer(const char* name) {
    if (!name || !name[0]) return false;
    return strncasecmp(name, "PT-", 3) == 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make test && ./build/bin/helix-tests "is_brother_pt_printer" -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/bt_discovery_utils.h tests/unit/test_bt_discovery_utils.cpp
git commit -m "feat(bluetooth): add is_brother_pt_printer() discovery helper"
```

---

### Task 6: Bluetooth Printer Backend

**Files:**
- Create: `include/brother_pt_bt_printer.h`
- Create: `src/system/brother_pt_bt_printer.cpp`

- [ ] **Step 1: Create the header**

```cpp
// include/brother_pt_bt_printer.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "label_printer.h"

#include <string>
#include <vector>

struct SpoolInfo;
enum class LabelPreset;

namespace helix::label {

/// Brother PT-series (P-Touch) Bluetooth label printer.
/// Uses RFCOMM transport with auto-detected tape width from status query.
class BrotherPTBluetoothPrinter : public ILabelPrinter {
  public:
    void set_device(const std::string& mac, int channel = 1);

    [[nodiscard]] std::string name() const override;
    void print(const LabelBitmap& bitmap, const LabelSize& size,
               PrintCallback callback) override;
    [[nodiscard]] std::vector<LabelSize> supported_sizes() const override;

    /// Deferred-render print: queries tape, renders at correct size, prints.
    void print_spool(const SpoolInfo& spool, LabelPreset preset,
                     PrintCallback callback);

    /// All supported TZe tape sizes (for settings UI)
    static std::vector<LabelSize> supported_sizes_static();

  private:
    std::string mac_;
    int channel_ = 1;
};

}  // namespace helix::label
```

- [ ] **Step 2: Create the implementation**

```cpp
// src/system/brother_pt_bt_printer.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "brother_pt_bt_printer.h"
#include "bluetooth_loader.h"
#include "brother_pt_protocol.h"
#include "bt_print_utils.h"
#include "label_renderer.h"
#include "spoolman_types.h"
#include "ui_update_queue.h"

#include <spdlog/spdlog.h>

#include <thread>

namespace helix::label {

void BrotherPTBluetoothPrinter::set_device(const std::string& mac, int channel) {
    mac_ = mac;
    channel_ = channel;
}

std::string BrotherPTBluetoothPrinter::name() const {
    return "Brother PT (Bluetooth)";
}

std::vector<LabelSize> BrotherPTBluetoothPrinter::supported_sizes() const {
    return supported_sizes_static();
}

std::vector<LabelSize> BrotherPTBluetoothPrinter::supported_sizes_static() {
    std::vector<LabelSize> sizes;
    // All 128-pin tape widths
    for (int w : {4, 6, 9, 12, 18, 24}) {
        auto s = brother_pt_label_size_for_tape(w);
        if (s) sizes.push_back(*s);
    }
    return sizes;
}

void BrotherPTBluetoothPrinter::print(const LabelBitmap& bitmap,
                                        const LabelSize& size,
                                        PrintCallback callback) {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available()) {
        spdlog::error("[Brother PT BT] Bluetooth not available");
        helix::ui::queue_update([callback]() {
            if (callback) callback(false, "Bluetooth not available");
        });
        return;
    }

    if (mac_.empty()) {
        spdlog::error("[Brother PT BT] No device configured");
        helix::ui::queue_update([callback]() {
            if (callback) callback(false, "Bluetooth device not configured");
        });
        return;
    }

    // Use the passed-in label size's width_mm to determine tape
    // (fallback path when called directly with pre-rendered bitmap)
    auto commands = brother_pt_build_raster(bitmap, size.width_mm);
    if (commands.empty()) {
        helix::ui::queue_update([callback]() {
            if (callback) callback(false, "Unsupported tape width");
        });
        return;
    }

    spdlog::info("[Brother PT BT] Sending {} bytes to {} ch{}",
                 commands.size(), mac_, channel_);

    std::string mac = mac_;
    int channel = channel_;

    std::thread([mac, channel, commands = std::move(commands), callback]() {
        // No local mutex needed — rfcomm_send serializes via s_rfcomm_mutex in bt_print_utils
        auto result = helix::bluetooth::rfcomm_send(mac, channel, commands, "Brother PT BT");
        helix::ui::queue_update([callback, result]() {
            if (callback) callback(result.success, result.error);
        });
    }).detach();
}

void BrotherPTBluetoothPrinter::print_spool(const SpoolInfo& spool,
                                              LabelPreset preset,
                                              PrintCallback callback) {
    auto& loader = helix::bluetooth::BluetoothLoader::instance();
    if (!loader.is_available()) {
        spdlog::error("[Brother PT BT] Bluetooth not available");
        helix::ui::queue_update([callback]() {
            if (callback) callback(false, "Bluetooth not available");
        });
        return;
    }

    if (mac_.empty()) {
        spdlog::error("[Brother PT BT] No device configured");
        helix::ui::queue_update([callback]() {
            if (callback) callback(false, "Bluetooth device not configured");
        });
        return;
    }

    std::string mac = mac_;
    int channel = channel_;

    std::thread([mac, channel, spool, preset, callback]() {
        // Step 1: Query status to detect loaded tape
        auto status_cmd = brother_pt_build_status_request();
        auto status_result = helix::bluetooth::rfcomm_send_receive(
            mac, channel, status_cmd, 32, "Brother PT BT status");

        if (!status_result.success) {
            helix::ui::queue_update([callback, err = status_result.error]() {
                if (callback) callback(false, "Status query failed: " + err);
            });
            return;
        }

        auto media = brother_pt_parse_status(status_result.response.data(),
                                              status_result.response.size());
        if (!media.valid) {
            helix::ui::queue_update([callback]() {
                if (callback) callback(false, "Invalid status response from printer");
            });
            return;
        }

        // Check for errors
        auto error = brother_pt_error_string(media);
        if (!error.empty()) {
            helix::ui::queue_update([callback, error]() {
                if (callback) callback(false, error);
            });
            return;
        }

        if (media.width_mm == 0) {
            helix::ui::queue_update([callback]() {
                if (callback) callback(false, "No tape detected in printer");
            });
            return;
        }

        // Step 2: Build label size from detected tape
        auto label_size = brother_pt_label_size_for_tape(media.width_mm);
        if (!label_size) {
            helix::ui::queue_update([callback, w = media.width_mm]() {
                if (callback)
                    callback(false, "Unsupported tape width: " + std::to_string(w) + "mm");
            });
            return;
        }

        spdlog::info("[Brother PT BT] Detected {}mm tape ({} printable pixels)",
                     label_size->name, label_size->width_px);

        // Step 3: Render label at detected tape dimensions
        auto actual_preset = preset;
        if (label_size->width_px <= 50) {
            actual_preset = LabelPreset::MINIMAL;
        }

        auto bitmap = helix::LabelRenderer::render(spool, actual_preset, *label_size);
        if (bitmap.empty()) {
            helix::ui::queue_update([callback]() {
                if (callback) callback(false, "Failed to render label");
            });
            return;
        }

        // Step 4: Build raster and send
        auto commands = brother_pt_build_raster(bitmap, media.width_mm);
        if (commands.empty()) {
            helix::ui::queue_update([callback]() {
                if (callback) callback(false, "Failed to build raster data");
            });
            return;
        }

        spdlog::info("[Brother PT BT] Sending {} bytes to {} ch{}",
                     commands.size(), mac, channel);

        auto result = helix::bluetooth::rfcomm_send(mac, channel, commands, "Brother PT BT");

        helix::ui::queue_update([callback, result]() {
            if (callback) callback(result.success, result.error);
        });
    }).detach();
}

}  // namespace helix::label
```

Note: The `print_spool()` method uses two separate RFCOMM connections — `rfcomm_send_receive` for status query, then `rfcomm_send` for raster. Both are serialized by `s_print_mutex` and internally by the shared RFCOMM mutex in `bt_print_utils.cpp`. This is simpler than direct BT context management and works because Brother printers don't require session continuity between status query and print.

- [ ] **Step 3: Verify it compiles**

Run: `make -j`
Expected: Clean compilation

- [ ] **Step 4: Commit**

```bash
git add include/brother_pt_bt_printer.h src/system/brother_pt_bt_printer.cpp
git commit -m "feat(label): add Brother PT Bluetooth printer backend with auto-detect"
```

---

### Task 7: Dispatch Integration

**Files:**
- Modify: `src/system/label_printer_utils.cpp`

- [ ] **Step 1: Add includes**

At the top of `label_printer_utils.cpp`, add:

```cpp
#include "brother_pt_bt_printer.h"
#include "brother_pt_protocol.h"
```

- [ ] **Step 2: Update size selection (around line 99)**

Change the Brother BT size selection from:

```cpp
if (helix::bluetooth::is_brother_printer(bt_name.c_str())) {
    sizes = BrotherQLPrinter::supported_sizes_static();
```

To:

```cpp
if (helix::bluetooth::is_brother_pt_printer(bt_name.c_str())) {
    sizes = helix::label::BrotherPTBluetoothPrinter::supported_sizes_static();
} else if (helix::bluetooth::is_brother_printer(bt_name.c_str())) {
    sizes = BrotherQLPrinter::supported_sizes_static();
```

- [ ] **Step 3: Update print dispatch (around line 179)**

Change the Brother BT print dispatch from:

```cpp
if (helix::bluetooth::is_brother_printer(bt_name.c_str())) {
    helix::label::BrotherQLBluetoothPrinter printer;
    printer.set_device(bt_address);
    printer.print(bitmap, label_size, callback);
```

To:

```cpp
if (helix::bluetooth::is_brother_pt_printer(bt_name.c_str())) {
    helix::label::BrotherPTBluetoothPrinter printer;
    printer.set_device(bt_address);
    printer.print_spool(spool, preset, callback);
} else if (helix::bluetooth::is_brother_printer(bt_name.c_str())) {
    helix::label::BrotherQLBluetoothPrinter printer;
    printer.set_device(bt_address);
    printer.print(bitmap, label_size, callback);
```

- [ ] **Step 4: Verify compilation**

Run: `make -j`
Expected: Clean compilation

- [ ] **Step 5: Commit**

```bash
git add src/system/label_printer_utils.cpp
git commit -m "feat(label): integrate Brother PT dispatch into label printer routing"
```

---

### Task 8: Integration Test with Physical Printer

**Files:** None (manual test)

- [ ] **Step 1: Build the app**

Run: `make -j`

- [ ] **Step 2: Configure the PT-P300BT**

Launch with `./build/bin/helix-screen --test -vv`, navigate to Settings → Label Printer → Bluetooth, pair and select the PT-P300BT.

- [ ] **Step 3: Print a test spool label**

Navigate to a spool in Spoolman, tap the print label button. Verify:
- Status query succeeds (check logs for "Detected Xmm tape")
- Label prints with correct orientation (text reads left-to-right along tape)
- Content fits within printable area
- Auto-cut works

- [ ] **Step 4: Test error handling**

Remove tape from the printer, attempt print. Verify the error callback shows "No media installed" or similar.

- [ ] **Step 5: If orientation is wrong**

If the label prints mirrored, remove the horizontal flip in `brother_pt_build_raster()` — the flip loop that reverses pixel X positions within each row. The spec flags this as needing physical verification.
