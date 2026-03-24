# Settings: Appearance

The first section in the Settings panel covers visual preferences and display configuration.

---

## Language

Choose the display language for all UI text. Currently English is the only supported language; more translations are planned.

---

## Animations

Toggle UI motion effects (transitions, panel slides, confetti). Disable for better performance on slower hardware like Raspberry Pi 3.

---

## 3D Preview

Enable interactive 3D G-code visualization during prints. When off, only the 2D layer view is available. Disable if your hardware struggles with 3D rendering.

---

## Display Settings

Tap **Display Settings** to open an overlay with detailed display options:

| Setting | Options |
|---------|---------|
| **Dark Mode** | Switch between light and dark themes. Disabled when the active theme doesn't support both modes. |
| **Theme Colors** | Open the theme explorer to browse, preview, and apply color themes. |
| **Brightness** | Slider from 10–100%. Only shown on hardware with backlight control (hidden on Android). |
| **Bed Mesh Render** | Auto, 3D View, or 2D Heatmap visualization for bed mesh data. |
| **Screen Dim** | When the screen dims to lower brightness: Never, 30s, 1m, 2m, or 5m of inactivity. |
| **Display Sleep** | When the screen turns off completely: Never, 1m, 5m, 10m, or 30m of inactivity. |
| **Sleep While Printing** | Allow the display to sleep during active prints. Off by default so you can monitor progress. |
| **Time Format** | 12-hour or 24-hour clock display. |

> **Tip:** Touch the screen to wake from sleep.

### Display Rotation

On first boot, HelixScreen automatically detects your display orientation. It cycles through rotation options — tap the screen when the text appears right-side up, then tap again to confirm. The setting is saved and touch coordinates adjust automatically.

To change rotation manually, edit `display.rotate` in `settings.json` (values: `0`, `90`, `180`, `270`) and restart HelixScreen. To re-run automatic detection, remove both `rotate` and `rotation_probed` from the `display` section and restart.

> **Note:** Rotation is not exposed in the Settings UI because it requires restarting all three binaries (main app, splash screen, watchdog).

### Layout Auto-Detection

HelixScreen automatically selects the best layout for your display size:

| Layout | Resolution | Use Case |
|--------|-----------|----------|
| **Standard** | 800x480 | Most 7" touchscreens |
| **Ultrawide** | 1920x480 | Bar-style displays |
| **Compact** | 480x320 | Small 3.5" screens |

Override with the `--layout` command-line flag if auto-detection picks the wrong layout.

---

## Home Widgets

Tap **Home Widgets** to configure which widgets appear on your Home Panel dashboard and in what order.

- **Toggle** individual widgets on or off (maximum 10 enabled) — changes appear on the Home Panel in real time as you flip each switch
- **Reorder** by long-pressing the drag handle on any row and dragging it up or down
- Widgets tied to specific hardware (AMS, humidity sensor, probe, etc.) show "(not detected)" and are disabled if that hardware isn't present

### Widget Details

| Widget | Description |
|--------|-------------|
| **Power** | Toggle Moonraker power devices. Only appears if power devices are configured. |
| **Network** | WiFi signal strength or Ethernet status. Disabled by default. |
| **Firmware Restart** | Restart Klipper firmware with one tap. Always shown in SHUTDOWN state, even if disabled. Disabled by default. |
| **AMS Status** | Mini view of multi-material spool slots. Only appears if an AMS/MMU system is detected. |
| **Temperature** | Nozzle temperature with animated heating icon. Tap to open the Temperature panel. |
| **Temperatures** | Stacked nozzle, bed, and chamber temperatures. Tap the chamber row to open the Chamber Temperature panel. Disabled by default. |
| **LED Light** | Quick LED toggle. Long-press for full LED Control Overlay. |
| **Humidity** | Enclosure humidity sensor reading. Only appears if a humidity sensor is detected. |
| **Width Sensor** | Filament width sensor reading. Only appears if a width sensor is detected. |
| **Probe** | Z probe status and offset. Only appears if a probe is configured. |
| **Filament Sensor** | Filament runout detection status. Only appears if a filament sensor is detected. |
| **Fan Speeds** | Part, hotend, and auxiliary fan speeds. Fan icons spin when running. Labels switch to compact abbreviations (P/H/C) when many widgets are active. Tap to open the Fan Control overlay. |
| **Temperature Sensors** | Monitor additional temperature sensors (e.g., chamber, enclosure). Only appears if extra sensors are detected. Disabled by default. |
| **Macro Button 1–4** | Run a configured macro with one tap. Auto-detects and prompts for parameters. Disabled by default. |
| **Notifications** | Pending alerts with severity badge. Tap to open notification history. |

See [Home Panel — Home Widgets](../home-panel.md#home-widgets) for more on widget layout and hardware gating.

---

[Back to Settings](../settings.md) | [Next: Printer Settings](printer.md)
