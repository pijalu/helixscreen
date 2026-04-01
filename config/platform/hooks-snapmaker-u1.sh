#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: Snapmaker U1 (Extended Firmware)
#
# The Snapmaker U1 runs Debian Trixie with its own touchscreen UI application.
# HelixScreen uses the DRM backend for double-buffered page flipping.
#
# DRM CRTC keepalive: The U1's display is driven by a Rockchip DRM/KMS
# pipeline (rockchipdrmfb). When the stock UI process exits, the kernel's
# VOP2 driver disables the CRTC, leaving the display permanently black.
# To prevent this, we spawn a background process that holds /dev/dri/card0
# open while we kill the stock UI. Once HelixScreen opens the DRM device
# itself, the keepalive process exits — but the CRTC stays active because
# HelixScreen now has its own fd on the device.
#
# Stock UI: /usr/bin/gui (started by S99screen init script)
# Camera: /usr/bin/lmd (started by S90lmd)
# Touch: TLSC6x capacitive controller (tlsc6x_touch on /dev/input/event0)
# Display: 480x320 32bpp rockchipdrmfb (/dev/fb0)
# SSH access: root@<ip> (password: snapmaker) via extended firmware

# PID of the background keepalive process
DRM_KEEPALIVE_PID=""

# Stop Snapmaker's stock touchscreen UI so HelixScreen can access the display.
#
# CRITICAL: We must hold /dev/dri/card0 open before killing the stock UI.
# Without this, the VOP2 driver disables the CRTC and the display goes
# permanently black until reboot.
platform_stop_competing_uis() {
    # Spawn a background process that holds the DRM device open.
    # It stays alive until helix-screen opens /dev/dri/card0 itself (detected
    # via /proc/*/fd), or for a maximum of 30 seconds as a safety timeout.
    if [ -e /dev/dri/card0 ]; then
        (
            # Hold the device open via our own fd
            exec 3>/dev/dri/card0
            echo "DRM keepalive: holding /dev/dri/card0 (pid $$)"
            # Wait until helix-screen has the device open, or timeout
            elapsed=0
            while [ "$elapsed" -lt 30 ]; do
                for pid_dir in /proc/[0-9]*/fd; do
                    pid=$(echo "$pid_dir" | sed 's|/proc/\([0-9]*\)/fd|\1|')
                    comm=$(cat "/proc/$pid/comm" 2>/dev/null) || continue
                    case "$comm" in
                        helix-screen)
                            if ls -l "$pid_dir" 2>/dev/null | grep -q '/dev/dri/card0'; then
                                echo "DRM keepalive: helix-screen (pid $pid) has /dev/dri/card0, releasing"
                                exit 0
                            fi
                            ;;
                    esac
                done
                sleep 1
                elapsed=$((elapsed + 1))
            done
            echo "DRM keepalive: timeout after 30s, releasing"
        ) &
        DRM_KEEPALIVE_PID=$!
        echo "DRM keepalive: background process PID $DRM_KEEPALIVE_PID"
    fi

    # Kill stock UI and camera processes directly.
    # NOTE: Do NOT call /etc/init.d/S99screen stop — on the U1, S99screen is
    # patched to delegate to helixscreen.init, which would cause infinite recursion.
    for ui in gui unisrv lmd snapmaker-ui snapmaker-screen KlipperScreen klipperscreen; do
        if command -v killall >/dev/null 2>&1; then
            killall "$ui" 2>/dev/null || true
        else
            for pid in $(pidof "$ui" 2>/dev/null); do
                kill "$pid" 2>/dev/null || true
            done
        fi
    done

    # Kill python-based KlipperScreen if running
    # shellcheck disable=SC2009
    for pid in $(ps aux 2>/dev/null | grep -E 'python.*screen\.py' | grep -v grep | awk '{print $2}'); do
        echo "Killing KlipperScreen python process (PID $pid)"
        kill "$pid" 2>/dev/null || true
    done

    # Brief pause to let processes settle
    sleep 1
}

# The U1 display backlight is managed by the kernel/hardware.
platform_enable_backlight() {
    return 0
}

# Debian Trixie manages services via systemd - Klipper/Moonraker should be
# available by the time HelixScreen starts.
platform_wait_for_services() {
    return 0
}

platform_pre_start() {
    export HELIX_CACHE_DIR="/userdata/helixscreen/cache"
    # Force DRM device — skip auto-detection which may race with connector state
    export HELIX_DRM_DEVICE="/dev/dri/card0"
    return 0
}

platform_post_stop() {
    # Kill the keepalive process if still running
    if [ -n "$DRM_KEEPALIVE_PID" ]; then
        kill "$DRM_KEEPALIVE_PID" 2>/dev/null || true
        wait "$DRM_KEEPALIVE_PID" 2>/dev/null || true
        echo "DRM keepalive: cleaned up process $DRM_KEEPALIVE_PID"
        DRM_KEEPALIVE_PID=""
    fi

    # Restart stock GUI directly (not via S99screen, which may delegate back to us)
    if [ -x /usr/bin/gui ]; then
        start-stop-daemon -S -b -x /usr/bin/gui -m -p /var/run/gui.pid 2>/dev/null || true
    fi
    return 0
}
