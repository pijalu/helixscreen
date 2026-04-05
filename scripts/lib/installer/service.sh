#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Module: service
# Service installation and management (systemd and SysV)
#
# Reads: INIT_SYSTEM, INSTALL_DIR, INIT_SCRIPT_DEST, SERVICE_NAME, SUDO
# Writes: CLEANUP_SERVICE

# Source guard
[ -n "${_HELIX_SERVICE_SOURCED:-}" ] && return 0
_HELIX_SERVICE_SOURCED=1

# SERVICE_NAME is defined in common.sh

# Portable in-place sed: GNU sed uses -i, BSD/macOS sed requires -i ''
_sed_inplace() {
    local pattern=$1 file=$2
    $SUDO sed -i "$pattern" "$file" 2>/dev/null || \
    $SUDO sed -i '' "$pattern" "$file" 2>/dev/null || true
}

# Returns true if this process is running under the NoNewPrivileges systemd constraint.
# When helix-screen self-updates, it spawns install.sh as a child process.  The
# helixscreen.service unit has NoNewPrivileges=true, so ALL sudo calls in install.sh
# will fail.  Callers use this to skip operations that require root (service file
# copy, daemon-reload, systemctl start) and instead let update_checker.cpp restart
# the process via exit(0), which the watchdog treats as "restart silently".
_has_no_new_privs() {
    # Primary: check /proc/self/status (kernel 4.10+)
    if [ -r /proc/self/status ] && grep -q '^NoNewPrivs:[[:space:]]*1' /proc/self/status 2>/dev/null; then
        return 0
    fi
    # Fallback for older kernels (e.g. SonicPad 4.9): if we know this is a
    # self-update spawned from the running app, the service unit's
    # NoNewPrivileges=true is inherited — sudo will fail.
    _is_self_update
}

# _is_self_update() is defined in common.sh (sourced before this module)

# Install service (dispatcher)
# Calls install_service_systemd or install_service_sysv based on INIT_SYSTEM
install_service() {
    local platform=$1

    if [ "$platform" = "snapmaker-u1" ]; then
        install_service_snapmaker_u1
        return
    fi

    if [ "$INIT_SYSTEM" = "systemd" ]; then
        install_service_systemd
    else
        install_service_sysv
    fi
}

install_service_snapmaker_u1() {
    log_info "Configuring Snapmaker U1 autostart..."

    # --- Fix DAEMON_DIR in the init script (U1 keeps it in-place, not /etc/init.d/) ---
    local init_script="${INSTALL_DIR}/config/helixscreen.init"
    if [ ! -f "$init_script" ]; then
        log_error "Init script not found: $init_script"
        exit 1
    fi
    chmod +x "$init_script"
    _sed_inplace "s|DAEMON_DIR=.*|DAEMON_DIR=\"${INSTALL_DIR}\"|" "$init_script"

    # --- Patch S99screen to delegate to helixscreen.init on boot ---
    if [ -f "${INSTALL_DIR}/scripts/snapmaker-u1-setup-autostart.sh" ]; then
        if ! bash "${INSTALL_DIR}/scripts/snapmaker-u1-setup-autostart.sh" "${INSTALL_DIR}"; then
            log_error "Snapmaker U1 autostart configuration failed."
            log_error "Ensure /oem and /etc/init.d are writable."
            exit 1
        fi
    else
        log_error "Snapmaker U1 autostart script not found at ${INSTALL_DIR}/scripts/snapmaker-u1-setup-autostart.sh"
        log_error "The release package may be incomplete."
        exit 1
    fi
}

