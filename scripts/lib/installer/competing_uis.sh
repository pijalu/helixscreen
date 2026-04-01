#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: competing_uis
# Stop competing screen UIs (GuppyScreen, KlipperScreen, Xorg, stock Creality/FlashForge UI)
#
# Reads: AD5M_FIRMWARE, K1_FIRMWARE, INIT_SYSTEM, PREVIOUS_UI_SCRIPT, SUDO, INSTALL_DIR

# Source guard
[ -n "${_HELIX_COMPETING_UIS_SOURCED:-}" ] && return 0
_HELIX_COMPETING_UIS_SOURCED=1

# Known competing screen UIs to stop
# Includes: GuppyScreen (AD5M/K1), Grumpyscreen (K1/Simple AF), KlipperScreen, FeatherScreen
COMPETING_UIS="guppyscreen GuppyScreen grumpyscreen Grumpyscreen KlipperScreen klipperscreen featherscreen FeatherScreen"

# Record a disabled service for later re-enablement
# Args: $1 = type ("systemd" or "sysv-chmod"), $2 = target (service name or script path)
record_disabled_service() {
    local type="$1"
    local target="$2"
    local entry="${type}:${target}"
    local state_file="${INSTALL_DIR}/config/.disabled_services"

    # Ensure config directory exists
    if [ -n "${INSTALL_DIR:-}" ] && [ ! -d "${INSTALL_DIR}/config" ]; then
        $(file_sudo "${INSTALL_DIR}") mkdir -p "${INSTALL_DIR}/config"
    fi

    # Don't duplicate entries
    if [ -f "$state_file" ] && grep -qF "$entry" "$state_file" 2>/dev/null; then
        return 0
    fi

    echo "$entry" | $(file_sudo "${INSTALL_DIR}/config") tee -a "$state_file" >/dev/null
}

# Stop ForgeX-specific competing UIs (stock FlashForge firmware UI)
stop_forgex_competing_uis() {
    # Stop stock FlashForge firmware UI (AD5M/Adventurer 5M)
    # ffstartup-arm is the startup manager that launches firmwareExe (the stock Qt UI)
    if [ -f /opt/PROGRAM/ffstartup-arm ]; then
        log_info "Stopping stock FlashForge UI..."
        kill_process_by_name firmwareExe ffstartup-arm || true
        found_any=true
    fi
}

# Stop Klipper Mod-specific competing UIs (Xorg, KlipperScreen)
stop_kmod_competing_uis() {
    # Stop Xorg first (required for framebuffer access)
    # Xorg takes over /dev/fb0 layer, preventing direct framebuffer rendering
    if [ -x "/etc/init.d/S40xorg" ]; then
        log_info "Stopping Xorg (Klipper Mod display server)..."
        $SUDO /etc/init.d/S40xorg stop 2>/dev/null || true
        # Disable Xorg init script (non-destructive, reversible)
        $SUDO chmod a-x /etc/init.d/S40xorg 2>/dev/null || true
        record_disabled_service "sysv-chmod" "/etc/init.d/S40xorg"
        # Kill any remaining Xorg processes
        kill_process_by_name Xorg X || true
        found_any=true
    fi

    # Kill python processes running KlipperScreen (common on Klipper Mod)
    # BusyBox ps doesn't support 'aux', use portable approach
    # shellcheck disable=SC2009
    for pid in $(ps -ef 2>/dev/null | grep -E 'KlipperScreen.*screen\.py' | grep -v grep | awk '{print $2}'); do
        log_info "Killing KlipperScreen python process (PID $pid)..."
        $SUDO kill "$pid" 2>/dev/null || true
        found_any=true
    done
}

