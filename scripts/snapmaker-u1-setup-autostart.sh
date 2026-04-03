#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Set up HelixScreen auto-start on Snapmaker U1
# Run on the device: ./snapmaker-u1-setup-autostart.sh /userdata/helixscreen
#
# This script:
# 1. Creates /oem/.debug to prevent overlay wipe on boot
# 2. Patches /etc/init.d/S99screen to start HelixScreen instead of stock GUI
#
# To revert: rm -rf /userdata/helixscreen && reboot
# (S99screen falls back to stock GUI when HelixScreen is not installed)

set -e

DEPLOY_DIR="${1:-/userdata/helixscreen}"

INIT_SCRIPT="$DEPLOY_DIR/config/helixscreen.init"
if [ ! -f "$INIT_SCRIPT" ]; then
    echo "Error: $INIT_SCRIPT not found"
    echo "Deploy HelixScreen first, then run this script"
    exit 1
fi

# Step 1: Create /oem/.debug to prevent overlay wipe on boot
# Without this, S01aoverlayfs runs: rm -rf /oem/overlay/*
if [ ! -f /oem/.debug ]; then
    touch /oem/.debug
    echo "Created /oem/.debug (overlay persistence enabled)"
else
    echo "/oem/.debug already exists"
fi

# Step 2: Check if S99screen is already patched
if head -5 /etc/init.d/S99screen 2>/dev/null | grep -q "HelixScreen"; then
    echo "S99screen already patched for HelixScreen"
    exit 0
fi

# Step 3: Patch S99screen to delegate to HelixScreen when installed
cat > /etc/init.d/S99screen << 'PATCH'
#!/bin/sh
#
# Start/stop GUI process
# Modified by HelixScreen: delegates to HelixScreen init when installed
#

GUI="/usr/bin/gui"
PIDFILE=/var/run/gui.pid

log()
{
	logger -p user.info -t "GUI[$$]" -- "$1"
	echo "$1"
}

# If HelixScreen is installed, delegate to its init script
for helix_init in /userdata/helixscreen/config/helixscreen.init /opt/helixscreen/config/helixscreen.init; do
    if [ -x "$helix_init" ]; then
        case "$1" in
          start)
            log "HelixScreen detected, starting instead of stock GUI"
            "$helix_init" start
            ;;
          stop)
            "$helix_init" stop
            ;;
          restart)
            "$helix_init" stop
            sleep 1
            "$helix_init" start
            ;;
          *)
            echo "Usage: $0 {start|stop|restart}"
            exit 1
        esac
        exit 0
    fi
done

# Stock GUI fallback (no HelixScreen installed)
case "$1" in
  start)
	log "Starting GUI process..."
	ulimit -c 102400
	start-stop-daemon -S -b -x "$GUI" -m -p "$PIDFILE"
	;;
  stop)
	log "Stopping GUI process..."
	start-stop-daemon -K -x "$GUI" -p "$PIDFILE" -o
	;;
  restart)
	"$0" stop
	sleep 1
	"$0" start
	;;
  *)
	echo "Usage: $0 {start|stop|restart}"
	exit 1
esac
PATCH

chmod +x /etc/init.d/S99screen
echo "S99screen patched — HelixScreen will auto-start on boot"
echo "To revert: rm -rf $DEPLOY_DIR && reboot"
