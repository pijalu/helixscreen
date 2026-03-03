# Advanced Features

![Advanced Panel](../../images/user/advanced.png)

Access via the **More** icon in the navigation bar.

---

## G-code Console

![Console Panel](../../images/user/advanced-console.png)

A full-featured G-code terminal for sending commands directly to your printer and viewing Klipper responses in real time.

**Opening the console:**

- Navigate to **Advanced > G-code Console**, or
- Add the **G-code Console** widget to your home panel for one-tap access

**Sending commands:**

1. Type a G-code command in the input field at the bottom (e.g., `G28`, `M104 S210`)
2. Press **Enter** on the keyboard or tap the **send button**
3. The command appears with a `>` prefix, and Klipper's response streams in below

**Command history:**

- Press **Up/Down arrow keys** to recall previously sent commands
- Up to 20 recent commands are remembered within the session

**Color coding:**

- **White**: Commands you sent (prefixed with `>`)
- **Green**: Successful responses from Klipper
- **Red**: Errors and warnings (lines starting with `!!` or `Error`)
- **Colored spans**: AFC and Happy Hare plugins send colored output that renders inline

**Other features:**

- **Auto-scroll**: The console scrolls to show new messages automatically. Scroll up to pause auto-scroll and read history — it resumes when you send a new command
- **Timestamps**: On medium and larger screens, each line shows an `HH:MM:SS` timestamp
- **Clear button**: Tap the trash icon to clear the display (with confirmation)
- **Monospace font**: Console text uses Source Code Pro for easier reading of G-code output
- Temperature status messages (`T:210.0 /210.0 B:60.0 /60.0`) are automatically filtered out to reduce noise

---

## Macro Execution

![Macro Panel](../../images/user/advanced-macros.png)

Execute custom Klipper macros:

1. Navigate to **Advanced > Macros**
2. Browse available macros (alphabetically sorted)
3. Tap a macro to execute

**Notes:**

- System macros (starting with `_`) are hidden by default
- Names are prettified: `CLEAN_NOZZLE` becomes "Clean Nozzle"
- Dangerous macros (`SAVE_CONFIG`, etc.) require confirmation

---

## Power Device Control

Control Moonraker power devices from the full power panel or the home panel quick-toggle button.

### Home Panel Quick Toggle

A **power-cycle button** appears on the home panel when power devices are configured:

- **Tap** to toggle your selected power devices on or off
- **Long-press** to open the full power panel overlay
- The button shows a **danger (red) variant** when devices are on, and **muted** when off

### Full Power Panel

1. Navigate to **Advanced > Power Devices**, or **Settings > System > Power Devices** (hidden when no power devices are detected)
2. Toggle individual devices on/off with switches

**Main Power Button section:**

At the top of the power panel, a **"Main Power Button"** section lets you choose which devices the home panel quick-toggle controls:

- Selection chips appear for each discovered power device
- Tap chips to include or exclude devices from the home button
- Your selection is saved automatically

### Auto-Discovery

HelixScreen automatically discovers power devices from Moonraker when it connects to your printer. On first discovery, all devices are selected for the home panel button by default. The **Power Devices** row in the Advanced panel is hidden when no power devices are available.

**Notes:**

- Devices may be locked during prints (safety feature)
- Lock icon indicates protected devices

---

## Print History

![Print History](../../images/user/advanced-history.png)

View past print jobs:

**Dashboard view:**

- Total prints, success rate
- Print time and filament usage statistics
- Trend graphs over time

**List view:**

- Search by filename
- Filter by status (completed, failed, cancelled)
- Sort by date, duration, or name

**Detail view:**

- Tap any job for full details
- **Reprint**: Start the same file again
- **Delete**: Remove from history

---

## Notification History

Review past system notifications:

1. Tap the **bell icon** in the status bar
2. Scroll through history
3. Tap **Clear All** to dismiss

**Color coding:**

- Blue: Info
- Yellow: Warning
- Red: Error

---

## Timelapse Settings

Configure Moonraker-Timelapse (beta feature):

1. Navigate to **Advanced > Timelapse**
2. If the timelapse plugin is not installed, HelixScreen detects this and offers an **Install Wizard** to set it up
3. Once installed, configure settings:
   - Enable/disable timelapse recording
   - Select mode: **Layer Macro** (snapshot at each layer) or **Hyperlapse** (time-based)
   - Set framerate (15/24/30/60 fps)
   - Enable auto-render for automatic video creation

### Render Controls

Below the settings, a **Render Now** section shows:

- **Frame count**: How many frames have been captured during the current print
- **Render progress bar**: Appears during rendering with a percentage indicator
- **Render Now button**: Manually trigger video rendering from captured frames

### Recorded Videos

The bottom of the timelapse settings shows all rendered timelapse videos:

- View file names and sizes
- Delete individual videos (with confirmation)
- Videos are stored on your printer and managed by the timelapse plugin

### Notifications

During rendering, HelixScreen shows toast notifications:

- Progress updates at 25%, 50%, 75%, and 100%
- Success notification when rendering completes
- Error notification if rendering fails

---

**Next:** [Beta Features](beta-features.md) | **Prev:** [Settings](settings.md) | [Back to User Guide](../USER_GUIDE.md)
