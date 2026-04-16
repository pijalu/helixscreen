# Scroll Performance Analysis — Pi 4 DRM

**Date:** 2026-04-16
**Device:** Raspberry Pi 4, DRM dumb buffers (CPU rendering), 800x480
**Method:** `perf record -g -F 99` — 30-second per-panel recordings + 60-second aggregate

## Per-Panel Profiles

### Print Select (scrolling file list, 30s)

| % CPU | Function | Category |
|-------|----------|----------|
| 19.7% | `blend_neon_color_to_rgb888_with_opa` | Opacity blending |
| 7.3% | `blend_neon_color_to_rgb888_with_mask` | Mask blending |
| 7.0% | `blend_image_to_rgb888` (+0x134) | Image blit |
| 5.9% | `blend_image_to_rgb888` (+0x118) | Image blit |
| 2.9% | `lv_draw_sw_transform` (+0xb6c) | Image transform |
| 2.7% | `lv_draw_sw_transform` (+0xb54) | Image transform |
| 2.3% | `blend_neon_color_to_rgb888` (no opa) | Opaque blend |
| 1.6% | `lv_draw_sw_transform` (+0x7ac) | Image transform |
| 1.5% | `blend_image_to_rgb888` (+0x10e8) | Image blit |
| 1.5% | `lv_style_get_prop` | Style lookup |
| 1.4% | `blur_walk_cb` | Backdrop blur |
| 1.3% | `lv_obj_get_child_count` | Widget tree query |
| 1.3% | `lv_obj_get_ext_draw_size` | Layout query |
| 1.1% | `lv_draw_sw_transform` (+0x758) | Image transform |
| 1.1% | `blend_image_to_rgb888` (+0xd0) | Image blit |
| 1.0% | `blur_walk_cb` (+0x60) | Backdrop blur |
| 1.0% | `blend_image_to_rgb888` (+0x10c8) | Image blit |

**Category rollup:**

| Category | Total % | Notes |
|----------|---------|-------|
| Opacity blending | 19.7% | `with_opa` path — something has `opa < 255` |
| Image blit | 16.5% | Thumbnail images being blitted every frame |
| Image transform | 8.3% | **Thumbnails being rescaled during scroll** |
| Mask blending | 7.3% | Rounded corners, clipping masks |
| Opaque blend | 2.3% | Fast path — this is what we want more of |
| Blur | 2.4% | Backdrop blur running during scroll |
| Layout/style queries | 4.1% | Widget tree traversal, style lookups |

**Key finding:** 25% of Print Select CPU is thumbnail image work (blit + transform). Thumbnails are being rescaled on every frame during scroll rather than cached at target size.

### Settings (scrolling overlays, 30s)

| % CPU | Function | Category |
|-------|----------|----------|
| **66.8%** | `blend_neon_color_to_rgb888_with_opa` | **Opacity blending** |
| 6.4% | `blend_neon_color_to_rgb888_with_mask` | Mask blending |
| 2.3% | `lv_obj_get_ext_draw_size` | Layout query |
| 1.4% | `blur_walk_cb` (+0xe4) | Backdrop blur |
| 1.3% | `lv_obj_get_ext_draw_size` (+0x4) | Layout query |
| 1.2% | `blend_neon_color_to_rgb888_with_mask` (+0x288) | Mask blending |
| 1.1% | `blend_neon_color_to_rgb888` (no opa) | Opaque blend |
| 0.8% | `blur_walk_cb` (+0x60) | Backdrop blur |
| 0.8% | `lv_obj_get_child` | Widget tree query |
| 0.7% | `blur_walk_cb` (+0x100) | Backdrop blur |
| 0.7% | `blend_neon_color_to_rgb888_with_mask` (+0x334) | Mask blending |
| 0.7% | `lv_style_get_prop` | Style lookup |
| 0.6% | `lv_style_get_prop` (+0x30) | Style lookup |
| 0.6% | `lv_obj_get_state` | State query |

**Category rollup:**

| Category | Total % | Notes |
|----------|---------|-------|
| **Opacity blending** | **66.8%** | **Dominant cost — settings overlays have opacity** |
| Mask blending | 8.3% | Rounded corners, clipping |
| Blur | 2.9% | Backdrop blur effect |
| Layout/style queries | 5.7% | Widget tree + style lookups |
| Opaque blend | 1.1% | Very little opaque content |

**Key finding:** Settings is **67% opacity blending**. The settings overlays are rendering nearly everything through the expensive `with_opa` code path. This is likely the overlay background or container having partial opacity. Making the overlay fully opaque would eliminate most of the rendering cost.

### Home (idle, camera running, 30s)

