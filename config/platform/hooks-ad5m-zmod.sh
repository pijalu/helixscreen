#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Platform hooks: AD5M / AD5M Pro with ZMOD firmware
#
# ZMOD provides Klipper, Moonraker, and SSH access on FlashForge AD5 series
# printers. Unlike ForgeX, ZMOD does not use a chroot or custom display
# management scripts. S80guppyscreen is managed by ZMOD — do NOT modify it.
#
# Key coordination point: /tmp/helixscreen_active
#   Used to signal that HelixScreen owns the display framebuffer.

# shellcheck disable=SC3043  # local is supported by BusyBox ash

# Stop competing screen UIs so HelixScreen can access the framebuffer.
# ZMOD does not run stock FlashForge UI (ffstartup-arm) or Xorg.
platform_stop_competing_uis() {
    # Stop known competing UIs via init scripts and process kill
    for ui in guppyscreen GuppyScreen KlipperScreen klipperscreen featherscreen FeatherScreen; do
        for initscript in /etc/init.d/S*"${ui}"*; do
            if [ -x "$initscript" ] 2>/dev/null; then
                echo "Stopping competing UI: $initscript"
                "$initscript" stop 2>/dev/null || true
            fi
        done
        if command -v killall >/dev/null 2>&1; then
            killall "$ui" 2>/dev/null || true
        else
            for pid in $(pidof "$ui" 2>/dev/null); do
                kill "$pid" 2>/dev/null || true
            done
        fi
    done

    # Brief pause to let processes exit
    sleep 1
}

# ZMOD does not require special backlight control.
# The display backlight is managed by the kernel/hardware.
platform_enable_backlight() {
    return 0
}

# ZMOD systems do not need Moonraker wait — memory is not as constrained
# as ForgeX and there is no boot sequence contention.
platform_wait_for_services() {
    return 0
}

# Pre-start setup: set the active flag so other services know HelixScreen
# owns the display.
platform_pre_start() {
    export HELIX_CACHE_DIR="/data/helixscreen/cache"
    touch /tmp/helixscreen_active
}

# Post-stop cleanup: remove the active flag.
platform_post_stop() {
    rm -f /tmp/helixscreen_active
}
