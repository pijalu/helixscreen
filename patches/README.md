# Submodule Patches

Local patches applied to git submodules. Managed by `mk/patches.mk` — run `make reapply-patches` to reset and reapply all.

## Base Version

**LVGL**: v9.5.0 (commit `85aa60d18`)

## Upstream PR Status

Several patches have been submitted upstream to [lvgl/lvgl](https://github.com/lvgl/lvgl). If merged, the corresponding local patches can be dropped on the next LVGL version bump.

| PR | Title | Patches Included | CI |
|----|-------|-----------------|-----|
| [#9827](https://github.com/lvgl/lvgl/pull/9827) | fix(string): NULL guard for lv_strdup | `lvgl-strdup-null-guard` | All green |
| [#9828](https://github.com/lvgl/lvgl/pull/9828) | fix(slider): block perpendicular scroll chain while dragging | `lvgl_slider_scroll_chain` | All green |
| [#9829](https://github.com/lvgl/lvgl/pull/9829) | fix(evdev): Protocol-A multitouch release handling | `lvgl-evdev-protocol-a` | All green |
| [#9830](https://github.com/lvgl/lvgl/pull/9830) | fix(arc): guard against negative inner radius | `lvgl_arc_draw_guard` | All green |
| [#9831](https://github.com/lvgl/lvgl/pull/9831) | fix(draw): comprehensive NULL safety for SW draw pipeline | `lvgl_blend_null_guard`, `lvgl_blend_buf_bounds_clip`, `lvgl_blend_color_null_guard`, `lvgl-fix-signed-unsigned-draw-coords`, `lvgl_draw_sw_label_null_guard`, `lvgl_refr_reshape_null_guard`, `lvgl_img_null_guard`, `lvgl_blur_null_guard`, `lvgl_draw_buf_oom_guard` | All green |
| [#9832](https://github.com/lvgl/lvgl/pull/9832) | fix(fbdev): stride-based bpp, BGR auto-detect, buffer alignment, skip-unblank | `lvgl_fbdev_stride_bpp`, `lvgl-fbdev-bgr-swap`, `lvgl-fbdev-buffer-align`, `lvgl_fbdev_skip_unblank` | All green |

## LVGL Patches

Applied in order by `mk/patches.mk`. Grouped by subsystem.

### Display Drivers

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl_fbdev_stride_bpp.patch` | `lv_linux_fbdev.c` | Fix incorrect bpp on AD5M displays (calculate from stride) | PR #9832 |
| `lvgl_fbdev_skip_unblank.patch` | `lv_linux_fbdev.c`, `.h` | Skip FBIOBLANK during splash handoff | PR #9832 |
| `lvgl-fbdev-bgr-swap.patch` | `lv_linux_fbdev.c`, `.h` | Auto-detect BGR framebuffers and swap R/B channels (Allwinner R818) | PR #9832 |
| `lvgl-fbdev-buffer-align.patch` | `lv_linux_fbdev.c` | Over-allocate for LV_DRAW_BUF_ALIGN alignment | PR #9832 |
| `lvgl-drm-flush-rotation.patch` | `lv_linux_drm.c`, `.h` | DRM plane rotation API + 180deg software rotation via shadow buffer + legacy drmModeSetCrtc fallback | Project-specific |
| `lvgl-drm-egl-getters.patch` | `lv_linux_drm_egl.c` | EGL display/context/config getters (implementation only; header decls are in drm-flush-rotation) | Project-specific |

### Draw Pipeline

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl_blend_null_guard.patch` | `lv_draw_sw_blend.c` | NULL check for layer/draw_buf at blend entry | PR #9831 |
| `lvgl_blend_buf_bounds_clip.patch` | `lv_draw_sw_blend.c` | Clip blend_area to layer->buf_area | PR #9831 |
| `lvgl_blend_color_null_guard.patch` | `lv_draw_sw_blend_to_*.c` (16 files) | NULL dest_buf checks in all per-format blend functions | PR #9831 |
| `lvgl-fix-signed-unsigned-draw-coords.patch` | `lv_draw.c`, `lv_draw_buf.c`, `lv_draw_sw_mask_rect.c` | Fix signed→unsigned conversion in go_to_xy, downgrade OOB log to WARN | PR #9831 |
| `lvgl_draw_sw_label_null_guard.patch` | `lv_draw_sw_letter.c` | NULL check for font/glyph before all glyph format rendering | PR #9831 |
| `lvgl_draw_buf_oom_guard.patch` | `lv_draw_buf.c` | Remove redundant LV_ASSERT_MALLOC before NULL check | PR #9831 |
| `lvgl_refr_reshape_null_guard.patch` | `lv_refr.c` | NULL guard on draw_buf reshape failure, skip render gracefully | PR #9831 |
| `lvgl_img_null_guard.patch` | `lv_draw_sw_img.c` | NULL guard after go_to_xy in image mask path | PR #9831 |
| `lvgl_blur_null_guard.patch` | `lv_draw_sw_blur.c` | NULL checks after all ~15 lv_draw_buf_goto_xy() calls | PR #9831 |

### Widgets & Input

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl_slider_scroll_chain.patch` | `lv_slider.c` | Block perpendicular scroll chain during drag (touchscreen UX) | PR #9828 |
| `lvgl_arc_draw_guard.patch` | `lv_draw_arc.c`, `lv_arc.c` | Guard negative inner radius and zero-radius arc invalidation | PR #9830 |
| `lvgl-evdev-protocol-a.patch` | `lv_evdev.c` | Protocol-A touch release synthesis for Goodix GT9xx | PR #9829 |

### Core & Stdlib

| Patch | File(s) | Purpose | Upstream |
|-------|---------|---------|----------|
| `lvgl-strdup-null-guard.patch` | `lv_string_builtin.c`, `lv_string_clib.c` | NULL input guard for lv_strdup | PR #9827 |
| `lvgl_observer_debug.patch` | `lv_observer.c` | Enhanced error logging with pointer/type info | Project-specific |
| `lvgl_observer_remove_null_guard.patch` | `lv_observer.c` | NULL guard for observer removal | Project-specific |
| `lvgl_obj_delete_null_guards.patch` | `lv_global.h`, `lv_event.c`, `lv_obj.c`, `lv_obj_tree.c` | Event depth counter for corruption detection, NULL guards + alignment/depth-limit checks in event_mark_deleted, async cancel before child recursion in obj_delete_core | Pending |
| `lvgl_event_crash_hook.patch` | `lv_obj_event.c` | Weak-linked `helix_crash_note_event()` call at top of `event_send_core` — records innermost dispatch target+code for crash diagnostic reports | Project-specific |

### Project-Specific (not submitted upstream)

| Patch | File(s) | Purpose |
|-------|---------|---------|
| `lvgl_label_text_transform.patch` | `lv_label.c`, `lv_label.h`, `lv_label_private.h` | text_transform_upper flag for i18n-safe uppercase at text-set time |
| `lvgl_sdl_window.patch` | `lv_sdl_window.c` | Multi-display positioning, Android support, macOS crash fix |
| `lvgl_sdl_sw_android_debug.patch` | SDL files | SDL software renderer Android debug support |
| `lvgl_theme_breakpoints.patch` | `lv_theme_default.c` | Custom breakpoint tuning for 480-800px |

## Dropped Patches (v9.5.0)

LVGL 9.5 removed the entire XML system from core. These patches are now in `lib/helix-xml/`:

- `lv_xml.c` / `.h` -- `lv_xml_get_const_silent()` addition
- `lv_xml_style.c` -- `translate_x`/`translate_y` using `lv_xml_to_size()`
- `lv_xml_image_parser.c` -- image "contain"/"cover" alignment enums

## libhv Patches

| Patch | Purpose |
|-------|---------|
| `libhv-dns-resolver-fallback.patch` | DNS resolver fallback for mDNS |
| `libhv-streaming-upload.patch` | Streaming upload support |
| `libhv-openssl-static-link.patch` | OpenSSL/static build hook |

## Usage

```bash
# Automatic (preferred) — applies all patches if needed
make apply-patches

# Force reset and reapply all
make reapply-patches

# Regenerate a patch after manual edits in lib/lvgl/
git -C lib/lvgl diff src/path/to/file.c > patches/patch_name.patch
```