# Install systemd service
install_service_systemd() {
    # During self-update, the main service file is already installed and may
    # contain platform customizations (#314) — don't overwrite it.
    # BUT: always update the update-watcher units (path + oneshot service).
    # They have no platform customizations and older versions had a PathExists
    # bug that causes an infinite restart loop after any update.
    if _is_self_update; then
        log_info "Skipping main service file install (self-update; preserving customizations)"
        update_watcher_if_stale
        CLEANUP_SERVICE=true
        return 0
    fi

    log_info "Installing systemd service..."

    local service_src="${INSTALL_DIR}/config/helixscreen.service"
    local service_dest="/etc/systemd/system/${SERVICE_NAME}.service"

    if [ ! -f "$service_src" ]; then
        log_error "Service file not found: $service_src"
        log_error "The release package may be incomplete."
        exit 1
    fi

    # Under NoNewPrivileges (self-update spawned by helix-screen), sudo is blocked and
    # /etc/systemd/system/ is read-only in the service's mount namespace.  The service
    # is already installed and correct — skip reinstall.  The process restart is handled
    # by update_checker.cpp calling exit(0) after install.sh succeeds.
    if _has_no_new_privs; then
        if [ -f "$service_dest" ]; then
            log_info "Skipping service reinstall (NoNewPrivileges; already installed)"
            CLEANUP_SERVICE=true
            return 0
        fi
        log_error "Service not installed and NoNewPrivileges prevents installation"
        exit 1
    fi

    $SUDO cp "$service_src" "$service_dest"

    # Template placeholders (match SysV pattern in install_service_sysv)
    local helix_user="${KLIPPER_USER:-root}"
    local helix_group="${KLIPPER_USER:-root}"
    local install_dir="${INSTALL_DIR:-/opt/helixscreen}"

    local install_parent
    install_parent="$(dirname "${install_dir}")"

    _sed_inplace "s|@@HELIX_USER@@|${helix_user}|g" "$service_dest"
    _sed_inplace "s|@@HELIX_GROUP@@|${helix_group}|g" "$service_dest"
    _sed_inplace "s|@@INSTALL_DIR@@|${install_dir}|g" "$service_dest"
    _sed_inplace "s|@@INSTALL_PARENT@@|${install_parent}|g" "$service_dest"

    if ! $SUDO systemctl daemon-reload; then
        log_error "Failed to reload systemd daemon."
        exit 1
    fi

    # Install update watcher (restarts helixscreen after Moonraker web-type update)
    # Workaround for mainsail-crew/mainsail#2444: type: web lacks managed_services
    install_update_watcher_systemd

    CLEANUP_SERVICE=true
    log_success "Installed systemd service"
}

# During self-update, check if the deployed update-watcher path unit has the
# old PathExists directive (which causes infinite restart loops) and fix it.
# The watcher units have no platform customizations — only @@INSTALL_DIR@@.
# Under NoNewPrivileges we can't write to /etc/systemd/system/ or run
# systemctl daemon-reload, so we rely on helixscreen-update.service's
# ExecStartPre to refresh the main service file on next Moonraker update.
# However, the PATH unit itself won't get refreshed that way, so we attempt
# a direct fix here and fall back to a warning if permissions block it.
update_watcher_if_stale() {
    local path_dest="/etc/systemd/system/helixscreen-update.path"
    local svc_dest="/etc/systemd/system/helixscreen-update.service"

    # Only relevant on systemd with the watcher installed
    [ "$INIT_SYSTEM" = "systemd" ] || return 0
    [ -f "$path_dest" ] || return 0

    # Detect staleness: PathExists bug (restart loop), missing sentinel check
    # (self-update double-trigger #536), or missing refresh-service-units.sh.
    local stale=false reason=""
    if grep -q '^PathExists=' "$path_dest" 2>/dev/null; then
        stale=true reason="PathExists bug (restart loop)"
    elif [ -f "$svc_dest" ] && ! grep -q 'self_restart_sentinel' "$svc_dest" 2>/dev/null; then
        stale=true reason="missing self-update sentinel check (#536)"
    elif [ -f "$svc_dest" ] && ! grep -q 'refresh-service-units' "$svc_dest" 2>/dev/null; then
        stale=true reason="missing service file refresh"
    fi

    if ! $stale; then
        log_info "Update watcher units are current"
        return 0
    fi

    log_warn "Stale update watcher units: $reason"

    local path_src="${INSTALL_DIR}/config/helixscreen-update.path"
    local svc_src="${INSTALL_DIR}/config/helixscreen-update.service"
    local install_dir="${INSTALL_DIR:-/opt/helixscreen}"
    local install_parent
    install_parent="$(dirname "$install_dir")"

    if _has_no_new_privs; then
        # Can't write to /etc/systemd/system/ under NoNewPrivileges.
        # Attempt in-place fixes — may fail under ProtectSystem=strict.
        local fixed=false
        if grep -q '^PathExists=' "$path_dest" 2>/dev/null; then
            sed -i 's/^PathExists=/PathChanged=/' "$path_dest" 2>/dev/null && fixed=true
        fi
        if [ -f "$svc_src" ] && [ -f "$svc_dest" ]; then
            if cp "$svc_src" "$svc_dest" 2>/dev/null; then
                sed -i -e "s|@@INSTALL_DIR@@|${install_dir}|g" \
                       -e "s|@@INSTALL_PARENT@@|${install_parent}|g" \
                    "$svc_dest" 2>/dev/null
                fixed=true
            fi
        fi
        if $fixed; then
            log_success "Updated watcher units (best-effort under NoNewPrivileges)"
        else
            log_warn "Cannot update watcher units under NoNewPrivileges."
            log_warn "Fix: sudo ${install_dir}/config/refresh-service-units.sh"
        fi
        return 0
    fi

    # Have sudo access — do a full replacement
    if [ -f "$path_src" ] && [ -f "$svc_src" ]; then
        $SUDO cp "$path_src" "$path_dest"
        $SUDO cp "$svc_src" "$svc_dest"
        _sed_inplace "s|@@INSTALL_DIR@@|${install_dir}|g" "$path_dest"
        _sed_inplace "s|@@INSTALL_DIR@@|${install_dir}|g" "$svc_dest"
        _sed_inplace "s|@@INSTALL_PARENT@@|${install_parent}|g" "$path_dest"
        _sed_inplace "s|@@INSTALL_PARENT@@|${install_parent}|g" "$svc_dest"
        $SUDO systemctl daemon-reload 2>/dev/null || true
        $SUDO systemctl restart helixscreen-update.path 2>/dev/null || true
        log_success "Updated watcher units ($reason)"
    fi
}

