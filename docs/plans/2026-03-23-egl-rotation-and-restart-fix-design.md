# Fix EGL Rotation (#457) and First-Boot Restart (#470)

**Date:** 2026-03-23
**Issues:** prestonbrown/helixscreen#457, prestonbrown/helixscreen#470

---

## Problem 1: EGL Rotation Fails Silently (#457)

### Symptom

Users with GPU-accelerated (EGL/OpenGL ES) DRM displays cannot use the `"rotate"` config option. Setting `"rotate": 270` in `helixconfig.json` causes display init to fail, the process exits, watchdog restarts without rotation, and the config key may be lost.

### Root Cause

In `src/api/display_backend_drm.cpp:591-592`, `supports_hardware_rotation()` unconditionally returns `false` when `HELIX_ENABLE_OPENGLES` is defined:

```cpp
#ifdef HELIX_ENABLE_OPENGLES
    return false;
#endif
```

This triggers `try_drm_to_fbdev_fallback()` in `display_manager.cpp:287`, which attempts to fall back to fbdev. On DSI displays that only expose DRM (no `/dev/fb0`), the fallback fails, `init()` returns `false`, and the process exits.

### Fix

LVGL's OpenGL ES renderer already handles rotation natively:

- `lv_opengles_driver.c:672-764` — `populate_vertex_buffer()` transforms vertex coordinates based on `lv_display_get_rotation()`
- `lv_opengles_shader.c:61-76` — vertex shader applies a transform matrix with flip/rotation
- `lv_linux_drm_egl.c:195-208` — `set_viewport()` swaps width/height for 90°/270°

Change `supports_hardware_rotation()` to return `true` on the EGL path:

```cpp
#ifdef HELIX_ENABLE_OPENGLES
    // LVGL's OpenGL ES renderer handles rotation natively via vertex
    // transforms in the shader — no DRM plane rotation needed.
    return true;
#endif
```

Touch coordinate rotation is already handled by `set_display_rotation()` which remaps evdev coordinates for all DRM paths.

### Files Modified

| File | Change |
|------|--------|
| `src/api/display_backend_drm.cpp` | `supports_hardware_rotation()`: return `true` on EGL path |

### Testing

- **Unit test:** Verify `supports_hardware_rotation()` returns `true` on EGL for 90°/180°/270°
- **Hardware test (Pi 5, 192.168.1.113):** Set `"rotate": 270` in helixconfig.json, verify display rotates and touch maps correctly

---

## Problem 2: Spurious Service Restart on Boot (#470)

### Symptom

HelixScreen starts, runs for a few seconds, then systemd stops and restarts the service. After the restart, it stays running. Reported independently by two users (#457, #470).

### Root Cause (Theory)

`helixscreen.service` has an `ExecStartPre` step that recursively chowns the install directory:

```ini
ExecStartPre=+/bin/sh -c 'u=@@HELIX_USER@@; [ "$$u" != "root" ] && chown -Rh "$$u:$$u" "@@INSTALL_DIR@@" 2>/dev/null || true'
```

This changes the `ctime` of `release_info.json`. The active `helixscreen-update.path` unit (PathChanged) detects this metadata change and triggers `helixscreen-update.service`, which sleeps 10 seconds then runs `systemctl restart helixscreen`.

On the second start, ownership is already correct, so `chown` is a no-op (no ctime change), and no trigger occurs. This explains why it happens exactly once per boot.

### Fix

Stop the update path watcher before `ExecStartPre` steps run, re-arm it after the service starts. This prevents ANY `ExecStartPre` side-effect from triggering a spurious restart.

Add to `config/helixscreen.service`:

```ini
# Pause update watcher during startup to prevent ExecStartPre side-effects
# (e.g., chown changing ctime on release_info.json) from triggering a
# spurious restart via helixscreen-update.path → helixscreen-update.service.
ExecStartPre=+/bin/systemctl stop helixscreen-update.path 2>/dev/null || true

# ... existing ExecStartPre steps ...

# Re-arm update watcher after main process starts
ExecStartPost=+/bin/systemctl start helixscreen-update.path 2>/dev/null || true
```

The `|| true` ensures the service starts even if the path unit isn't installed (e.g., SysV init systems, development environments).

### Files Modified

| File | Change |
|------|--------|
| `config/helixscreen.service` | Add ExecStartPre to stop path watcher, ExecStartPost to re-arm |

### Testing

- **Shell test (BATS):** Validate that `chown` on a file triggers systemd `PathChanged` via ctime modification (confirms theory)
- **Hardware test (Pi 5, 192.168.1.113):**
  1. Before fix: `sudo systemctl restart helixscreen`, check `journalctl -u helixscreen-update.service` for activation
  2. After fix: same test, verify no spurious activation

### Follow-up

Post diagnostic question on both issues asking reporters to run:
```bash
journalctl -u helixscreen-update.path -u helixscreen-update.service --no-pager -n 50
```
This confirms whether the update path unit is the trigger on their specific systems.
