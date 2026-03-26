# Brother PT (P-Touch) Label Printer Support

**Date:** 2026-03-26
**Status:** Design
**Scope:** Add support for Brother PT-series (P-Touch) Bluetooth label printers

## Summary

Add support for Brother PT-series label printers (PT-P300BT, PT-P710BT, PT-P900W, etc.) using the PTCBP raster protocol over Bluetooth RFCOMM. The existing spool label workflow will work with TZe tape cartridges, with tape size auto-detected from the printer status response.

## Motivation

Brother PT-series (P-Touch Cube) printers are popular, affordable Bluetooth label makers that use TZe laminated tape. Supporting them expands the label printer options beyond the QL series (shipping labels) to include smaller, more portable tape-based labeling — useful for filament spool labeling at a lower price point.

## Requirements

- Support 128-pin PT models (P300BT, P710BT) initially; 256-pin models (P900W, P950NW) are future work
- Use existing spool label rendering workflow
- Auto-detect loaded tape width from printer status (no manual size selection)
- Bluetooth Classic (RFCOMM/SPP) transport — same as existing Brother QL BT path
- Pure-function protocol implementation for testability

## Protocol: PTCBP Raster Mode

### Relationship to Brother QL

The PT series uses PTCBP (P-Touch Command Binary Protocol), which shares significant structure with the QL raster protocol:

| Feature | QL Series | PT Series |
|---------|-----------|-----------|
| Print head width | 720 pins (90 bytes/row) | 128 pins (16 bytes/row) |
| Resolution | 300 dpi | 180 dpi (180x360 hi-res optional) |
| Media | DK die-cut labels + continuous rolls | TZe laminated tape cartridges |
| Status request | `1B 69 53` | `1B 69 53` (same) |
| Status response | 32 bytes | 32 bytes (same format) |
| Raster transfer | `47 {n1} {n2} {data}` | `47 {n1} {n2} {data}` (same) |
| Blank row | `5A` | `5A` (same) |
| Print command | `0C` / `1A` | `0C` / `1A` (same) |
| Compression | TIFF PackBits | TIFF PackBits (same) |
| Init invalidate | 200 bytes of 0x00 | 100 bytes of 0x00 |

The wire format is compatible — the differences are physical parameters (row width, resolution, media types).

### Print Sequence

```
1. Invalidate       — 100 bytes of 0x00
2. Initialize       — 1B 40
3. Enter raster     — 1B 69 61 01
4. Print info       — 1B 69 7A {n1..n10}  (media type, width, raster count)
5. Mode settings    — 1B 69 4D {n1}       (auto-cut flag)
6. Advanced mode    — 1B 69 4B {n1}       (no chain printing)
7. Margin           — 1B 69 64 {n1} {n2}  (feed in dots, LE 16-bit)
8. Compression      — 4D 02               (TIFF PackBits)
9. Raster rows      — 47 {len_lo} {len_hi} {compressed_data}  (per row)
                    — 5A                                        (blank rows)
10. Print + feed    — 1A
```

### Print Info Command (`1B 69 7A`)

10 parameter bytes:

| Byte | Field | Value |
|------|-------|-------|
| n1 | Valid flags | `0x86` (media type + width + quality) |
| n2 | Media type | `0x01` laminated, `0x03` non-laminated, `0x11` heat-shrink |
| n3 | Tape width (mm) | e.g., `0x0C` for 12mm |
| n4 | Tape length (mm) | `0x00` (continuous) |
| n5-n8 | Raster line count | LE 32-bit |
| n9 | Page flag | `0x00` first/only page |
| n10 | Reserved | `0x00` |

### Tape Width Table

128-pin models (P300BT, P710BT) use 16 bytes per raster row. Non-printable margins are zeroed.

Note: The status response reports 3.5mm tape as width byte `0x04` (integer 4). The tape info lookup uses this raw byte value, not the physical 3.5mm.

| Tape Width | Status Byte | Left Margin (pins) | Printable Pins | Right Margin (pins) |
|------------|------------|-------------------|----------------|---------------------|
| 3.5mm | 4 | 52 | 24 | 52 |
| 6mm | 6 | 48 | 32 | 48 |
| 9mm | 9 | 39 | 50 | 39 |
| 12mm | 12 | 29 | 70 | 29 |
| 18mm | 18 | 8 | 112 | 8 |
| 24mm | 24 | 0 | 128 | 0 |