| % CPU | Function | Category |
|-------|----------|----------|
| 11.5% | `CameraStream::process_stream_data` (+0x13c) | Camera MJPEG |
| 5.9% | `transform_rgb888` (+0x2d4) | Camera pixel xform |
| 5.8% | `transform_rgb888` (+0x1c8) | Camera pixel xform |
| 5.5% | `transform_rgb888` (+0x240) | Camera pixel xform |
| 4.2% | `CameraStream::process_stream_data` (+0x144) | Camera MJPEG |
| 3.6% | `memcmp` (libc) | Memory comparison |
| 3.5% | libturbojpeg JPEG decode | Camera JPEG decode |
| 3.1% | `lv_obj_get_child_count` | Widget tree query |
| 2.6% | libturbojpeg JPEG decode | Camera JPEG decode |
| 1.9% | `CameraStream::copy_pixels_to_lvgl` (+0xc8) | Camera pixel copy |
| 1.7% | `CameraStream::copy_pixels_to_lvgl` (+0xe0) | Camera pixel copy |
| 1.5% | `CameraStream::process_stream_data` (+0x148) | Camera MJPEG |
| 1.5% | `lv_memcpy` | Memory copy |
| 1.2% | `transform_rgb888` (+0x84) | Camera pixel xform |
| 1.2% | libturbojpeg JPEG decode | Camera JPEG decode |
| 1.1% | `blend_neon_color_to_rgb888_with_mask` | Mask blending |
| 1.1% | `lv_memcpy` (+0x1dc) | Memory copy |
| 1.1% | libturbojpeg JPEG decode | Camera JPEG decode |

**Category rollup:**

| Category | Total % | Notes |
|----------|---------|-------|
| **Camera stream** | **17.2%** | MJPEG stream processing + pixel copy |
| **Camera pixel transform** | **18.4%** | RGB888 color space conversion |
| **JPEG decode** | **8.4%** | libturbojpeg decode |
| Widget tree queries | 3.1% | `lv_obj_get_child_count` |
| Memory operations | 6.2% | memcmp + lv_memcpy |
| Mask blending | 1.1% | Minimal rendering cost when idle |

**Key finding:** ~44% of idle Home CPU is the camera stream pipeline (decode → transform → copy → render). The camera runs at full frame rate even when the user isn't interacting.

---

## Optimization Targets (Priority Order)

### 1. Settings Overlay Opacity — **HIGH IMPACT, LOW EFFORT**

67% of Settings CPU is `blend_with_opa`. The settings overlays likely have a semi-transparent background or container. Audit:
- `ui_xml/settings_*.xml` for `style_opa`, `style_bg_opa` values < 255
- The overlay container or backdrop that sits behind settings content
- Any `bind_style` that sets opacity dynamically

**Fix:** Make overlay backgrounds fully opaque (`opa=255`). This shifts rendering from the expensive `with_opa` NEON path to the fast direct blend path. Expected improvement: ~60% reduction in Settings rendering cost.

### 2. Print Select Thumbnail Caching — **HIGH IMPACT, MEDIUM EFFORT**

25% of Print Select CPU is thumbnail image blit + transform. Thumbnails appear to be rescaled on every frame during scroll.

**Fix:** Pre-scale thumbnails to target display size on first load, cache the scaled version. During scroll, blit the pre-scaled image (fast) instead of transform + blit (slow). Expected improvement: ~20% reduction in Print Select rendering cost.

### 3. Camera Frame Rate Throttle — **MEDIUM IMPACT, LOW EFFORT**

44% of idle Home CPU is camera stream. When the display is idle (no touch interaction), the camera could reduce to a lower frame rate (e.g., 5fps instead of 15-30fps).

**Fix:** Throttle camera frame rate when the display has been idle for >5 seconds. Resume full rate on touch. Expected improvement: ~30% reduction in Home idle CPU, freeing CPU for the printer firmware.

### 4. Second Draw Thread — **MEDIUM IMPACT, HIGH EFFORT**

`LV_DRAW_SW_DRAW_UNIT_CNT=2` with `nice` priority. The NEON blend work is embarrassingly parallel. But needs careful testing to avoid races, and must not starve printer firmware.

**Defer until:** after items 1-3 are done and measured.

### 5. Backdrop Blur During Scroll — **LOW IMPACT**

2-3% across panels. Only worth investigating if it's running when no overlay transition is happening.

---

## Idle Threshold Validation

Validated that the 500µs idle frame threshold is safe:
- Minimum real render on Pi 4 home panel: **1,291µs** (1.3ms)
- Safety margin: **2.6x** above threshold
- Frame times on home panel (sampled): 1.3ms – 5.2ms range
