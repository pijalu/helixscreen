# Settings: Display & Sound

The Display & Sound category covers visual preferences, display configuration, and audio settings.

---

## Appearance

### Language

Choose the display language for all UI text. Currently English is the only supported language; more translations are planned.

### Timezone

Set your local timezone so that print completion times, ETAs, and the clock widget display the correct time. Most printers default to UTC. Select from a curated list of common timezones — each entry shows the UTC offset (e.g., "Eastern (-5:00)"). DST is handled automatically using your system's timezone database.

**Path:** Settings > Display & Sound > Timezone

### Time Format

Choose between 12-hour and 24-hour clock display. Affects all timestamps shown throughout the UI.

### Animations

Toggle UI motion effects (transitions, panel slides, confetti). Disable for better performance on slower hardware like Raspberry Pi 3.

### Widget Labels

Toggle labels on home panel widgets. Disable for a cleaner look on small screens.

### Bed Mesh Render

Choose how bed mesh data is visualized: Auto, 3D View, or 2D Heatmap.

---

## Display

### Dark Mode

Switch between light and dark themes. Disabled when the active theme doesn't support both modes.

### Theme Colors

Open the theme explorer to browse, preview, and apply color themes.

### Brightness

Slider from 10–100%. Only shown on hardware with backlight control (hidden on Android).

### Screen Dim

When the screen dims to lower brightness: Never, 30s, 1m, 2m, or 5m of inactivity.

### Display Sleep

When the screen turns off completely: Never, 1m, 5m, 10m, or 30m of inactivity.

### Screensaver

Choose a screensaver to display during inactivity instead of turning the screen off.

### Sleep While Printing

Allow the display to sleep during active prints. Off by default so you can monitor progress.

> **Tip:** Touch the screen to wake from sleep.

---

## Sound

### Sounds

Master toggle — turns all sounds on or off. All other sound options are hidden when sounds are disabled.

> Sound options appear as soon as HelixScreen starts — you don't need to wait for the printer to connect. If no audio hardware is detected after connection, the sound options are hidden entirely.

### Volume

Master volume slider (0–100%). Only shown when sounds are enabled.

### UI Sounds

Controls button taps, navigation, and toggle sounds. When off, only important sounds still play (print complete, errors, alarms).

### Sound Theme

Choose sound style. A test sound plays when you switch themes.

### Sound Themes

HelixScreen comes with three built-in themes:

| Theme | Description |
|-------|-------------|
| **Default** | Balanced, tasteful sounds. Subtle clicks, smooth navigation chirps, and a melodic fanfare when your print completes. |
| **Minimal** | Only plays sounds for important events: print complete, errors, and alarms. No button or navigation sounds at all. |
| **Retro** | 8-bit chiptune style. Punchy square-wave arpeggios, a Mario-style victory fanfare, and buzzy retro alarms. |

Advanced users can create custom sound themes by adding a JSON file to `config/sounds/` on the printer. Custom themes appear automatically in the dropdown — no restart required. See the [Sound System developer docs](../../devel/SOUND_SYSTEM.md#adding-a-new-sound-theme) for the file format.

### What Sounds When

| Event | Sound | When It Plays |
|-------|-------|---------------|
| Button press | Short click | Any button tapped |
| Toggle on | Rising chirp | A switch turned on |
| Toggle off | Falling chirp | A switch turned off |
| Navigate forward | Ascending tone | Opening a screen or overlay |
| Navigate back | Descending tone | Closing an overlay or going back |
| Print complete | Victory melody | Print finished successfully |
| Print cancelled | Descending tone | Print job cancelled |
| Error alert | Pulsing alarm | A significant error occurred |
| Error notification | Short buzz | An error toast appeared |
| Critical alarm | Urgent siren | Critical failure requiring attention |
| Test sound | Short beep | "Test Sound" button in settings |
| Startup | Theme jingle | HelixScreen launches (plays once at startup) |

The first five (button press, toggles, navigation) are **UI sounds** and respect the "UI Sounds" toggle. The rest always play as long as the master toggle is on.

### Supported Hardware

| Hardware | How It Works |
|----------|-------------|
| **Desktop (SDL)** | Full audio synthesis through your computer speakers. Best sound quality. |
| **ALSA (Linux)** | Direct audio output on devices with ALSA sound support. 4-voice polyphony with MOD/MED tracker music support for richer sound themes. |
| **Creality AD5M / AD5M Pro** | Hardware PWM buzzer. Supports different tones and volume levels. |
| **Other Klipper printers** | Beeper commands sent through Moonraker. Requires `[output_pin beeper]` in your Klipper config. Basic beep tones only. |

If no audio hardware is detected, the sound options are hidden entirely.

**Disabling sound entirely:** On some hardware (e.g., Artillery M1 Pro), audio drivers are present but using them causes excessive CPU load. If you experience performance issues with sound enabled, add `"disable_sound": true` to your `settings.json` or start HelixScreen with `--no-sound`. This prevents the audio backend from initializing at all, unlike the Sounds toggle which just mutes playback.

### Sound Troubleshooting

**I don't see Sound options in Settings.**
Your printer doesn't have a detected speaker or buzzer. For Klipper printers, make sure you have `[output_pin beeper]` configured in your `printer.cfg`, then restart HelixScreen.

**Sounds are too quiet or too loud.**
Adjust the Volume slider. Volume also varies by theme — try switching themes.

**Print complete sound doesn't play.**
Make sure the master "Sounds" toggle is on. The "UI Sounds" toggle does not affect print completion sounds.

**Button click sounds are annoying.**
Turn off "UI Sounds". This disables button, toggle, and navigation sounds while keeping important notifications.

**Sounds work on desktop but not on my printer.**
Confirm your printer has audio hardware. For Klipper printers, verify `[output_pin beeper]` is present and correctly configured. Test by sending an `M300` command from the Klipper console.

---

[Back to Settings](../settings.md) | [Next: Printing](printing.md)
