# Label Printing

Print spool labels directly from HelixScreen to a compatible thermal label printer. Labels include spool name, material type, color swatch, temperatures, and a QR code linking back to Spoolman.

---

## Supported Printers

HelixScreen supports four families of label printers:

| Brand | Connection | Models |
|-------|------------|--------|
| **Brother QL** | Network (TCP) or Bluetooth | QL-820NWB, QL-810W, QL-800, PT-*, TD-*, RJ-* |
| **Phomemo** | USB or Bluetooth | M110, M120, M02, Q199, and other M*/Q* series |
| **Niimbot** | Bluetooth only | B21, D11, D110 |
| **MakeID** | Bluetooth only | E1 (advertises as "YichipFPGA-XXXX"), L1, M1 — 9/12/16mm continuous tape |

> **Note:** Bluetooth label printing requires a Bluetooth adapter on your HelixScreen device (Raspberry Pi 4/5 have built-in Bluetooth). Devices without Bluetooth hardware will only see Network and USB options. If Bluetooth is disabled for UART, see the [Bluetooth Setup Guide](bluetooth-setup.md).

---

## Setting Up Your Label Printer

### Step 1: Open Label Printer Settings

1. Go to **Settings** (gear icon in the sidebar)
2. Scroll to **Label Printer**
3. Tap to open the Label Printer settings overlay

### Step 2: Select Connection Type

Choose your printer's connection method:

| Type | When to Use |
|------|-------------|
| **Network** | Brother QL printers connected via Ethernet or WiFi |
| **USB** | Phomemo printers connected via USB cable |
| **Bluetooth** | Any supported printer paired via Bluetooth |

> **Bluetooth** only appears when HelixScreen detects Bluetooth hardware on your device.

### Step 3: Configure Your Printer

**For Network printers:**
1. Enter the printer's IP address
2. Port defaults to 9100 (standard for Brother QL)

**For USB printers:**
1. Connect the printer via USB
2. HelixScreen auto-detects the printer

**For Bluetooth printers:**
1. Turn on your label printer and make sure it's in pairing mode
2. Tap **Scan** to discover nearby printers
3. Select your printer from the dropdown
4. If the printer isn't yet paired, HelixScreen will prompt to pair it (most label printers use "Just Works" pairing — no PIN required)

### Step 4: Choose Label Size

Select the label size that matches your loaded label roll:

**Brother QL sizes:** 29mm, 62mm, 29x90mm, and more (300 DPI)

**Phomemo sizes:** 40x30mm, 50x30mm, and more (203 DPI)

**Niimbot B21 sizes:** 50x30mm, 40x30mm, 50x50mm, 40x20mm, 50x80mm (203 DPI)

**Niimbot D11/D110 sizes:** 12x40mm, 12x22mm, 12x30mm, 12x50mm, 12x60mm, 12x70mm (203 DPI)

**MakeID E1 sizes:** 9x30mm, 12x30mm, 12x40mm, 16x30mm, 16x40mm (203 DPI)

> **Tip:** The size list updates automatically based on the detected printer model. A Niimbot D11 shows different sizes than a B21.

### Step 5: Choose Label Preset

| Preset | Description |
|--------|-------------|
| **Standard** | Full label with spool name, material, color, temps, and QR code |
| **Compact** | Condensed layout for smaller labels |
| **Minimal** | Just the essentials — material name and color |

---

## Printing a Label

1. Navigate to the **Spoolman** panel (via Filament tab)
2. Find the spool you want to label
3. Tap the **Print Label** button
4. The label prints automatically using your configured printer

A toast notification confirms success or shows an error message.

---

## Troubleshooting

### Bluetooth printer not found during scan

- Make sure the printer is powered on and in discoverable mode
- Move the printer closer to the HelixScreen device (Bluetooth range is typically 10 meters)
- Try scanning again — some printers take a few seconds to advertise

### Print says "success" but nothing prints

- Check that the printer has labels loaded and isn't in an error state (paper jam, cover open)
- For Bluetooth printers, try turning the printer off and on, then print again
- Check the label size setting matches the actual label roll loaded in the printer

### "Bluetooth not available" error

- Your device doesn't have Bluetooth hardware, or it's disabled
- On Raspberry Pi, check that Bluetooth is enabled: `bluetoothctl show` should list an adapter
- If Bluetooth is disabled for UART (common in Klipper setups), see the [Bluetooth Setup Guide](bluetooth-setup.md) for how to enable it or add a USB dongle
- The Bluetooth plugin (`libhelix-bluetooth.so`) must be present next to the HelixScreen binary

### "No USB printer detected"

- Unplug and replug the USB cable
- Try a different USB port
- Check that the printer is powered on

### Niimbot printer not connecting

- Niimbot printers use BLE (Bluetooth Low Energy), which requires Bluetooth 4.0 or later
- Make sure no other app (like the Niimbot phone app) is currently connected to the printer — BLE only allows one connection at a time
- Power cycle the printer and try again

### Niimbot first print is slow

- The first print after connecting takes a few extra seconds — HelixScreen needs to initialize the BLE connection and warm up the printer's thermal subsystem
- Subsequent prints are faster because the connection stays alive between jobs

### MakeID printer not working

- MakeID E1 advertises as "YichipFPGA-XXXX" over Bluetooth — look for this name during scanning
- Make sure no other app (like the MakeID phone app) is connected to the printer
- MakeID printers use Bluetooth Classic (RFCOMM), so any Bluetooth adapter that supports standard Bluetooth will work (BLE-only dongles are not required)
- Power cycle the printer and try again if the first attempt fails

---

**Next:** [Calibration & Tuning](calibration.md) | **Prev:** [Filament Management](filament.md) | [Back to User Guide](../USER_GUIDE.md)
