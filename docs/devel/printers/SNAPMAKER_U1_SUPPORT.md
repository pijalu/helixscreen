<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Snapmaker U1 Support

HelixScreen supports the Snapmaker U1 toolchanger as an alternative touchscreen UI. The U1 runs Klipper with Moonraker on a Rockchip ARM64 SoC, and HelixScreen can replace the stock display interface when deployed via SSH.

## Hardware

| Spec | Value |
|------|-------|
| SoC | Rockchip RK3562 — quad Cortex-A53 @ 2GHz (aarch64) |
| GPU | Mali-G52 2EE (OpenGL ES 3.2) |
| RAM | 961MB |
| Display | 3.5" 480x320 32bpp capacitive touch, DRM/KMS (`/dev/dri/card0`, rockchipdrmfb) |
| Touch Controller | TLSC6x capacitive (`tlsc6x_touch` on `/dev/input/event0`) |
| Storage | 28GB eMMC (`/userdata` ext4 persistent, SquashFS rootfs read-only overlay) |
| Recovery | A/B firmware slots + Rockchip MaskRom (unbrickable) |
| Firmware | Klipper + Moonraker |
| OS | Debian Trixie (ARM64) |
| Drivers | TMC2240 steppers |
| Filament | 4-channel RFID reader (FM175xx), OpenSpool NTAG215/216 |
| Camera | MIPI CSI + USB (Rockchip MPP/VPU) |
| Toolheads | 4 independent heads (SnapSwap system) |
| Max Speed | 500mm/s |

### SnapSwap Toolchanger

The U1 is a 4-toolhead color printer. Each head has its own nozzle, extruder, heater, and filament sensor. Tool changes take approximately 5 seconds with no purging required.

