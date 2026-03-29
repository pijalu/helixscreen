# Snapshot QR Scanner Fallback

**Date:** 2026-03-29
**Status:** Approved

## Problem

Camera-based QR scanning for Spoolman spool assignment requires `HELIX_HAS_CAMERA=1`, which is disabled on K1C, AD5M, AD5X, K2, SonicPad, and other constrained platforms. These platforms may still have a webcam connected (e.g., K1C's built-in AI camera), but the full MJPEG streaming path is too memory-intensive (~40MB peak). Users on these platforms can only use USB barcode scanners.

## Solution

A lightweight snapshot-polling QR scanner that fetches still images from the webcam snapshot URL at ~1.5 fps. This serves as a **runtime fallback** when the full camera streaming path is unavailable — no new build flags needed.

### Fallback Hierarchy

1. **Full camera stream** (`HELIX_HAS_CAMERA=1`) — existing MJPEG streaming + QR decode
2. **Snapshot polling** (any platform, runtime) — polls snapshot URL, decodes JPEG, runs QR detection
3. **USB barcode scanner** — always available in parallel

## Architecture

### New Class: `SnapshotQrScanner`

**Files:** `include/snapshot_qr_scanner.h`, `src/system/snapshot_qr_scanner.cpp`

Lightweight alternative to `CameraStream`. Allocated only when the QR scanner overlay is shown; fully deallocated (including all frame buffers) when the overlay is dismissed. Zero persistent RAM cost.

**Responsibilities:**
- Poll webcam snapshot URL via HTTP GET on a background thread
- Decode JPEG with stb_image (header-only, no turbojpeg dependency)
- Produce an `lv_draw_buf_t` (RGB888, full-color) for viewfinder display
- Subsample to grayscale and run QUIRC QR decode
- Expose a frame callback matching `CameraStream`'s pattern

**Lifecycle:**
- Created by `QrScannerOverlay::show()` when camera streaming is unavailable
- Destroyed by `QrScannerOverlay` dismissal (go_back, timeout, or QR found)
- All buffers freed on destruction — no leaks, no persistent allocation

### QrScannerOverlay Changes

Minimal changes to the existing overlay:

```
show() {
    if HELIX_HAS_CAMERA:
        camera_ = CameraStream(...)     // existing path
    else if snapshot_url available:
        snapshot_scanner_ = SnapshotQrScanner(snapshot_url, frame_callback)

    // USB scanner always starts regardless
    usb_monitor_.start(...)
}
```

Both `CameraStream` and `SnapshotQrScanner` deliver frames through the same `on_camera_frame()` handler. The overlay doesn't know or care which is driving it.

### Memory Budget (Snapshot Path)

| Item | Size | Lifetime |
|------|------|----------|
| JPEG response buffer | ~200KB | Per-fetch, freed after decode |
| stb_image RGB decode (temp) | ~2.7MB | Freed immediately after copy to draw buf |
| LVGL draw buffer (viewfinder) | ~2.7MB | Reused each frame, freed on dismiss |
| Grayscale QR buffer | ~130KB | Reused each frame, freed on dismiss |
| QrDecoder (QUIRC context) | ~8KB | Freed on dismiss |
| **Peak (during decode)** | **~5.7MB** | |
| **Steady state** | **~2.8MB** | |

Fits comfortably in K1C's 256MB RAM. All memory freed when overlay dismissed.

### Polling Loop

```
background thread:
    while scan_active:
        if previous frame consumed:
            response = http_get(snapshot_url)
            if success:
                rgb = stb_image_decode(response.body)
                copy to draw_buf (reuse single buffer)
                free stb decode buffer
                subsample to grayscale
                run quirc decode
                deliver frame to UI thread via queue_update
                reset backoff
            else:
                backoff (min 1s, max 5s)
        sleep(1500ms)
```

- **Backpressure:** Skips fetch if previous frame not consumed
- **Error handling:** Exponential backoff on HTTP failure, max 5s
- **Thread safety:** `AsyncLifetimeGuard` token pattern for callback safety
- **Termination:** `scan_active_` atomic flag, checked each iteration

### Viewfinder Display

- Full-color RGB888 — same quality as the streaming camera path
- Single frame buffer, reused each poll cycle (no double-buffering needed at 1.5 fps)
- Displayed via `lv_image_set_src()` on the existing viewfinder widget

### QR Decode

- Grayscale subsampled to max 480px on longest dimension (same as existing camera path)
- Green channel extracted as luma proxy
- QUIRC decode on background thread, non-blocking
- Same `on_spool_id_detected()` → Spoolman lookup → result callback flow

## What Stays Unchanged

- `QrDecoder` — used as-is
- Spoolman lookup flow — unchanged
- USB scanner path — unchanged, always runs in parallel
- Overlay UI/layout XML — unchanged
- `CameraStream` — untouched, still used when `HELIX_HAS_CAMERA=1`

## Testing

- Unit test: `SnapshotQrScanner` with mock HTTP responses (JPEG bytes → QR decode)
- Integration: test on K1C with AI camera (verify snapshot URL available from Moonraker)
- Memory: verify peak and steady-state RAM on K1C via `/proc/self/status`
- Fallback: verify overlay gracefully shows "No camera available" if snapshot URL is also empty
