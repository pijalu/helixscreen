#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for setup_config_symlink() and remove_config_symlink() in platform.sh
#
# New layout: real directory in printer_data/config/helixscreen/ with per-file
# symlinks from install_dir/config/ pointing into printer_data.
# Old layout (upgrade path): directory symlink printer_data/config/helixscreen → install_dir/config

WORKTREE_ROOT="$(cd "$BATS_TEST_DIRNAME/../.." && pwd)"

setup() {
    load helpers

    # Reset globals before each test
    KLIPPER_USER=""
    KLIPPER_HOME=""
    INIT_SCRIPT_DEST=""
    PREVIOUS_UI_SCRIPT=""
    AD5M_FIRMWARE=""
    K1_FIRMWARE=""
    INSTALL_DIR="/opt/helixscreen"
    TMP_DIR="/tmp/helixscreen-install"
    _USER_INSTALL_DIR=""
    SUDO=""

    # Source platform.sh (skip source guard by unsetting it)
    unset _HELIX_PLATFORM_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

# --- Happy path: fresh install ---

@test "creates real directory and per-file symlinks on fresh install" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    # Create a user config file in install dir (as if just installed)
    echo '{"dark_mode":true}' > "$INSTALL_DIR/config/helixconfig.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # printer_data/config/helixscreen/ should be a real directory
    [ -d "$KLIPPER_HOME/printer_data/config/helixscreen" ]
    [ ! -L "$KLIPPER_HOME/printer_data/config/helixscreen" ]

    # File should have been moved to printer_data
    [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json" ]
    [ "$(cat "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json")" = '{"dark_mode":true}' ]

    # Install dir should have a symlink pointing to printer_data
    [ -L "$INSTALL_DIR/config/helixconfig.json" ]
    [ "$(readlink "$INSTALL_DIR/config/helixconfig.json")" = "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json" ]
}

@test "creates symlinks for all user config files" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    # Create all user-editable files
    echo '{}' > "$INSTALL_DIR/config/helixconfig.json"
    echo 'ENV=test' > "$INSTALL_DIR/config/helixscreen.env"
    echo 'svc:foo' > "$INSTALL_DIR/config/.disabled_services"
    echo '[]' > "$INSTALL_DIR/config/tool_spools.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # All files should be in printer_data
    for f in helixconfig.json helixscreen.env .disabled_services tool_spools.json; do
        [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/$f" ]
        [ -L "$INSTALL_DIR/config/$f" ]
    done
}

@test "does not move files that do not exist in install dir" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    # Only helixconfig.json exists
    echo '{}' > "$INSTALL_DIR/config/helixconfig.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # helixconfig.json moved and symlinked
    [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json" ]
    [ -L "$INSTALL_DIR/config/helixconfig.json" ]

    # Other files: symlink created but no file in printer_data (broken symlink is ok)
    [ -L "$INSTALL_DIR/config/helixscreen.env" ]
    [ ! -f "$KLIPPER_HOME/printer_data/config/helixscreen/helixscreen.env" ]
}

# --- Graceful skip conditions ---

@test "skips when KLIPPER_HOME is empty" {
    KLIPPER_HOME=""
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    run setup_config_symlink
    [ "$status" -eq 0 ]
}

@test "skips when INSTALL_DIR is empty" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR=""
    mkdir -p "$KLIPPER_HOME/printer_data/config"

    run setup_config_symlink
    [ "$status" -eq 0 ]
}

@test "skips when printer_data/config does not exist" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME"
    mkdir -p "$INSTALL_DIR/config"

    run setup_config_symlink
    [ "$status" -eq 0 ]
    [ ! -e "$KLIPPER_HOME/printer_data/config/helixscreen" ]
}

@test "skips when install config directory does not exist" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR"

    run setup_config_symlink
    [ "$status" -eq 0 ]
    [ ! -e "$KLIPPER_HOME/printer_data/config/helixscreen" ]
}

# --- Upgrade from old layout (directory symlink) ---

@test "migrates from old directory symlink layout" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"
    echo '{"upgraded":true}' > "$INSTALL_DIR/config/helixconfig.json"
    echo 'LOG_LEVEL=debug' > "$INSTALL_DIR/config/helixscreen.env"

    # Create old-style directory symlink
    ln -s "$INSTALL_DIR/config" "$KLIPPER_HOME/printer_data/config/helixscreen"
    [ -L "$KLIPPER_HOME/printer_data/config/helixscreen" ]

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # Old symlink should be gone, replaced by real directory
    [ ! -L "$KLIPPER_HOME/printer_data/config/helixscreen" ]
    [ -d "$KLIPPER_HOME/printer_data/config/helixscreen" ]

    # Files should be migrated to printer_data
    [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json" ]
    [ "$(cat "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json")" = '{"upgraded":true}' ]

    # Install dir should have symlinks pointing back
    [ -L "$INSTALL_DIR/config/helixconfig.json" ]
}

# --- Idempotency ---

@test "no-op when symlinks already correct" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    # Set up the correct layout manually
    echo '{}' > "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json"
    ln -s "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json" "$INSTALL_DIR/config/helixconfig.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # Should still be correct
    [ -L "$INSTALL_DIR/config/helixconfig.json" ]
    [ "$(readlink "$INSTALL_DIR/config/helixconfig.json")" = "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json" ]
}

@test "does not overwrite existing file in printer_data during migration" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    # File already in printer_data (user edited it via Fluidd)
    echo '{"from_fluidd":true}' > "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json"
    # Old file still in install dir
    echo '{"from_install":true}' > "$INSTALL_DIR/config/helixconfig.json"

    run setup_config_symlink
    [ "$status" -eq 0 ]

    # printer_data file should NOT be overwritten
    [ "$(cat "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json")" = '{"from_fluidd":true}' ]

    # Install dir file should be replaced with symlink
    [ -L "$INSTALL_DIR/config/helixconfig.json" ]
}

# --- remove_config_symlink ---

@test "remove_config_symlink removes per-file symlinks from install dir" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    # Set up per-file symlinks
    echo '{}' > "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json"
    ln -s "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json" "$INSTALL_DIR/config/helixconfig.json"

    run remove_config_symlink
    [ "$status" -eq 0 ]

    # Symlink removed
    [ ! -L "$INSTALL_DIR/config/helixconfig.json" ]

    # User files preserved in printer_data
    [ -f "$KLIPPER_HOME/printer_data/config/helixscreen/helixconfig.json" ]
}

@test "remove_config_symlink handles old directory symlink" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$KLIPPER_HOME/printer_data/config"
    mkdir -p "$INSTALL_DIR/config"

    # Old-style directory symlink
    ln -s "$INSTALL_DIR/config" "$KLIPPER_HOME/printer_data/config/helixscreen"

    run remove_config_symlink
    [ "$status" -eq 0 ]

    # Old symlink removed
    [ ! -L "$KLIPPER_HOME/printer_data/config/helixscreen" ]
}

@test "remove_config_symlink is safe when nothing exists" {
    KLIPPER_HOME="$BATS_TEST_TMPDIR/home/pi"
    INSTALL_DIR="$BATS_TEST_TMPDIR/helixscreen"
    mkdir -p "$INSTALL_DIR/config"

    run remove_config_symlink
    [ "$status" -eq 0 ]
}

# --- Bundled installer parity ---

@test "bundled install.sh has setup_config_symlink function" {
    grep -q 'setup_config_symlink()' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "bundled install.sh calls setup_config_symlink in main flow" {
    grep -q 'setup_config_symlink' "$WORKTREE_ROOT/scripts/install.sh"
}
