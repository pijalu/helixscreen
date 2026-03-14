# Creality K1C Setup Guide

Get HelixScreen running on your Creality K1C (also works for K1 and K1 Max).

**Time required:** ~20 minutes

---

## What You Need

- Creality K1C with firmware **v1.3.3.5 or later** (check: Settings on the touchscreen)
- K1C connected to your WiFi network
- A computer on the same network with an SSH client

---

## Step 1: Enable Root Access

On the K1C touchscreen:

1. Go to **Settings** > **Root account information**
2. Read the disclaimer, **wait 30 seconds**, then confirm

That's it. Root is now enabled.

---

## Step 2: SSH Into the Printer

From your computer:

```bash
ssh root@<PRINTER_IP>
# Password: creality_2023
```

Find the IP address on the K1C touchscreen under Settings > Network, or check your router's DHCP client list.

---

## Step 3: Install Simple AF (Recommended) or Guilouz Helper Script

HelixScreen needs Moonraker (the Klipper API server) to be running on your K1C. Stock firmware doesn't include it, so you need community firmware.

### Option A: Simple AF (Recommended)

[Simple AF](https://github.com/pellcorp/creality) is the easiest path and has first-class HelixScreen support.

```bash
cd /usr/data
git clone https://github.com/pellcorp/creality.git pellcorp
cd pellcorp
sh installer.sh
```

Follow the prompts. This installs Klipper, Moonraker, and GuppyScreen. Once it's working (GuppyScreen appears on the touchscreen), proceed to Step 4.

### Option B: Guilouz Helper Script

If you prefer the [Guilouz Helper Script](https://github.com/Guilouz/Creality-Helper-Script-Wiki):

```bash
git clone --depth 1 https://github.com/Guilouz/Creality-Helper-Script.git /usr/data/helper-script
sh /usr/data/helper-script/helper.sh
```

From the menu, install **Moonraker** at minimum. Optionally install Fluidd or Mainsail for web access.

> **Note:** If git clone fails with an SSL error, run `git config --global http.sslVerify false` first.

---

## Step 4: Install HelixScreen

With Moonraker running, install HelixScreen with one command:

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh
```

The installer automatically:
- Detects your K1C platform
- Downloads the correct MIPS binary
- Stops GuppyScreen (or whatever screen UI is running)
- Installs to `/usr/data/helixscreen/`
- Creates the boot service (`/etc/init.d/S99helixscreen`)
- Configures Moonraker update manager for easy future updates

---

## Step 5: Complete Setup on the Touchscreen

HelixScreen starts automatically after install. The on-screen wizard walks you through:

1. **Language** selection
2. **Moonraker connection** — should auto-detect on `localhost:7125`
3. **Printer identification** — the K1C is in the printer database
4. **Hardware discovery** — heaters, fans, sensors, LEDs

---

## Managing HelixScreen

```bash
# Start/stop/restart
/etc/init.d/S99helixscreen start
/etc/init.d/S99helixscreen stop
/etc/init.d/S99helixscreen restart

# View logs
cat /tmp/helixscreen.log
tail -f /tmp/helixscreen.log    # live follow
```

---

## Updating

### From Fluidd/Mainsail (easiest)

If you have Fluidd or Mainsail installed, HelixScreen appears in the update manager. One-click update.

### From SSH

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --update
```

---

## Uninstalling

```bash
curl -sSL https://raw.githubusercontent.com/prestonbrown/helixscreen/main/scripts/install.sh | sh -s -- --uninstall
```

This restores GuppyScreen automatically.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| Can't SSH in | Make sure root is enabled (Step 1). Password is `creality_2023` |
| `curl` not found | Use `wget -O - https://...install.sh \| sh` instead |
| Installer says "Moonraker not found" | Complete Step 3 first — Moonraker must be running |
| Blank screen after install | Check logs: `cat /tmp/helixscreen.log` |
| Touch not responding | Reboot: `reboot` |

For more help: [Troubleshooting Guide](../TROUBLESHOOTING.md) | [Discord](https://discord.gg/RZCT2StKhr) | [GitHub Issues](https://github.com/prestonbrown/helixscreen/issues)

---

## Firmware Upgrade Path

If your K1C is on older firmware, you must upgrade in order:

| Starting Version | Upgrade To |
|-----------------|------------|
| v1.2.9.14 or earlier | v1.2.9.15 → v1.2.9.22 → v1.3.0.30 → v1.3.1.4 → latest |
| v1.2.9.17 – v1.2.9.21 | v1.2.9.22 → v1.3.0.30 → v1.3.1.4 → latest |
| v1.3.0.30 | v1.3.1.4 → latest |
| v1.3.1.4+ | Latest directly |

Download firmware from Creality's site, put the `.img` on a FAT32 USB drive, insert into the printer, and follow the on-screen prompts. **Factory reset from Settings before installing rooted firmware.**

> **After factory reset**, root access must be re-enabled (Step 1) and community firmware reinstalled (Step 3).