# Stop stock Creality UI on K1 series (display-server, Monitor, master-server, etc.)
# S99start_app launches the entire stock Creality UI stack
stop_k1_stock_competing_uis() {
    if [ -x /etc/init.d/S99start_app ]; then
        log_info "Stopping stock Creality UI (S99start_app)..."
        /etc/init.d/S99start_app stop 2>/dev/null || true
        # Disable so it doesn't restart on reboot (reversible)
        chmod a-x /etc/init.d/S99start_app 2>/dev/null || true
        record_disabled_service "sysv-chmod" "/etc/init.d/S99start_app"
        found_any=true
    fi

    # Kill any remaining stock Creality UI processes
    for proc in display-server Monitor master-server audio-server wifi-server app-server upgrade-server web-server; do
        if kill_process_by_name "$proc"; then
            log_info "Killed remaining $proc process"
            found_any=true
        fi
    done

    # S99start_app also manages dropbear (SSH) on stock K1 firmware.
    # Disabling it kills SSH on next reboot (#535). Ensure SSH survives.
    ensure_k1_ssh
}

# Ensure SSH (dropbear) is running and will start on boot.
# On stock K1 firmware, dropbear is managed by S99start_app which we disable.
# This creates an independent dropbear init script so SSH survives reboots.
ensure_k1_ssh() {
    # Already running — nothing to do
    if pidof dropbear >/dev/null 2>&1; then
        # Make sure it has a boot-time init script
        _ensure_dropbear_init_script
        return 0
    fi

    # Try existing init script first
    for script in /etc/init.d/S50dropbear /etc/init.d/S*dropbear*; do
        [ -f "$script" ] || continue
        chmod +x "$script" 2>/dev/null || true
        "$script" start 2>/dev/null || true
        if pidof dropbear >/dev/null 2>&1; then
            log_info "SSH (dropbear) started via $script"
            return 0
        fi
    done

    # No init script or it failed — start dropbear directly
    local dropbear_bin=""
    for bin in /usr/sbin/dropbear /usr/bin/dropbear /sbin/dropbear; do
        if [ -x "$bin" ]; then
            dropbear_bin="$bin"
            break
        fi
    done

    if [ -n "$dropbear_bin" ]; then
        log_info "Starting dropbear directly ($dropbear_bin)..."
        "$dropbear_bin" -R 2>/dev/null || true
        _ensure_dropbear_init_script "$dropbear_bin"
        if pidof dropbear >/dev/null 2>&1; then
            log_info "SSH (dropbear) started successfully"
        else
            log_warn "Failed to start dropbear — SSH may not be available"
        fi
    else
        log_warn "dropbear not found — SSH may not be available"
    fi
}

# Create a minimal dropbear init script if one doesn't exist.
# Ensures SSH starts on boot independently of S99start_app.
# Args: $1 = dropbear binary path (optional, defaults to /usr/sbin/dropbear)
_ensure_dropbear_init_script() {
    local dropbear_path="${1:-/usr/sbin/dropbear}"

    # Check if any dropbear init script already exists
    for script in /etc/init.d/S50dropbear /etc/init.d/S*dropbear*; do
        if [ -f "$script" ] && [ -x "$script" ]; then
            return 0
        fi
    done

    # Create a minimal init script at S50 (before HelixScreen at S99)
    log_info "Creating dropbear init script (S50dropbear)..."
    cat > /etc/init.d/S50dropbear << INITEOF
#!/bin/sh
# Auto-created by HelixScreen to ensure SSH survives S99start_app being disabled
DROPBEAR="${dropbear_path}"
PIDFILE="/var/run/dropbear.pid"
case "\$1" in
    start)
        [ -x "\$DROPBEAR" ] || exit 0
        "\$DROPBEAR" -R -P "\$PIDFILE"
        ;;
    stop)
        [ -f "\$PIDFILE" ] && kill "\$(cat "\$PIDFILE")" 2>/dev/null
        killall dropbear 2>/dev/null || true
        rm -f "\$PIDFILE"
        ;;
    restart)
        \$0 stop
        sleep 1
        \$0 start
        ;;
    *)
        echo "Usage: \$0 {start|stop|restart}"
        exit 1
        ;;
