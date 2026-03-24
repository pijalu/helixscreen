#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# touch-diag.sh — Dump touch input diagnostics for debugging
# Usage: ./touch-diag.sh [/dev/input/eventN]

set -e

echo "=== HelixScreen Touch Diagnostics ==="
echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo

# Find the touch device
if [ -n "$1" ]; then
    DEVICE="$1"
else
    # Auto-detect: first event device with ABS_X + ABS_Y
    DEVICE=""
    for ev in /sys/class/input/event*; do
        num=$(basename "$ev" | sed 's/event//')
        abs=$(cat "$ev/device/capabilities/abs" 2>/dev/null || echo "0")
        # Check lowest hex word for bits 0+1 (ABS_X, ABS_Y)
        last=$(echo "$abs" | awk '{print $NF}')
        val=$((16#${last}))
        if [ $((val & 3)) -eq 3 ]; then
            DEVICE="/dev/input/event${num}"
            break
        fi
    done
    if [ -z "$DEVICE" ]; then
        echo "ERROR: No touch device found. Specify one: $0 /dev/input/event0"
        exit 1
    fi
fi

NUM=$(echo "$DEVICE" | grep -o '[0-9]*$')
SYSFS="/sys/class/input/event${NUM}/device"

echo "=== Device ==="
echo "Path: $DEVICE"
echo "Name: $(cat "$SYSFS/name" 2>/dev/null || echo 'unknown')"
echo "Phys: $(cat "$SYSFS/phys" 2>/dev/null || echo 'unknown')"
echo "Properties: $(cat "$SYSFS/properties" 2>/dev/null || echo 'unknown')"
echo

echo "=== Capabilities ==="
echo "ABS: $(cat "$SYSFS/capabilities/abs" 2>/dev/null || echo 'unknown')"
echo "KEY: $(cat "$SYSFS/capabilities/key" 2>/dev/null || echo 'unknown')"
echo "EV:  $(cat "$SYSFS/capabilities/ev" 2>/dev/null || echo 'unknown')"
echo

echo "=== ABS Ranges (via ioctl) ==="
# Use python3 to read EVIOCGABS for each axis
python3 - "$DEVICE" 2>/dev/null <<'PYEOF'
import struct, fcntl, array, sys

ABS_NAMES = {
    0x00: "ABS_X",
    0x01: "ABS_Y",
    0x18: "ABS_PRESSURE",
    0x35: "ABS_MT_POSITION_X",
    0x36: "ABS_MT_POSITION_Y",
    0x37: "ABS_MT_TOOL_TYPE",
    0x39: "ABS_MT_TRACKING_ID",
    0x3a: "ABS_MT_PRESSURE",
    0x30: "ABS_MT_TOUCH_MAJOR",
    0x2f: "ABS_MT_SLOT",
}

EVIOCGABS = lambda axis: 0x80184540 + axis

try:
    fd = open(sys.argv[1], "rb")
except PermissionError:
    print("ERROR: Permission denied. Run with sudo.")
    sys.exit(1)

for axis in sorted(ABS_NAMES.keys()):
    buf = array.array("i", [0] * 6)
    try:
        fcntl.ioctl(fd, EVIOCGABS(axis), buf)
        print(f"  {ABS_NAMES[axis]:25s} value={buf[0]:6d}  min={buf[1]:6d}  max={buf[2]:6d}  fuzz={buf[3]:4d}  flat={buf[4]:4d}  res={buf[5]:4d}")
    except OSError:
        pass

fd.close()
PYEOF

if [ $? -ne 0 ]; then
    echo "  (python3 not available — skipping ioctl ABS dump)"
fi

echo
echo "=== Display Info ==="
# Framebuffer resolution
for fb in /dev/fb*; do
    if [ -e "$fb" ]; then
        fbnum=$(echo "$fb" | grep -o '[0-9]*$')
        xres=$(cat "/sys/class/graphics/fb${fbnum}/virtual_size" 2>/dev/null | cut -d, -f1)
        yres=$(cat "/sys/class/graphics/fb${fbnum}/virtual_size" 2>/dev/null | cut -d, -f2)
        stride=$(cat "/sys/class/graphics/fb${fbnum}/stride" 2>/dev/null || echo "unknown")
        bpp=$(cat "/sys/class/graphics/fb${fbnum}/bits_per_pixel" 2>/dev/null || echo "unknown")
        echo "  $fb: ${xres}x${yres} stride=${stride} bpp=${bpp}"
    fi
done
echo

echo "=== HelixScreen Config ==="
CONFIG="$HOME/helixscreen/config/settings.json"
if [ ! -f "$CONFIG" ]; then
    CONFIG="$HOME/helixscreen/config/helixconfig.json"
fi
if [ -f "$CONFIG" ]; then
    echo "Config: $CONFIG"
    # Extract display and input sections
    python3 -c "
import json, sys
try:
    d = json.load(open('$CONFIG'))
    for section in ['display', 'input']:
        if section in d:
            print(f'  [{section}]')
            print(json.dumps(d[section], indent=4))
except Exception as e:
    print(f'  Error reading config: {e}')
" 2>/dev/null || echo "  (python3 not available)"
else
    echo "  No config file found"
fi

echo
echo "=== All Input Devices ==="
cat /proc/bus/input/devices 2>/dev/null || echo "  /proc/bus/input/devices not available"

echo
echo "=== Live Touch Test (5 seconds) ==="
echo "Touch the screen NOW..."
if command -v timeout >/dev/null 2>&1; then
    timeout 5 hexdump -C "$DEVICE" 2>/dev/null | head -60 || true
elif command -v python3 >/dev/null 2>&1; then
    python3 - "$DEVICE" <<'PYEOF2'
import struct, sys, time, signal

ABS_NAMES = {0x00: "X", 0x01: "Y", 0x35: "MT_X", 0x36: "MT_Y", 0x39: "MT_ID", 0x3a: "MT_P"}
EV_ABS = 0x03
EV_KEY = 0x01
BTN_TOUCH = 0x14a

signal.alarm(5)

try:
    fd = open(sys.argv[1], "rb")
    start = time.time()
    while time.time() - start < 5:
        data = fd.read(16)
        if len(data) < 16:
            break
        sec, usec, typ, code, value = struct.unpack("IIHHi", data)
        if typ == EV_ABS and code in ABS_NAMES:
            print(f"  {ABS_NAMES[code]:6s} = {value}")
        elif typ == EV_KEY and code == BTN_TOUCH:
            print(f"  TOUCH  = {'DOWN' if value else 'UP'}")
except (IOError, KeyboardInterrupt, SystemExit):
    pass
except Exception:
    pass
finally:
    fd.close()
PYEOF2
else
    echo "  (no timeout or python3 — skipping live test)"
fi

echo
echo "=== Done ==="
