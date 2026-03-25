<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# QIDI Printer Support

HelixScreen can run on QIDI printers that have a Linux framebuffer display. However, most older QIDI models use TJC HMI displays (a Chinese Nextion clone) connected over serial UART -- these are standalone MCU-driven screens that cannot be replaced by HelixScreen without a physical screen swap.

If your QIDI is running standard Moonraker -- whether through stock firmware, FreeDi, OpenQ1, or another community project -- and has a Linux framebuffer display, HelixScreen can replace the built-in display interface.

## Display Compatibility

QIDI uses two fundamentally different display architectures:

- **TJC HMI (serial)** -- A standalone microcontroller-driven display connected to the mainboard via serial UART. These are flashed with `.tft` firmware files via microSD card. HelixScreen **cannot** drive these displays. FreeDi targets this display type.
- **Linux framebuffer** -- A display driven directly by the Linux SoC (RK3328) via fbdev or DRM. HelixScreen **can** run on these.

## Models

All QIDI models listed below use MKSPI boards with ARM Cortex-A53 (aarch64) processors and 1GB RAM.

| Model | Display Type | Resolution | HelixScreen Compatible? | Notes |
|-------|-------------|------------|------------------------|-------|
| Q2 | Linux framebuffer (IPS capacitive) | 480x272 | **Yes** (untested) | Goodix touch controller. KlipperScreen and GuppyScreen also work on this model. |
| Max 4 | Linux framebuffer | TBD | **Yes** (untested) | Newer architecture with SoC-driven display. |
| X-Max 3 | TJC HMI (serial) | 800x480 | **No** | Requires screen replacement (HDMI/DSI touchscreen). |
| X-Plus 3 | TJC HMI (serial) | 800x480 | **No** | Requires screen replacement. Same display firmware as X-Max 3 and Plus 4. |
| Plus 4 | TJC HMI (serial) | 800x480 | **No** | Requires screen replacement. Same display firmware as X-Max 3 and X-Plus 3. |
| Q1 Pro | TJC HMI (serial) | 480x272 | **No** | Requires screen replacement. TJC model TJC4827X243_011. |
| X-Smart 3 | TJC HMI (serial) | 480x272 | **No** | Requires screen replacement. |

## Installation

### Prerequisites

- A QIDI printer with a compatible display (see table above)
- **[FreeDi](https://github.com/Phil1988/FreeDi) installed first** -- FreeDi replaces QIDI's stock OS with Armbian and mainline Klipper/Moonraker, which HelixScreen requires. Stock QIDI firmware has a modified Klipper/Moonraker that may not expose all standard endpoints.
- SSH access to the printer

### Using the Pi/aarch64 Binary

QIDI's Cortex-A53 processor is the same aarch64 architecture as the Raspberry Pi 4 and Pi 5. The standard Pi build of HelixScreen runs natively on QIDI hardware with no modifications.

```bash
# Build on a build server (or use a pre-built release)
make pi-docker

# Copy the binary to your QIDI printer
scp build-pi/bin/helix-screen root@<qidi-ip>:/usr/local/bin/

# SSH into the printer and run
ssh root@<qidi-ip>
helix-screen
```

For verbose output during first-time setup, add `-vv` for DEBUG-level logging:

```bash
helix-screen -vv
```

### Display Backend

HelixScreen auto-detects the best available display backend in this order: DRM, fbdev, SDL. QIDI hardware should work with either DRM or fbdev depending on the OS setup. No display configuration is needed -- HelixScreen picks the right backend automatically.

### Touch Input

HelixScreen uses libinput for touch input and should auto-detect `/dev/input/eventX` devices on QIDI hardware. If touch input doesn't work, check that input devices are present and accessible:

```bash
ls /dev/input/event*
```

Ensure the user running HelixScreen has read permissions on the event device. Running as root (common on QIDI printers) avoids permission issues.

## Auto-Detection

HelixScreen auto-detects QIDI printers using several heuristics:

- Hostname patterns
- Chamber heater presence
- MCU identification patterns
- Build volume dimensions
- QIDI-specific G-code macros (`M141`, `M191`, `CLEAR_NOZZLE`)

No manual printer configuration is needed in most cases. HelixScreen identifies your QIDI model and applies the correct settings automatically.

## Print Start Tracking

HelixScreen uses the `qidi` print start profile to track progress through your printer's start sequence. The profile recognizes QIDI's typical startup phases:

1. Homing
2. Bed heating
3. Nozzle cleaning (`CLEAR_NOZZLE`)
4. Z tilt adjust
5. Bed mesh calibration
6. Nozzle heating
7. Chamber heating
8. Print begins

The progress bar updates as each phase completes, so you can see exactly where your printer is in its startup routine.

## Known Limitations

- **Most older QIDI models have TJC HMI serial displays** -- The X-Max 3, X-Plus 3, Plus 4, Q1 Pro, and X-Smart 3 all use TJC (Nextion-compatible) displays connected via serial UART. HelixScreen cannot drive these. A physical screen replacement (HDMI or DSI touchscreen connected to the RK3328 SoC) is required.
- **Q2 resolution is very small** -- The Q2's 480x272 display is at the lower end of what HelixScreen supports. Some UI elements may overlap or be cramped.
- **Untested on real hardware** -- Detection heuristics and display rendering are based on documentation and community reports. Community testers are very welcome.
- **No chamber heater control UI** -- QIDI printers have heated chambers, but HelixScreen doesn't yet have a dedicated chamber temperature control panel.
- **Manual deployment required** -- There is no KIAUH or package manager integration for QIDI yet. Binary must be deployed manually after installing FreeDi.

## Community Testing

We need testers with QIDI hardware. If you can help:

1. Build or download the aarch64 binary
2. Deploy it to your QIDI printer
3. Report back: Does it start? Does the display render? Does touch work? Is your printer detected correctly?
4. File issues at the HelixScreen GitHub repository

Even a quick "it boots and shows the home screen" report is valuable. Testing on any of the supported models helps the whole community.

## Related Projects

- **[FreeDi](https://github.com/Phil1988/FreeDi)** -- Replaces QIDI's stock OS with Armbian and mainline Klipper. Recommended base OS for running HelixScreen on QIDI hardware.
- **[GuppyScreen](https://github.com/ballaswag/guppyscreen)** -- Another LVGL-based touchscreen display for Klipper printers.
- **[KlipperScreen](https://github.com/KlipperScreen/KlipperScreen)** -- Python/GTK-based display interface (typically requires an external monitor).