# Install systemd path unit that restarts helixscreen after Moonraker extracts an update
install_update_watcher_systemd() {
    local path_src="${INSTALL_DIR}/config/helixscreen-update.path"
    local svc_src="${INSTALL_DIR}/config/helixscreen-update.service"
    local path_dest="/etc/systemd/system/helixscreen-update.path"
    local svc_dest="/etc/systemd/system/helixscreen-update.service"

    if [ ! -f "$path_src" ] || [ ! -f "$svc_src" ]; then
        log_info "Update watcher units not found, skipping"
        return 0
    fi

    local install_dir="${INSTALL_DIR:-/opt/helixscreen}"

    $SUDO cp "$path_src" "$path_dest"
    $SUDO cp "$svc_src" "$svc_dest"

    # Template the install directory path in both units
    _sed_inplace "s|@@INSTALL_DIR@@|${install_dir}|g" "$path_dest"
    _sed_inplace "s|@@INSTALL_DIR@@|${install_dir}|g" "$svc_dest"

    $SUDO systemctl daemon-reload
    $SUDO systemctl enable helixscreen-update.path 2>/dev/null || true
    $SUDO systemctl start helixscreen-update.path 2>/dev/null || true

    log_info "Installed update watcher (helixscreen-update.path)"
}

# Install SysV init script
install_service_sysv() {
    # During self-update, the init script is already installed and correct.
    # Overwriting it destroys platform customizations (ZMOD, Klipper Mod) (#314).
    if _is_self_update; then
        log_info "Skipping init script install (self-update; already installed)"
        CLEANUP_SERVICE=true
        return 0
    fi

    log_info "Installing SysV init script..."

    local init_src="${INSTALL_DIR}/config/helixscreen.init"

    if [ ! -f "$init_src" ]; then
        log_error "Init script not found: $init_src"
        log_error "The release package may be incomplete."
        exit 1
    fi

    # Use the dynamically set INIT_SCRIPT_DEST (varies by firmware)
    $SUDO cp "$init_src" "$INIT_SCRIPT_DEST"
    $SUDO chmod +x "$INIT_SCRIPT_DEST"

    # Update the DAEMON_DIR in the init script to match the install location
    # This is important for Klipper Mod which uses a different path
    _sed_inplace "s|DAEMON_DIR=.*|DAEMON_DIR=\"${INSTALL_DIR}\"|" "$INIT_SCRIPT_DEST"

    CLEANUP_SERVICE=true
    log_success "Installed SysV init script at $INIT_SCRIPT_DEST"
}

# Start service (dispatcher)
# Calls start_service_systemd or start_service_sysv based on INIT_SYSTEM.
# Snapmaker U1 uses its own init script path (patched S99screen, not S99helixscreen).
start_service() {
    local platform=${1:-}

    if [ "$platform" = "snapmaker-u1" ]; then
        start_service_snapmaker_u1
        return
    fi

    if [ "$INIT_SYSTEM" = "systemd" ]; then
        start_service_systemd
    else
        start_service_sysv
    fi
}

