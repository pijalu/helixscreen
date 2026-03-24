# Bluetooth Setup

HelixScreen uses Bluetooth for label printing (Brother QL, Phomemo, Niimbot, MakeID). Most Raspberry Pi and BTT Pi boards have built-in Bluetooth, but it may be disabled if the UART serial port is in use — for example, to communicate with your printer's MCU.

This guide covers how to check your Bluetooth status, enable it if it's disabled, and add Bluetooth via a USB dongle when the built-in adapter can't be freed up.

---

## Quick Check: Is Bluetooth Working?

Run this command on your Pi:

```bash
bluetoothctl show
```

If you see an adapter listed with `Powered: yes`, Bluetooth is ready — no further setup needed.

If you see `No default controller available`, Bluetooth is either not present or disabled. Read on.

---

## Why Bluetooth Might Be Disabled

On Raspberry Pi and BTT Pi boards, the Bluetooth adapter and the hardware UART (`ttyAMA0`) share the same serial interface. By default, the Pi maps `ttyAMA0` to Bluetooth and gives GPIO serial traffic the less-capable "mini UART."

Many Klipper setups override this to give the MCU the full hardware UART for reliable communication:

```
# /boot/config.txt (Raspberry Pi) or /boot/BoardEnv.txt (BTT Pi)
dtoverlay=disable-bt
```

or equivalently:

```
dtoverlay=miniuart-bt
```

The first (`disable-bt`) disables Bluetooth entirely and gives the full UART to GPIO. The second (`miniuart-bt`) moves Bluetooth to the mini UART (which often doesn't work reliably for Bluetooth).

---

## Option A: Enable Built-In Bluetooth (No UART Needed)

If you are **not** using the UART GPIO pins for MCU communication (for example, your printer connects via USB), you can simply enable Bluetooth.

### Raspberry Pi (All Models)

1. Edit the boot config:

   ```bash
   sudo nano /boot/firmware/config.txt
   ```

   > **Note:** On older Raspberry Pi OS versions, the file is `/boot/config.txt` instead of `/boot/firmware/config.txt`.

2. Find and comment out or remove the Bluetooth disable line:

   ```
   # dtoverlay=disable-bt      ← comment out with #
   ```

   Also remove `dtoverlay=miniuart-bt` if present.

3. Enable the Bluetooth service:

   ```bash
   sudo systemctl enable hciuart
   sudo systemctl enable bluetooth
   ```

4. Reboot:

   ```bash
   sudo reboot
   ```

5. Verify after reboot:

   ```bash
   bluetoothctl show
   ```

### BTT Pi (v1 / v2)

The BTT Pi uses a similar device tree overlay system but the config file location differs.

1. Edit the board environment file:

   ```bash
   sudo nano /boot/BoardEnv.txt
   ```

2. Find and comment out or remove the Bluetooth disable overlay:

   ```
   # dtoverlay=disable-bt      ← comment out with #
   ```

3. Enable the Bluetooth service:

   ```bash
   sudo systemctl enable hciuart
   sudo systemctl enable bluetooth
   ```

4. Reboot:

   ```bash
   sudo reboot
   ```

5. Verify after reboot:

   ```bash
   bluetoothctl show
   ```

---

## Option B: Add a USB Bluetooth Dongle (UART Still in Use)

If your printer's MCU communicates over UART and you can't give that up, the simplest solution is a **USB Bluetooth dongle**. These are inexpensive, widely available, and work alongside the disabled built-in adapter.

### Recommended Dongles

Most USB Bluetooth 4.0+ dongles work out of the box with Linux. Look for dongles based on these chipsets:

| Chipset | Notes |
|---------|-------|
| **Realtek RTL8761B** | Very common, cheap (~$5-8), BT 5.0, excellent Linux support |
| **Cambridge Silicon Radio (CSR)** | Classic budget option, BT 4.0, widely compatible |
| **Broadcom BCM20702** | Reliable, good BLE support |

> **Tip:** Any dongle marketed as "Linux compatible" or "Bluetooth 4.0+" should work. BLE (Bluetooth Low Energy) support is required for Niimbot printers.

### Setup

1. Plug the USB Bluetooth dongle into any available USB port on your Pi.

2. Check that the system detects it:

   ```bash
   bluetoothctl show
   ```

   You should see an adapter listed. If you have both built-in (disabled) and USB adapters, only the USB one will appear.

3. If the adapter shows `Powered: no`, power it on:

   ```bash
   bluetoothctl power on
   ```

4. Enable Bluetooth to start on boot:

   ```bash
   sudo systemctl enable bluetooth
   ```

5. Verify everything is working:

   ```bash
   bluetoothctl show
   ```

   You should see `Powered: yes` and a valid Bluetooth address.

That's it — no reboot required for USB dongles. HelixScreen will detect the adapter automatically.

### If the Dongle Isn't Detected

Some dongles need a firmware file. Check the kernel log:

```bash
dmesg | grep -i bluetooth
```

If you see firmware errors, install the firmware package:

```bash
sudo apt install bluetooth bluez firmware-realtek
```

Then unplug and replug the dongle.

---

## Verifying Bluetooth Works with HelixScreen

Once Bluetooth is enabled (either built-in or via dongle):

1. Open HelixScreen
2. Go to **Settings > Label Printer**
3. Set **Connection** to **Bluetooth**
4. Tap **Scan** — your label printer should appear in the list

If "Bluetooth" doesn't appear as a connection option, HelixScreen didn't detect an adapter. Double-check with `bluetoothctl show`.

---

## Common Configurations

| Setup | Bluetooth Available? | Action Needed |
|-------|---------------------|---------------|
| Pi with MCU on USB, no UART overlay | Yes (built-in) | None — already works |
| Pi with `disable-bt` for UART MCU | No | Add USB dongle (Option B) |
| Pi with `miniuart-bt` | Unreliable | Remove overlay + add USB dongle (Option B) |
| Pi with no MCU on UART | No (disabled) | Enable built-in (Option A) |
| BTT Pi with `disable-bt` | No | Add USB dongle (Option B) |
| BTT Pi with no UART overlay | Yes (built-in) | None — already works |

---

## Troubleshooting

### `bluetoothctl show` says "No default controller available"

- No Bluetooth adapter is active. Either enable the built-in one (Option A) or plug in a USB dongle (Option B).

### Bluetooth adapter shows but is not powered

```bash
sudo bluetoothctl power on
```

If this doesn't persist across reboots, check that the `bluetooth` service is enabled:

```bash
sudo systemctl enable bluetooth
sudo systemctl start bluetooth
```

### USB dongle detected but scanning finds nothing

- Make sure your label printer is powered on and in discoverable mode.
- Some cheap dongles have poor BLE support. If you're using a Niimbot printer (BLE only), try a dongle with a Realtek RTL8761B chipset.

### Two adapters showing in `bluetoothctl`

If you re-enabled built-in Bluetooth while also having a USB dongle plugged in, you may see two adapters. This is fine — HelixScreen will use whichever is available. To select a specific one:

```bash
bluetoothctl select <MAC_ADDRESS>
```

---

**Next:** [Label Printing](label-printing.md) | **Prev:** [Settings](settings.md) | [Back to User Guide](../USER_GUIDE.md)
