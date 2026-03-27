# Motion & Positioning

![Motion Panel](../../images/user/controls-motion.png)

---

## Jog Pad

- **X/Y crosshair**: Tap directions to move print head horizontally
- **Z buttons**: Up/down arrows for vertical movement
- **Position display**: Shows current X, Y, Z coordinates

---

## Homing

| Button | Action |
|--------|--------|
| **Home All** | Homes X, Y, and Z axes |
| **Home XY** | Homes only X and Y |
| **Home Z** | Homes only Z (requires X/Y homed first on most printers) |

---

## Jog Modes

The jog pad has three modes that control how far the print head moves per tap. Toggle between modes using the **Fine**, **Coarse**, and **Turbo** buttons below the jog pad.

| Mode | Inner Ring | Outer Ring | Best For |
|------|-----------|------------|----------|
| **Fine** | 0.1mm | 1mm | Precise calibration, Z-offset, first-layer tuning |
| **Coarse** | 1mm | 10mm | General positioning, moving to a specific area |
| **Turbo** | 10mm | 50mm | Rapid movement across the full build plate |

The jog pad has two zones:
- **Inner ring** (closer to center) — smaller movements
- **Outer ring** (near the edge) — larger movements

The Z-axis buttons on the right side follow the same mode — their labels update to show the current step sizes.

> **Tip:** Your selected jog mode is remembered between sessions. If you frequently do calibration work, leave it on Fine; for everyday use, Coarse is the default.

---

## Emergency Stop

The E-Stop button halts all printer motion immediately. By default, requires confirmation to prevent accidental presses. This can be configured in **Settings > Motion**.

---

**Next:** [Filament Management](filament.md) | **Prev:** [Temperature Control](temperature.md) | [Back to User Guide](../USER_GUIDE.md)
