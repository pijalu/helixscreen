# Settings: System

The System category covers security, network configuration, privacy settings, and maintenance actions.

---

## Security

Set up a screen lock with a PIN code to prevent unauthorized access to your printer controls. Tap to open the Security overlay.

**When no PIN is set:**

- **Set PIN** — Create a 4–6 digit numeric PIN. You'll be asked to enter it twice to confirm.

**When a PIN is set:**

- **Change PIN** — Update your PIN. You must enter the current PIN first, then enter and confirm the new one.
- **Remove PIN** — Disable the PIN entirely. Requires entering the current PIN for confirmation.
- **Auto-lock** — Toggle automatic screen locking. When enabled, the screen locks after the display sleep timeout. You'll need to enter your PIN to unlock.

When the screen is locked, a full-screen lock overlay appears with a numeric keypad. Enter your PIN and tap the checkmark to unlock. If you enter the wrong PIN, an error message appears briefly. An **Emergency Stop** button remains accessible in the top-right corner of the lock screen while a print is running, so you can always halt the printer in an emergency without unlocking.

The PIN is stored securely as a one-way hash in your settings — the actual digits are never saved in plain text. A factory reset clears all security settings.

---

## Network Settings

> Hidden on Android (the OS manages networking).

Tap to open the Network Settings overlay with a two-column layout:

**Left column — Status:**
- **WiFi** — Toggle on/off, view connection status (SSID, IP address, MAC address, signal strength). Shows a 2.4GHz indicator if your hardware only supports that band.
- **Ethernet** — View connection status (IP address, MAC address) — read-only, no toggle.
- **Test Network** — Verify internet connectivity. Disabled when no network is connected.

**Right column — Available Networks:**
- Scans and lists available WiFi networks with signal strength indicators
- Tap a network to connect (enter password if needed)
- **Add Hidden Network** — Connect to a network that doesn't broadcast its SSID
- **Refresh** button to re-scan (shows a spinner while scanning)

---

## Host

Shows the current Moonraker host address (e.g., `localhost:7125`). Tap to open the **Change Host** dialog where you can enter a new IP address and port to connect to a different printer.

After changing the host, HelixScreen disconnects from the current printer and reconnects to the new one.

---

## Touch Calibration

> Only shown on touchscreen displays that need calibration.

Recalibrate if taps register in the wrong location:

1. Tap **Touch Calibration** in Settings
2. Tap each crosshair target as it appears on screen (3 points)
3. Test that taps land correctly in the verify area
4. Calibration saves automatically

The row description shows "Calibrated" or "Not calibrated" status.

If this option doesn't appear in your Settings, your screen type doesn't normally need calibration. If you still need to force it, see the [Touch Calibration Guide](../touch-calibration.md) for alternative methods including CLI flags and config file options.

---

## Hardware Issues

> Only shown when hardware issues are detected.

Tap to open the Hardware Issues overlay, which lists detected issues:

| Category | Meaning |
|----------|---------|
| **Critical** | Required hardware missing (e.g., no extruder heater) |
| **Warning** | Expected hardware not found (e.g., bed sensor disappeared) |
| **Info** | Newly discovered hardware that wasn't seen before |
| **Session** | Hardware changed since last session |

**Actions for non-critical issues:**

- **Ignore** — Mark as optional (won't warn again even if missing)
- **Save** — Add to expected list (will warn if it disappears later)

Use this when adding or removing hardware to keep HelixScreen's expectations accurate.

---

## Plugins

> Only shown when beta features are enabled (tap Current Version 7 times in About).

View installed plugins and their status. Plugins extend HelixScreen with additional capabilities like custom LED effects, overlays, and integrations.

---

## Share Usage Data (Telemetry)

Toggle anonymous usage telemetry that helps improve HelixScreen. Data collection is completely anonymous — no personal information, printer names, or file names are ever sent.

When enabled, a **View Telemetry Data** row appears below the toggle. Tap it to see exactly what data will be sent. See the [Telemetry & Privacy](../../TELEMETRY.md) documentation for full details on what is and isn't collected.

---

## Restart HelixScreen

Restart the display application. Useful after changing settings that require a restart (like theme changes) or if the UI becomes unresponsive. Shows a brief "Restarting..." toast before the app restarts.

---

## Factory Reset

Clears **all** HelixScreen settings and restarts the Setup Wizard. This resets:

- All appearance, display, and sound settings
- LED configuration
- Printer connection details
- Sensor roles and hardware expectations
- All other preferences

**Does not affect** your Klipper configuration, Moonraker, or any files on the printer itself.

---

[Back to Settings](../settings.md) | [Prev: Safety & Notifications](safety.md) | [Next: Help & About](help-about.md)