esac
INITEOF
    chmod +x /etc/init.d/S50dropbear
}

# Stop competing screen UIs (GuppyScreen, KlipperScreen, Xorg, etc.)
# Dispatches platform-specific logic, then runs generic UI stopping
stop_competing_uis() {
    # During self-update, competing UIs were already disabled during initial install.
    # Re-running this would chmod -x init scripts that may have been restored or
    # customized by the platform (e.g. ZMOD manages S80guppyscreen) (#314).
    if _is_self_update; then
        log_info "Skipping competing UI check (self-update; already handled at install)"
        return 0
    fi

    log_info "Checking for competing screen UIs..."

    found_any=false

    # Platform-specific competing UI handling
    case "${AD5M_FIRMWARE:-}" in
        forge_x)    stop_forgex_competing_uis ;;
        klipper_mod) stop_kmod_competing_uis ;;
        zmod)
            # ZMOD manages its own init scripts (S80guppyscreen etc.)
            # Do NOT fall through to the generic loop which would chmod -x them
            log_info "ZMOD platform: skipping generic UI disabling (ZMOD-managed)"
            return 0
            ;;
    esac

    # K1 platform: stop stock Creality UI
    case "${K1_FIRMWARE:-}" in
        stock_klipper|guilouz) stop_k1_stock_competing_uis ;;
    esac

    # Snapmaker U1 stock UI
    if pgrep -x "unisrv" >/dev/null 2>&1; then
        log_info "Stopping Snapmaker stock UI (unisrv)..."
        killall unisrv 2>/dev/null || true
        found_any=true
    fi

    # Handle the specific previous UI if we know it (for clean reversibility)
    if [ -n "$PREVIOUS_UI_SCRIPT" ] && [ -x "$PREVIOUS_UI_SCRIPT" ] 2>/dev/null; then
        log_info "Stopping previous UI: $PREVIOUS_UI_SCRIPT"
        $SUDO "$PREVIOUS_UI_SCRIPT" stop 2>/dev/null || true
        # Disable by removing execute permission (non-destructive, reversible)
        $SUDO chmod a-x "$PREVIOUS_UI_SCRIPT" 2>/dev/null || true
        record_disabled_service "sysv-chmod" "$PREVIOUS_UI_SCRIPT"
        found_any=true
    fi

    for ui in $COMPETING_UIS; do
        # Check systemd services
        if [ "$INIT_SYSTEM" = "systemd" ]; then
            if systemctl is-active --quiet "$ui" 2>/dev/null; then
                log_info "Stopping $ui (systemd service)..."
                $SUDO systemctl stop "$ui" 2>/dev/null || true
                $SUDO systemctl disable "$ui" 2>/dev/null || true
                record_disabled_service "systemd" "$ui"
                found_any=true
            fi
        fi

        # Check SysV init scripts (various locations)
        for initscript in /etc/init.d/S*${ui}* /etc/init.d/${ui}* /opt/config/mod/.root/S*${ui}*; do
            # Skip if glob didn't match any files (literal pattern returned)
            [ -e "$initscript" ] || continue
            # Skip if this is the PREVIOUS_UI_SCRIPT we already handled
            if [ "$initscript" = "$PREVIOUS_UI_SCRIPT" ]; then
                continue
            fi
            if [ -x "$initscript" ]; then
                log_info "Stopping $ui ($initscript)..."
                $SUDO "$initscript" stop 2>/dev/null || true
                # Disable by removing execute permission (non-destructive)
                $SUDO chmod a-x "$initscript" 2>/dev/null || true
                record_disabled_service "sysv-chmod" "$initscript"
                found_any=true
            fi
        done

        # Kill any remaining processes by name
        if kill_process_by_name "$ui"; then
            log_info "Killed remaining $ui processes"
            found_any=true
        fi
    done

    if [ "$found_any" = true ]; then
        log_info "Waiting for competing UIs to stop..."
        sleep 2
    else
        log_info "No competing UIs found"
    fi
}
