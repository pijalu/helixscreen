# Settings: Printer

The Printer section contains settings for hardware connected to your printer. Items in this section appear or hide dynamically based on what HelixScreen detects from Klipper and Moonraker.

---

## Sensors

> Only shown when sensors are detected.

Tap to open the Sensor Settings overlay. Manage all detected filament sensors by choosing a role for each:

| Role | Behavior |
|------|----------|
| **Runout** | Pauses print when filament runs out |
| **Motion** | Detects filament movement (clog detection) |
| **Ignore** | Sensor present but not monitored |

Other detected sensors (accelerometers, probes, humidity, width, color) are listed as read-only information.

---

## AMS Management

> Only shown when an AMS (Automatic Material System) is detected.

Tap to open Device Operations for quick actions, calibration, and speed settings for multi-material systems. Supports AFC, Happy Hare, ValgACE, and other Klipper-based filament changers.

---

## Spoolman

> Only shown when Spoolman is configured in Moonraker.

Tap to open weight sync settings for Spoolman spool tracking integration.

---

## LED Settings

> Only shown when LED hardware is detected.

Tap to open the full LED configuration overlay. This is a large topic with its own page — see [LED Settings](led-settings.md).

---

## Retraction Settings

> Only shown when firmware retraction (`[firmware_retraction]`) is configured in Klipper.

Tap to open the retraction overlay. Configure G10/G11 firmware retraction parameters. **Changes apply immediately** and can be adjusted mid-print for tuning.

| Parameter | Range | Description |
|-----------|-------|-------------|
| **Enable Retraction** | On/Off | Master toggle for firmware retraction |
| **Retract Length** | 0–6 mm | Amount of filament to retract. 0.4–2 mm for direct drive, 4–6 mm for bowden. |
| **Retract Speed** | 10–80 mm/s | Speed of the retraction movement |
| **Unretract Extra** | 0–1 mm | Extra filament to prime after retraction to compensate for ooze |
| **Unretract Speed** | 10–60 mm/s | Speed of the prime (unretract) movement |

---

## Macro Buttons

Tap to open the Macro Buttons overlay. Configure quick-action buttons and standard macro assignments.

### Quick Buttons

These macros power the buttons you see on the Controls and Filament panels:

| Button | Where it appears | What it does by default |
|--------|-----------------|------------------------|
| **Cooldown** | Preheat widget (when heaters are on), Filament panel | Turns off extruder and bed heaters |
| **Load Filament** | Filament panel | Runs `LOAD_FILAMENT` |
| **Unload Filament** | Filament panel | Runs `UNLOAD_FILAMENT` |
| **Custom Macro 1** | Controls panel | Runs `HELIX_CLEAN_NOZZLE` (label: "Clean Nozzle") |
| **Custom Macro 2** | Controls panel | Runs `HELIX_BED_LEVEL_IF_NEEDED` (label: "Bed Level") |

**Cooldown behavior:** When you preheat a material using the Preheat widget on the home or controls panel, the button automatically switches to **Cool Down** while any heater target is above zero. Tapping it runs your configured cooldown macro. This is especially useful if your cooldown needs to do more than just turn off heaters — for example, turning off chamber heaters, bed fans, or recirculation fans.

You can customize any of these. Each button has a **label** (what the button says) and **G-code** (what it runs when tapped). The cooldown macro can be a simple G-code string or a multi-line sequence:

```
SET_HEATER_TEMPERATURE HEATER=extruder TARGET=0
SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=0
SET_FAN_SPEED FAN=bed_fan SPEED=0
```

For advanced configuration via `settings.json`, see the [default_macros reference](../../CONFIGURATION.md#default_macros).

### Standard Macros

HelixScreen auto-detects common macros from your Klipper configuration (e.g., it recognizes `CLEAN_NOZZLE`, `NOZZLE_CLEAN`, and similar naming patterns). You can override any auto-detected assignment:

| Slot | What it controls | Common auto-detected macros |
|------|------------------|-----------------------------|
| **Load Filament** | Filament load operations | LOAD_FILAMENT, M701 |
| **Unload Filament** | Filament unload operations | UNLOAD_FILAMENT, M702 |
| **Purge** | Nozzle purge/prime | PURGE, PURGE_LINE, LINE_PURGE, PRIME_LINE |
| **Pause** | Print pause | PAUSE, M600 |
| **Resume** | Print resume | RESUME |
| **Cancel** | Print cancel | CANCEL_PRINT |
| **Bed Mesh** | Bed mesh calibration | BED_MESH_CALIBRATE |
| **Bed Level** | Manual bed leveling | BED_SCREWS_ADJUST, SCREWS_TILT_CALCULATE |
| **Clean Nozzle** | Nozzle cleaning | CLEAN_NOZZLE, NOZZLE_CLEAN |
| **Heat Soak** | Chamber heat soak | HEAT_SOAK |

If your printer doesn't have a matching macro, some slots fall back to HelixScreen helper macros (installed via **Settings > Advanced > Install HelixScreen Macros**). Leave a slot empty to disable that function.

### Per-Material Preheat Macros

You can also assign a custom Klipper macro to each material preset (PLA, PETG, ABS, TPU). This is useful when preheating requires more than just setting temperatures — for example, turning on bed fans for ABS or starting a chamber heater.

Configure per-material macros in **Settings > Material Temperatures**. Each material can have:

- **Preheat Macro** — A Klipper macro to run when preheating this material
- **Macro Handles Heating** — If enabled, the macro is responsible for setting temperatures. If disabled, HelixScreen sets temperatures first, then runs the macro as an additional step.

---

[Back to Settings](../settings.md) | [Prev: Appearance](appearance.md) | [Next: Notifications](notifications.md)