### Model Support

| Model | Max Tape | Print Head | BT Transport | Notes |
|-------|----------|-----------|-------------|-------|
| PT-P300BT | 12mm | 128-pin | RFCOMM | Battery-powered, most common |
| PT-P710BT | 24mm | 128-pin | RFCOMM | AC/battery, NFC tap-to-connect |
| PT-P900W | 36mm | 256-pin | RFCOMM + WiFi | **Future work** — different row width |
| PT-P950NW | 36mm | 256-pin | RFCOMM + WiFi | **Future work** — different row width |

Model is identified from the BT device name (e.g., `"PT-P300BT"`). The tape width is always auto-detected from the status response, so model detection is informational only. The 256-pin models (P900W, P950NW) use 32 bytes per raster row and are out of scope for this initial implementation.

### TIFF PackBits Compression

Each 16-byte raster row is independently compressed using TIFF PackBits RLE:

- Control byte `N` where `0 <= N <= 127`: copy next `N+1` literal bytes
- Control byte `N` where `-127 <= N <= -1` (as signed): repeat next byte `1-N` times
- Control byte `-128` (`0x80`): no-op

Output always decompresses to exactly 16 bytes.

Note: `pwg_raster.cpp` has a PackBits implementation for IPP/PWG, but it operates on different row widths and includes PWG-specific framing. The PT PackBits encoder is simple enough (16-byte rows) that a standalone implementation is cleaner than extracting a shared utility.

### Status Response (32 bytes)

Key fields for our use:

| Offset | Field | Usage |
|--------|-------|-------|
| 8 | Error info 1 | `0x01`=no media, `0x04`=cutter jam, `0x08`=weak battery |
| 9 | Error info 2 | `0x01`=wrong media, `0x10`=cover open, `0x20`=overheating |
| 10 | Tape width (mm) | Auto-detect: 0, 4, 6, 9, 12, 18, 24 |
| 11 | Media type | `0x01`=laminated, `0x03`=non-laminated |
| 18 | Status type | `0x00`=ready, `0x01`=print complete, `0x02`=error |

### Image Preparation

1. Input: 1bpp `LabelBitmap` (spool label rendered at 180 dpi)
2. Rotate 90° CW (label is printed lengthwise along tape)
3. Horizontal flip (print head orientation — **verify against physical output during development**, this is based on QL behavior and reference implementations; if labels print mirrored, remove this step)
4. Center-pad to 128 pixels wide with zero margins per tape width table

### Feed/Margin Constraints (180 dpi)

- Minimum margin: 14 dots (2mm)
- Default margin: 14 dots
- Minimum print length: 31 dots (4.4mm)
- Maximum print length: 7086 dots (1000mm)

## Architecture

### RFCOMM Transport: Direct Context Management

The existing `rfcomm_send()` and `rfcomm_send_receive()` helpers in `bt_print_utils.h` are insufficient for the PT print flow, which requires a multi-step exchange within a single RFCOMM connection (status query → read response → send raster → read completion).

The PT backend manages its own BT context lifecycle directly, following the same pattern as `MakeIdBluetoothPrinter` (which also needs bidirectional multi-step RFCOMM). The flow is:

1. `helix_bt_init()` → context
2. `helix_bt_connect_rfcomm(ctx, mac, channel)` → fd
3. `write(fd, status_request)` → `read(fd, 32 bytes)` → parse tape
4. `write(fd, raster_data)` → `read(fd, 32 bytes)` → verify completion
5. `helix_bt_disconnect(ctx, fd)`
6. `helix_bt_deinit(ctx)`

The PT backend uses a file-local `s_print_mutex` (same pattern as `MakeIdBluetoothPrinter`) to serialize its own print operations. Cross-backend serialization (e.g., PT and QL printing simultaneously) is handled at the BlueZ socket level — the adapter only supports one RFCOMM connection at a time, so a second `helix_bt_connect_rfcomm()` will fail with an error rather than corrupt state.

### Deferred Rendering (Auto-Detect Pattern)

Because tape width is unknown until the status query completes on the worker thread, the PT dispatch path in `label_printer_utils.cpp` follows the **deferred-render pattern** already used by the Brother QL network path (which detects loaded media over TCP before rendering).

