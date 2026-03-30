# Camera Probe at Startup

**Date:** 2026-03-29
**Status:** Approved

## Problem

On K1C and AD5M, mjpg_streamer runs on localhost but Moonraker's `server.webcams.list` returns empty because the `[webcam]` config section is commented out. The webcam config is deliberately left disabled to avoid streaming overhead from other frontends. Our snapshot QR scanner has no URL to poll, and `printer_has_webcam` stays 0.

## Solution

After Moonraker's `server.webcams.list` returns no enabled webcams, probe a short list of known local camera endpoints on a background thread. If one responds, store the snapshot URL and set `printer_has_webcam` so the QR scanner and camera widget work.

## Probe Sequence

Runs during `moonraker_discovery_sequence.cpp`, after processing the webcam list response. Only fires if no enabled webcam was found.

**Endpoints probed (in order, first success wins):**

1. `http://127.0.0.1:8080/?action=snapshot` — direct mjpg_streamer (standard port)
2. `http://127.0.0.1:8081/?action=snapshot` — second camera instance
3. `http://127.0.0.1:4408/webcam/?action=snapshot` — Creality nginx proxy

**Success criteria:** HTTP 200 response (HEAD request, 2s timeout per endpoint).

**On success:** Call `set_webcam_available(true, "", snapshot_url, false, false)` — empty stream URL forces the snapshot scanner path rather than full MJPEG streaming. This is intentional: we don't want to enable streaming on constrained devices.

**On failure:** Do nothing. `printer_has_webcam` stays 0. QR scanner falls back to USB barcode scanner only.

## What Changes

- **`src/api/moonraker_discovery_sequence.cpp`** — add camera probe after webcam list processing

## What Stays Unchanged

- `PrinterCapabilitiesState` — already handles `set_webcam_available()`
- `QrScannerOverlay` — already falls back to snapshot scanner when no stream URL
- `SnapshotQrScanner` — works as-is with the discovered snapshot URL
- Camera widget visibility — already bound to `printer_has_webcam` subject
- QR button visibility — stays gated on `printer_has_spoolman` only

## Constraints

- Probe runs on a background thread (non-blocking)
- Total probe time: max ~6s worst case (3 endpoints × 2s timeout), typically <200ms
- Only probes localhost — no network traffic
- Only runs when Moonraker returns no webcams — does not override user-configured webcams