The U1 does **not** use the standard [viesturz/klipper-toolchanger](https://github.com/viesturz/klipper-toolchanger) module. Instead, it uses native multi-extruder with custom Klipper extensions. Extruders are named `extruder`, `extruder1`, `extruder2`, `extruder3` with custom state fields (`park_pin`, `active_pin`, `activating_move`, `state`). HelixScreen has a dedicated `AmsBackendSnapmaker` that tracks tool state, RFID filament data, and supports tool switching via `T0`–`T3` gcodes.

## Cross-Compilation

The U1 target uses the same aarch64 cross-compiler as the Raspberry Pi, with fully static linking to avoid glibc version dependencies.

### Build via Docker (Recommended)

```bash
# Build the Docker toolchain (first time only — cached after)
make snapmaker-u1-docker
```

The Docker image (`docker/Dockerfile.snapmaker-u1`) is based on Debian Trixie with `crossbuild-essential-arm64`. It uses Debian's `aarch64-linux-gnu` toolchain with static linking for a self-contained binary.

### Build Directly (Requires Toolchain)

```bash
make PLATFORM_TARGET=snapmaker-u1 -j
```

### Build Configuration

| Setting | Value |
|---------|-------|
| Architecture | aarch64 (ARMv8-A) |
| Toolchain | `aarch64-linux-gnu-gcc` (Debian cross) |
| Linking | Hybrid (static libstdc++/libgcc, dynamic libc/libdrm) |
| Display backend | DRM/KMS (`/dev/dri/card0`, double-buffered page flipping) |
| Input | evdev (auto-detected) |
| SSL | Enabled |
| Optimization | `-Os` (size-optimized) |
| Platform define | `HELIX_PLATFORM_SNAPMAKER_U1` |

### CI/Release Status

The Snapmaker U1 target is **deliberately excluded** from the release pipeline (`release-all` and `package-all`). It will not build in GitHub Actions until a workflow job is explicitly added to `.github/workflows/release.yml`. Manual packaging is available:

```bash
make package-snapmaker-u1
```

## Installation

### Prerequisites

1. **Snapmaker U1** on the network (Ethernet or WiFi)
2. **Extended Firmware** installed — provides SSH access. Download from [paxx12/SnapmakerU1-Extended-Firmware](https://github.com/paxx12/SnapmakerU1-Extended-Firmware), flash via USB drive (FAT32, `.bin` file in root)
3. **SSH enabled** — after Extended Firmware is installed, enable SSH via the firmware config web UI:
   ```bash
   # Open http://<printer-ip>/firmware-config/ in a browser, or:
   curl -X POST http://<printer-ip>/firmware-config/api/settings/ssh/true
   ```
4. **SSH access verified** — connect as root:
   ```bash
   ssh root@<printer-ip>   # password: snapmaker
   ```

### Build

```bash
# Build the Docker toolchain and cross-compile (first time builds the toolchain image)
make snapmaker-u1-docker
```

Output: `build/snapmaker-u1/bin/helix-screen` (~13MB stripped aarch64 binary)

### Deploy

```bash
# Full deploy (binary + assets + platform hooks) — stops stock UI, starts HelixScreen
make deploy-snapmaker-u1 SNAPMAKER_U1_HOST=<printer-ip>

# Deploy and run in foreground with debug logging (recommended for first run)
make deploy-snapmaker-u1-fg SNAPMAKER_U1_HOST=<printer-ip>

# Deploy binary only (fast iteration during development)
make deploy-snapmaker-u1-bin SNAPMAKER_U1_HOST=<printer-ip>

# SSH into the printer
make snapmaker-u1-ssh SNAPMAKER_U1_HOST=<printer-ip>
```

Default SSH user is `root` (override with `SNAPMAKER_U1_USER`). Default deploy directory is `/userdata/helixscreen` (override with `SNAPMAKER_U1_DEPLOY_DIR`).

The deploy target automatically:
- Copies the binary, assets, and platform hooks to `/userdata/helixscreen/`
- Deploys the init script (`helixscreen.init`) and DRM keepalive hooks
- Starts HelixScreen via the init script (which sources the hooks)

### What Happens on Deploy

1. DRM keepalive: a background process opens `/dev/dri/card0` to prevent CRTC teardown
2. Stock UI processes (`gui`, `lmd`) are killed via their SysV init scripts
3. HelixScreen starts as DRM master with double-buffered page flipping
4. The DRM keepalive process exits once HelixScreen has the DRM device open
5. The first-run wizard appears (language selection, printer connection setup)

### Rollback (Restore Stock UI)

To restore the stock Snapmaker touchscreen UI at any time:

```bash
# Quick rollback — kill HelixScreen, restart stock UI
ssh root@<printer-ip> "killall helix-screen helix-watchdog; /etc/init.d/S99screen start; /etc/init.d/S90lmd start"

# Or simply reboot — stock UI starts automatically on boot
ssh root@<printer-ip> reboot
```

The stock UI lives on the read-only SquashFS rootfs and cannot be damaged by HelixScreen deployment. HelixScreen files are entirely on `/userdata/` and can be removed cleanly:

```bash
ssh root@<printer-ip> "killall helix-screen helix-watchdog 2>/dev/null; rm -rf /userdata/helixscreen; reboot"
```

## Reversible Deployment Strategy

HelixScreen can be deployed to the U1 without modifying the stock firmware. The approach is fully reversible at multiple levels.

### Level 1: Manual SSH Deployment (Current, Fully Reversible)

This is the current deployment method used by `make deploy-snapmaker-u1`:

1. Install [Extended Firmware](https://github.com/paxx12/SnapmakerU1-Extended-Firmware) for SSH access
2. Enable SSH via firmware config web UI
3. Deploy via `make deploy-snapmaker-u1 SNAPMAKER_U1_HOST=<ip>`
4. Platform hooks stop stock UI (`S99screen`, `S90lmd`) and HelixScreen starts on `/dev/fb0`

**To revert**: `killall helix-screen; /etc/init.d/S99screen start` — or simply reboot. The stock UI is on the read-only SquashFS rootfs and cannot be damaged. Init scripts are not modified.

### Level 2: SysV Init Override (Persistent, Reversible)

The U1 uses SysV init (not systemd). A persistent override would:

1. Create a HelixScreen init script in `/etc/init.d/` (writable overlay)
2. Optionally chmod -x the stock `S99screen` script (reversible since overlay)
3. HelixScreen starts on boot; stock UI stays dormant

**To revert**: Remove the init script override and reboot. Stock UI resumes from the read-only base.

### Level 3: Extended Firmware Overlay (Cleanest, Reversible)

Package HelixScreen as an overlay in paxx12's Extended Firmware build system:

1. Add a HelixScreen overlay that deploys the binary and init script
2. Build a custom firmware .bin with the overlay included
3. Flash via USB like any firmware update

**To revert**: Flash stock firmware (or Extended Firmware without the HelixScreen overlay) via USB. A/B firmware slots ensure the previous firmware is preserved.

### Safety Guarantees

| Risk | Mitigation |
|------|-----------|
| Bricked device | Impossible — Rockchip MaskRom mode provides hardware-level recovery |
| Lost stock UI | Stock UI lives on read-only SquashFS — cannot be accidentally deleted |
| Klipper/Moonraker disrupted | HelixScreen only replaces the display UI; Klipper (S60klipper) and Moonraker (S61moonraker) are independent services |
| Can't revert | Multiple revert paths: reboot, kill process, remove override, reflash firmware |
| Firmware slot corruption | A/B slots — switch with `updateEngine --misc=other --reboot` |

### Display Backend — DRM/KMS with CRTC Keepalive

HelixScreen uses the DRM backend for double-buffered page flipping on `/dev/dri/card0` (rockchipdrmfb). The 480x320 MCU panel runs on a DPI/RGB parallel interface via the Rockchip VOP2 display controller.

**The CRTC keepalive problem**: The stock UI (`/usr/bin/gui`) holds DRM master. When gui exits, the kernel's VOP2 driver calls `vop2_crtc_atomic_disable`, permanently disabling the display until reboot. The MCU panel driver only creates modes during the initial boot sequence — once the CRTC is disabled, there's no way to re-enable it.

**The solution**: The platform hooks (`config/platform/hooks-snapmaker-u1.sh`) spawn a background process that holds `/dev/dri/card0` open *before* killing gui. This prevents the kernel from tearing down the CRTC when gui exits. HelixScreen then opens the DRM device itself and becomes DRM master. The keepalive process detects that HelixScreen has the device open and exits — but the CRTC stays active because HelixScreen now holds the fd.

**Critical implementation notes**:
- The keepalive MUST be a background subshell (`(exec 3>/dev/dri/card0; ...) &`), not a shell fd (`exec 7>`). Shell fds die when the init script exits, but background processes survive.
- The keepalive polls `/proc/*/fd` until it sees `helix-screen` with `/dev/dri/card0` open, then exits.
- `HELIX_DRM_DEVICE=/dev/dri/card0` is set in `platform_pre_start()` to skip auto-detection.
- No libinput is needed — touch input uses evdev directly.

**Filesystem note**: `/opt/` is an overlay filesystem wiped on reboot. All persistent data lives on `/userdata/` (ext4, 28GB). `/home/lava/` is also part of the overlay and is NOT persistent.

### Touch Input

Touch input is provided by a TLSC6x capacitive controller (`tlsc6x_touch`) on `/dev/input/event0`. HelixScreen auto-detects this device and uses multitouch (MT) axis ranges (0-480, 0-320). No touch calibration is required — the capacitive controller is factory-calibrated.

### Backlight

HelixScreen auto-detects the sysfs backlight device (`/sys/class/backlight/backlight`, max brightness 255) for sleep/wake control.

## Auto-Detection

HelixScreen auto-detects the Snapmaker U1 using several heuristics from `config/printer_database.json`:

| Heuristic | Confidence | Description |
|-----------|------------|-------------|
| `fm175xx_reader` object | 99 | FM175xx RFID reader -- definitive U1 signature |
| `FILAMENT_DT_UPDATE` macro | 95 | RFID filament detection macro (extended firmware) |
| `FILAMENT_DT_QUERY` macro | 95 | RFID filament query macro (extended firmware) |
| Hostname `u1` | 90 | Hostname contains "u1" |
| Hostname `snapmaker` | 85 | Hostname contains "snapmaker" |
| `tmc2240` object | 60 | TMC2240 stepper driver presence |
| CoreXY kinematics | 40 | CoreXY motion system |
| Cartesian kinematics | 20 | Cartesian motion system |

No manual printer configuration is needed in most cases. The FM175xx RFID reader is the strongest signal -- it is unique to the U1 and provides near-certain identification.

## Print Start Tracking

HelixScreen uses the `snapmaker_u1` print start profile (`config/print_start_profiles/snapmaker_u1.json`) to track progress through the startup sequence. The profile uses weighted progress mode with these phases:

1. Homing (10%)
2. Bed heating (20%)
3. Nozzle heating (20%)
4. Z tilt adjust (15%)
5. Bed mesh calibration (15%)
6. Nozzle cleaning (10%)
7. Purging (10%)

The progress bar updates as each phase completes, so you can see exactly where your printer is in its startup routine.

## 480x320 Display Considerations

The U1's 480x320 display uses the TINY layout preset. This is the smallest resolution HelixScreen supports, and several UI panels have known layout issues at this size. See the [480x320 UI Audit](480x320_UI_AUDIT.md) for a panel-by-panel breakdown. Key issues:

- **Navbar icons clipped** at screen edges
- **Controls panel** labels overlapping, z-offset value wrapping
- **Print select list view** fundamentally broken at this size
- **Numeric keypad overlay** too tall, bottom rows cut off
- **Filament panel** cards pushed off-screen

These are resolution-specific issues, not Snapmaker-specific. Any 480x320 device benefits from the same fixes.

## Known Limitations

- **Not in CI/release pipeline** -- Must be built manually. No automated release artifacts yet.
- **480x320 UI needs work** -- Multiple panels have layout issues at this resolution (see above).
- **Extended firmware required** -- SSH access (needed for deployment) requires the community [Extended Firmware](https://github.com/paxx12/SnapmakerU1-Extended-Firmware). Stock firmware does not provide SSH.
- **No auto-start on boot** -- HelixScreen must be started manually after each reboot. An init script (`helixscreen.init`) is deployed but must be installed to `/etc/init.d/` (on the overlay, or via Extended Firmware overlay mechanism) for boot persistence.
- **WiFi management** -- Stopping `unisrv` (stock UI) does not affect WiFi — the U1 uses standard `wpa_supplicant` managed by the OS. HelixScreen has its own WiFi manager with `wpa_supplicant` support.

## Future Work

### Auto-Start on Boot

The init script (`helixscreen.init`) exists and works, but the overlay filesystem is reset on reboot. Options for persistence:
1. Install to `/oem/overlay/upper/etc/init.d/S90helixscreen` (persists in the overlay upper layer)
2. Package as an Extended Firmware overlay for automatic deployment
3. Add a post-boot hook in the Extended Firmware configuration

### Extended Firmware Overlay

Package HelixScreen as an Extended Firmware overlay for one-click installation via paxx12's build system.

### RFID Filament UI

The `AmsBackendSnapmaker` backend already parses RFID data from `filament_detect.info` (material type, color, temperature ranges, spool weight). The filament panel UI needs to be tested and refined to properly display this data.

### Virtual Slot Mapping

The U1 supports an `extruder_map_table` with 32 virtual slots mapped to 4 physical extruders. This could enable more advanced filament management workflows.

## Verified Hardware

HelixScreen has been tested on a Snapmaker U1 with Extended Firmware. Confirmed working:

- DRM display at 480x320 via rockchipdrmfb with double-buffered page flipping
- DRM CRTC keepalive works — gui killed cleanly, no SIGSTOP hack needed
- Touch input via TLSC6x capacitive controller (no calibration needed)
- Backlight control via sysfs
- Stock UI stops and restarts cleanly via init script hooks
- SSH session survives stopping gui (WiFi unaffected)
- First-run wizard displays correctly at TINY breakpoint
- Memory monitor reports 961MB total with appropriate thresholds
- Persistent deployment on `/userdata/` survives reboots

## Community Testing

We welcome additional testers with Snapmaker U1 hardware:

1. Install the [Extended Firmware](https://github.com/paxx12/SnapmakerU1-Extended-Firmware) for SSH access
2. Enable SSH: `curl -X POST http://<ip>/firmware-config/api/settings/ssh/true`
3. Build: `make snapmaker-u1-docker`
4. Deploy: `make deploy-snapmaker-u1-fg SNAPMAKER_U1_HOST=<ip>` (runs in foreground with debug logging)
5. Report: Does the wizard appear? Does touch work? Can you connect to Moonraker? Do tool changes work?
6. File issues at the HelixScreen GitHub repository

## Related Resources

- **[Extended Firmware](https://github.com/paxx12/SnapmakerU1-Extended-Firmware)** -- Adds SSH access and community features to the U1
- **[U1 Config Example](https://github.com/JNP-1/Snapmaker-U1-Config)** -- Community reverse-engineered Klipper configuration
- **[Snapmaker Forum](https://forum.snapmaker.com/c/snapmaker-products/87)** -- Official U1 discussion
- **[Toolchanger Research](printer-research/SNAPMAKER_U1_RESEARCH.md)** -- Detailed analysis of U1's toolchanger implementation vs. standard Klipper toolchanger module
- **[Snapmaker/u1-klipper](https://github.com/Snapmaker/u1-klipper)** -- Open source Klipper fork
- **[Snapmaker/u1-moonraker](https://github.com/Snapmaker/u1-moonraker)** -- Open source Moonraker fork
- **[Snapmaker/u1-fluidd](https://github.com/Snapmaker/u1-fluidd)** -- Open source Fluidd fork
- **[paxx12/u1-firmware-tools](https://github.com/paxx12/u1-firmware-tools)** -- Firmware unpack/repack tools
- **[480x320 UI Audit](480x320_UI_AUDIT.md)** -- Panel-by-panel breakdown of layout issues at this resolution