The PT `print()` method receives the `SpoolInfo` and `LabelPreset` instead of a pre-rendered bitmap. The print thread:
1. Connects and queries tape status
2. Builds a `LabelSize` from the detected tape (`brother_pt_label_size_for_tape(width_mm)`)
3. Renders the spool label at 180 dpi with the correct printable width
4. Builds and sends the raster

This means the PT dispatch in `label_printer_utils.cpp` skips the pre-render step and passes spool info directly to the printer backend. The `ILabelPrinter` interface is not changed — the `bitmap` parameter to `print()` can be empty/placeholder since the PT backend renders internally. Alternatively, a second overload or a PT-specific dispatch function handles this cleanly.

### Print Completion: Unsolicited Status Response

Brother PT printers send an **unsolicited 32-byte status response** when printing completes (status_type=`0x01`) or encounters an error (status_type=`0x02`). The implementation reads with a timeout rather than polling:

```
write(fd, raster_data)
// Printer processes and prints...
read(fd, 32, timeout=10s)  // blocks until printer responds
if status_type == 0x01 → success
if status_type == 0x02 → parse error, callback(error)
if timeout → callback("Print timed out")
```

No polling loop is needed.

### New Files

#### `include/brother_pt_protocol.h`

```cpp
namespace helix::label {

/// Parsed status from a Brother PT printer
struct BrotherPTMedia {
    uint8_t media_type = 0;   // 0x01=laminated, 0x03=non-laminated
    uint8_t width_mm = 0;     // 0=none, 4, 6, 9, 12, 18, 24
    uint8_t error_info_1 = 0;
    uint8_t error_info_2 = 0;
    uint8_t status_type = 0;  // 0x00=ready, 0x01=complete, 0x02=error
    bool valid = false;
};

/// Tape geometry for a given width
struct BrotherPTTapeInfo {
    int width_mm;
    int printable_pins;
    int left_margin_pins;
};

/// Get tape info for a given width, or nullptr if unsupported
const BrotherPTTapeInfo* brother_pt_get_tape_info(int width_mm);

/// Build a status request command
std::vector<uint8_t> brother_pt_build_status_request();

/// Parse a 32-byte status response
BrotherPTMedia brother_pt_parse_status(const uint8_t* data, size_t len);

/// Build complete raster command sequence for printing a label.
/// Bitmap should already be at 180 dpi, oriented for the label content.
/// This function handles rotation, flip, centering, and compression.
std::vector<uint8_t> brother_pt_build_raster(const LabelBitmap& bitmap,
                                              int tape_width_mm);

/// Human-readable error string from status, or empty if no error
std::string brother_pt_error_string(const BrotherPTMedia& media);

/// Create a LabelSize from detected tape width (for renderer)
/// Returns nullopt if width is unsupported
std::optional<LabelSize> brother_pt_label_size_for_tape(int width_mm);

} // namespace helix::label
```

#### `include/brother_pt_bt_printer.h`

```cpp
namespace helix::label {

class BrotherPTBluetoothPrinter : public ILabelPrinter {
public:
    void set_device(const std::string& mac, int channel = 1);

    [[nodiscard]] std::string name() const override;
    void print(const LabelBitmap& bitmap, const LabelSize& size,
               PrintCallback callback) override;
    [[nodiscard]] std::vector<LabelSize> supported_sizes() const override;

    /// PT-specific: deferred render with auto-detect tape width
    void print_spool(const SpoolInfo& spool, int preset, PrintCallback callback);

    static std::vector<LabelSize> supported_sizes_static();

private:
    std::string mac_;
    int channel_ = 1;
};

} // namespace helix::label
```

### Modified Files

#### `src/system/label_printer_utils.cpp`

Two changes:

**1. Size selection dispatch** — PT printers return PT-specific sizes, not QL sizes:

```cpp
if (helix::bluetooth::is_brother_printer(bt_name.c_str())) {
    if (is_brother_pt_printer(bt_name)) {
        sizes = BrotherPTBluetoothPrinter::supported_sizes_static();
    } else {
        sizes = BrotherQLPrinter::supported_sizes_static();
    }
}
```

**2. Print dispatch** — PT path uses deferred rendering (renders on worker thread after tape detection):

```cpp
if (is_brother_printer(bt_name.c_str())) {
    if (is_brother_pt_printer(bt_name)) {
        // PT series: deferred render — tape width unknown until status query
        BrotherPTBluetoothPrinter printer;
        printer.set_device(bt_address);
        printer.print_spool(spool, preset, std::move(callback));
    } else {
        // QL series: pre-rendered bitmap
        BrotherQLBluetoothPrinter printer;
        printer.set_device(bt_address);
        printer.print(bitmap, label_size, std::move(callback));
    }
}
```