# Start service (Snapmaker U1)
# The U1 patches /etc/init.d/S99screen to delegate to helixscreen.init;
# there is no standalone S99helixscreen init script.
start_service_snapmaker_u1() {
    if _is_self_update; then
        log_info "Skipping service start (self-update; restart via watchdog)"
        return 0
    fi

    log_info "Starting HelixScreen (Snapmaker U1)..."

    local init_src="${INSTALL_DIR}/config/helixscreen.init"
    if [ ! -x "$init_src" ]; then
        chmod +x "$init_src" 2>/dev/null || true
    fi

    if [ ! -x "$init_src" ]; then
        log_error "Init script not found or not executable: $init_src"
        log_error "The release package may be incomplete."
        exit 1
    fi

    if ! $SUDO "$init_src" start; then
        log_error "Failed to start HelixScreen."
        log_error "Check logs in: /tmp/helixscreen.log"
        exit 1
    fi

    # Wait for service to start (may be slow on embedded hardware)
    local i
    for i in 1 2 3 4 5; do
        sleep 1
        if pidof helix-screen >/dev/null 2>&1; then
            log_success "HelixScreen is running!"
            return
        fi
    done
    log_warn "Service may still be starting..."
    log_warn "Check logs in: /tmp/helixscreen.log"
}

# Start service (systemd)
start_service_systemd() {
    log_info "Enabling and starting HelixScreen (systemd)..."

    # Under NoNewPrivileges, systemctl is blocked.  The restart is handled by
    # update_checker.cpp: it calls exit(0) after we return, which the watchdog
    # treats as "normal exit — restart silently".
    if _has_no_new_privs; then
        log_info "Skipping service start (NoNewPrivileges; restart via watchdog)"
        return 0
    fi

    if ! $SUDO systemctl enable "$SERVICE_NAME"; then
        log_error "Failed to enable ${SERVICE_NAME} service."
        exit 1
    fi

    if ! $SUDO systemctl start "$SERVICE_NAME"; then
        log_error "Failed to start ${SERVICE_NAME} service."
        log_error "Check logs with: journalctl -u ${SERVICE_NAME} -n 50"
        exit 1
    fi

    # Wait for service to start (may be slow on embedded hardware)
    local i
    for i in 1 2 3 4 5; do
        sleep 1
        if systemctl is-active --quiet "$SERVICE_NAME"; then
            log_success "HelixScreen is running!"
            return
        fi
    done
    log_warn "Service may still be starting..."
    log_warn "Check status with: systemctl status $SERVICE_NAME"
}

# Start service (SysV init)
start_service_sysv() {
    # During in-app self-update, the watchdog handles the restart via _exit(0).
    # Starting a second instance here would race with the still-running process.
    if _is_self_update; then
        log_info "Skipping service start (self-update; restart via watchdog)"
        return 0
    fi

    log_info "Starting HelixScreen (SysV init)..."

    if [ ! -x "$INIT_SCRIPT_DEST" ]; then
        log_error "Init script not executable: $INIT_SCRIPT_DEST"
        exit 1
    fi

    if ! $SUDO "$INIT_SCRIPT_DEST" start; then
        log_error "Failed to start HelixScreen."
        log_error "Check logs in: /tmp/helixscreen.log"
        exit 1
    fi

    # Wait for service to start (may be slow on embedded hardware)
    local i
    for i in 1 2 3 4 5; do
        sleep 1
        if $SUDO "$INIT_SCRIPT_DEST" status >/dev/null 2>&1; then
            log_success "HelixScreen is running!"
            return
        fi
    done
    log_warn "Service may still be starting..."
    log_warn "Check: $INIT_SCRIPT_DEST status"
}