Helper function `is_brother_pt_printer()` uses case-insensitive check for `"PT-"` prefix via `find_brand()` from `bt_discovery_utils.h`, matching the naming convention of `is_brother_printer()`, `is_niimbot_printer()`, etc.

#### `bt_discovery_utils.h`

Add `is_brother_pt_printer()` helper that returns true for `"PT-"` prefix devices. No changes to the brand table needed — `"PT-"` is already registered.

#### `Makefile`

Add `brother_pt_protocol.cpp` and `brother_pt_bt_printer.cpp` to build sources (BT plugin library).

### Print Flow

```
User taps "Print Label" on spool
    → label_printer_utils::print_spool_label()
    → detects BT + Brother + "PT-" prefix
    → BrotherPTBluetoothPrinter::print_spool(spool, preset, callback)
        → detach thread
        → acquire s_print_mutex (file-local)
        → helix_bt_init() → ctx
        → helix_bt_connect_rfcomm(ctx, mac, channel) → fd
        → write(fd, status_request)
        → read(fd, 32 bytes, timeout=5s)
        → parse tape width (e.g., 12mm)
        → if error/no tape → callback(error), cleanup, return
        → brother_pt_label_size_for_tape(12) → label_size
        → LabelRenderer::render(spool, preset, label_size) → bitmap
        → brother_pt_build_raster(bitmap, 12) → raster_bytes
        → write(fd, raster_bytes)
        → read(fd, 32 bytes, timeout=10s)  // unsolicited completion status
        → if status_type == 0x01 → success
        → if status_type == 0x02 → callback(error)
        → if timeout → callback("Print timed out")
        → helix_bt_disconnect(ctx, fd)
        → helix_bt_deinit(ctx)
        → release s_print_mutex
        → queue_update → callback(success/error)
```

### Label Rendering

The existing label renderer accepts `width_px` and `dpi` from `LabelSize`. For PT printers:

- 12mm tape: `width_px=70, height_px=0, dpi=180`
- 24mm tape: `width_px=128, height_px=0, dpi=180`

Content will be narrower than QL labels but the rendering path is shared. Label content (spool name, color swatch, material) scales down automatically.

### Settings

No new settings required. The existing BT label printer settings store MAC address and device name. The PT vs QL distinction is derived from the device name at print time.

`supported_sizes_static()` returns all possible TZe tape sizes (3.5mm through 24mm) for the settings UI size selector. The label size index from settings is used as a hint for the UI preview, but the actual print always uses the auto-detected tape width from the status query. If the detected tape differs from the selected size, the printer renders for the actual tape — no error, just prints what's loaded.

## Test Plan

### Unit Tests (`tests/unit/test_brother_pt_protocol.cpp`)

- **PackBits compression:** known input → expected compressed output, round-trip verify
- **Raster builder:** correct 16-byte row width, correct zero-padding margins per tape width
- **Status parsing:** valid 32-byte response → correct tape width/type/error fields
- **Status parsing:** truncated/invalid data → `valid=false`
- **Error string:** no-media, cover-open, overheating, weak battery, cutter jam → human-readable messages
- **Blank row encoding:** all-white row → `5A` command (not a full raster row)
- **Image centering:** bitmap narrower than printable area → correctly centered with zero margins
- **Tape info lookup:** valid widths return correct geometry, invalid width returns nullptr

### Integration Test

Print a spool label to the physical PT-P300BT and verify:
- Correct orientation (text reads left-to-right along tape)
- Content fits within printable area (no clipping)
- Auto-cut works
- Error handling: remove tape, verify error callback

## References

- [Brother PT-E550W/P750W/P710BT Raster Command Reference](https://download.brother.com/welcome/docp100064/cv_pte550wp750wp710bt_eng_raster_102.pdf)
- [Ircama/PT-P300BT (Python)](https://github.com/Ircama/PT-P300BT)
- [stecman's PT-P300BT driver (Python gist)](https://gist.github.com/stecman/ee1fd9a8b1b6f0fdd170ee87ba2ddafd)
- [philpem/printer-driver-ptouch (C, CUPS)](https://github.com/philpem/printer-driver-ptouch)