# Deploy platform-specific hook file
# Copies the correct hook file to $INSTALL_DIR/platform/hooks.sh so the
# init script can source it at runtime.
deploy_platform_hooks() {
    local install_dir="$1"
    local platform="$2"  # "ad5m-forgex", "ad5m-kmod", "pi", "k1"
    local hooks_src="${install_dir}/config/platform/hooks-${platform}.sh"

    if [ ! -f "$hooks_src" ]; then
        log_warn "No platform hooks for: $platform"
        return 0
    fi

    # Try without sudo first: during self-update INSTALL_DIR is pi-owned so no root
    # is needed.  Fall back to sudo for fresh installs where the directory may be
    # root-owned or not yet created.  Under NoNewPrivileges sudo is blocked, so
    # the sudo fallbacks must be silent and non-fatal.
    mkdir -p "${install_dir}/platform" 2>/dev/null || $SUDO mkdir -p "${install_dir}/platform" 2>/dev/null || true
    cp "$hooks_src" "${install_dir}/platform/hooks.sh" 2>/dev/null || $SUDO cp "$hooks_src" "${install_dir}/platform/hooks.sh" 2>/dev/null || true
    chmod +x "${install_dir}/platform/hooks.sh" 2>/dev/null || $SUDO chmod +x "${install_dir}/platform/hooks.sh" 2>/dev/null || true
    log_info "Deployed platform hooks: $platform"
}

# Fix ownership of install directory for non-root service users.
# The entire INSTALL_DIR must be user-owned so that in-app self-updates
# (which run under NoNewPrivileges=true, blocking sudo) can replace
# files without root access.  ProtectSystem=strict in the service file
# limits filesystem writes to ReadWritePaths regardless of ownership.
# Uses -h to avoid following symlinks outside INSTALL_DIR.
fix_install_ownership() {
    local user="${KLIPPER_USER:-}"
    if [ -n "$user" ] && [ "$user" != "root" ] && [ -d "$INSTALL_DIR" ]; then
        log_info "Setting ownership to ${user}..."
        # Try without sudo first: during self-update under NoNewPrivileges,
        # sudo is blocked but files are already user-owned so chown succeeds
        # without it (or is a no-op).  Fall back to sudo for fresh installs
        # where root may own the directory.
        chown -Rh "${user}:${user}" "${INSTALL_DIR}" 2>/dev/null || \
            $SUDO chown -Rh "${user}:${user}" "${INSTALL_DIR}" 2>/dev/null || true
    fi
}

# Stop service for update
stop_service() {
    local platform=${1:-}

    if [ "$platform" = "snapmaker-u1" ]; then
        stop_service_snapmaker_u1
        return
    fi

    if [ "$INIT_SYSTEM" = "systemd" ]; then
        # Under NoNewPrivileges (self-update), sudo is blocked.  The service will be
        # restarted by the watchdog after exit(0) — stopping it here isn't needed.
        if _has_no_new_privs; then
            log_info "Skipping service stop (NoNewPrivileges; restart via watchdog)"
        elif systemctl is-active --quiet "$SERVICE_NAME" 2>/dev/null; then
            log_info "Stopping existing HelixScreen service (systemd)..."
            $SUDO systemctl stop "$SERVICE_NAME" || true
        fi
    else
        # During in-app self-update, the app must stay running so the user sees
        # the update progress screen.  The watchdog restarts via _exit(0) after
        # install.sh exits — killing the process here would black-screen the display.
        if _is_self_update; then
            log_info "Skipping service stop (self-update; restart via watchdog)"
        else
            # Try the configured init script location first
            if [ -n "$INIT_SCRIPT_DEST" ] && [ -x "$INIT_SCRIPT_DEST" ]; then
                log_info "Stopping existing HelixScreen service (SysV)..."
                $SUDO "$INIT_SCRIPT_DEST" stop 2>/dev/null || true
            fi
            # Also check all possible locations (for updates/uninstalls)
            for init_script in $HELIX_INIT_SCRIPTS; do
                if [ -x "$init_script" ]; then
                    log_info "Stopping HelixScreen at $init_script..."
                    $SUDO "$init_script" stop 2>/dev/null || true
                fi
            done
            # Also try to kill by name (watchdog first to prevent crash dialog flash)
            # shellcheck disable=SC2086
            kill_process_by_name $HELIX_PROCESSES
        fi
    fi
}

# Stop service (Snapmaker U1)
stop_service_snapmaker_u1() {
    if _is_self_update; then
        log_info "Skipping service stop (self-update; restart via watchdog)"
        return
    fi

    local init_src="${INSTALL_DIR}/config/helixscreen.init"
    if [ -x "$init_src" ]; then
        log_info "Stopping existing HelixScreen service (Snapmaker U1)..."
        $SUDO "$init_src" stop 2>/dev/null || true
    fi
    # Also kill by name in case init script stop didn't clean up
    # shellcheck disable=SC2086
    kill_process_by_name $HELIX_PROCESSES
}
