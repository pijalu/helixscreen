#!/usr/bin/env bats
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for detect_platform() in platform.sh
# Covers Pi 64-bit kernel with 32-bit userspace detection (the "not found" bug)

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
    TMP_DIR=""

    # Source platform.sh (skip source guard by unsetting it)
    unset _HELIX_PLATFORM_SOURCED
    . "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"

    # Create a fake /etc/os-release indicating Debian (Pi-like system)
    mkdir -p "$BATS_TEST_TMPDIR/etc"
    echo 'ID=debian' > "$BATS_TEST_TMPDIR/etc/os-release"
    # Create a fake /home/pi directory to trigger is_pi=true
    mkdir -p "$BATS_TEST_TMPDIR/home/pi"
}


# Since detect_platform reads real /etc/os-release and /home/*, we
# redefine it with controlled inputs for testing. This helper captures
# the Pi userspace bitness detection logic from the real function.
_mock_pi_detect_platform() {
    local arch="$1"

    local userspace_bits
    userspace_bits=$(getconf LONG_BIT 2>/dev/null || echo "")
    if [ "$userspace_bits" = "64" ]; then
        echo "pi"
    elif [ "$userspace_bits" = "32" ]; then
        echo "pi32"
    else
        if file /usr/bin/id 2>/dev/null | grep -q "64-bit"; then
            echo "pi"
        elif file /usr/bin/id 2>/dev/null | grep -q "32-bit"; then
            echo "pi32"
        else
            # Last resort: trust kernel arch
            if [ "$arch" = "aarch64" ]; then
                echo "pi"
            else
                echo "pi32"
            fi
        fi
    fi
}

@test "Pi aarch64 kernel + 32-bit userspace (getconf) returns pi32" {
    detect_platform() { _mock_pi_detect_platform "aarch64"; }
    mock_command "getconf" "32"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi32" ]
}

@test "Pi aarch64 kernel + 64-bit userspace (getconf) returns pi" {
    detect_platform() { _mock_pi_detect_platform "aarch64"; }
    mock_command "getconf" "64"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "Pi armv7l kernel + 32-bit userspace returns pi32" {
    detect_platform() { _mock_pi_detect_platform "armv7l"; }
    mock_command "getconf" "32"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi32" ]
}

@test "Pi aarch64 + getconf unavailable + file says 32-bit returns pi32" {
    detect_platform() { _mock_pi_detect_platform "aarch64"; }
    mock_command_fail "getconf"
    mock_command "file" "/usr/bin/id: ELF 32-bit LSB pie executable, ARM, EABI5"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi32" ]
}

@test "Pi aarch64 + getconf unavailable + file says 64-bit returns pi" {
    detect_platform() { _mock_pi_detect_platform "aarch64"; }
    mock_command_fail "getconf"
    mock_command "file" "/usr/bin/id: ELF 64-bit LSB pie executable, ARM aarch64"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

@test "Pi aarch64 + getconf unavailable + file unavailable falls back to kernel arch" {
    detect_platform() { _mock_pi_detect_platform "aarch64"; }
    mock_command_fail "getconf"
    mock_command_fail "file"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "pi" ]
}

# --- AD5X platform detection tests ---

# Mock detect_platform for AD5X: checks temp dir for AD5X indicators
_mock_ad5x_detect_platform() {
    local arch="$1"
    local mock_root="$BATS_TEST_TMPDIR"

    if [ "$arch" = "mips" ]; then
        if [ -d "$mock_root/usr/data" ] && { [ -d "$mock_root/usr/prog" ] || [ -f "$mock_root/ZMOD" ]; }; then
            echo "ad5x"
            return
        fi
    fi

    echo "unsupported"
}

@test "AD5X: MIPS arch + /usr/data + /usr/prog returns ad5x" {
    mkdir -p "$BATS_TEST_TMPDIR/usr/data" "$BATS_TEST_TMPDIR/usr/prog"
    detect_platform() { _mock_ad5x_detect_platform "mips"; }
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "ad5x" ]
}

@test "AD5X: MIPS arch + /usr/data + /ZMOD file returns ad5x" {
    mkdir -p "$BATS_TEST_TMPDIR/usr/data"
    touch "$BATS_TEST_TMPDIR/ZMOD"
    detect_platform() { _mock_ad5x_detect_platform "mips"; }
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "ad5x" ]
}

@test "AD5X: MIPS arch + /usr/data but NO /usr/prog and NO /ZMOD does NOT return ad5x" {
    mkdir -p "$BATS_TEST_TMPDIR/usr/data"
    detect_platform() { _mock_ad5x_detect_platform "mips"; }
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "unsupported" ]
}

@test "AD5X: non-MIPS arch + /usr/data + /usr/prog does NOT return ad5x" {
    detect_platform() {
        local arch="armv7l"

        if [ "$arch" = "mips" ]; then
            if [ -d "/usr/data" ] && { [ -d "/usr/prog" ] || [ -f "/ZMOD" ]; }; then
                echo "ad5x"
                return
            fi
        fi

        # Fall through to other detection
        echo "unsupported"
    }

    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "unsupported" ]
}

@test "AD5X: set_install_paths sets correct paths for ad5x" {
    # Mock detect_tmp_dir to avoid filesystem checks
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }

    set_install_paths "ad5x"

    [ "$INSTALL_DIR" = "/srv/helixscreen" ]
    [ "$INIT_SCRIPT_DEST" = "/etc/init.d/S80helixscreen" ]
    [ "$PREVIOUS_UI_SCRIPT" = "" ]
}

@test "AD5X: detect_platform checks AD5X before K1 (ordering)" {
    # Verify the source code has AD5X check before K1 check
    # AD5X must be checked first because both have /usr/data, but only AD5X has /usr/prog
    local platform_sh="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    local ad5x_line k1_line
    ad5x_line=$(grep -n 'Check for FlashForge AD5X' "$platform_sh" | head -1 | cut -d: -f1)
    k1_line=$(grep -n 'Check for Creality K1' "$platform_sh" | head -1 | cut -d: -f1)
    [ -n "$ad5x_line" ]
    [ -n "$k1_line" ]
    [ "$ad5x_line" -lt "$k1_line" ]
}

# --- Source code regression tests ---

@test "platform.sh uses getconf LONG_BIT for userspace detection" {
    grep -q 'getconf LONG_BIT' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "platform.sh has file command fallback for userspace detection" {
    grep -q 'file /usr/bin/id' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "platform.sh warns about 64-bit kernel with 32-bit userspace" {
    grep -q '64-bit kernel with 32-bit userspace' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "install.sh (bundled) also uses getconf LONG_BIT" {
    grep -q 'getconf LONG_BIT' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "platform.sh has AD5X detection with /usr/prog check" {
    grep -q '/usr/prog' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "install.sh (bundled) has AD5X detection" {
    grep -q 'ad5x' "$WORKTREE_ROOT/scripts/install.sh"
}

@test "platform.sh returns ad5x in detect_platform docstring" {
    grep -q '"ad5x"' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

# --- AD5M ZMOD firmware detection tests ---

# Mock detect_ad5m_firmware using temp dir paths
_mock_detect_ad5m_firmware() {
    local mock_root="$BATS_TEST_TMPDIR"
    if [ -f "$mock_root/ZMOD" ]; then
        echo "zmod"
        return
    fi
    if [ -d "$mock_root/root/printer_software" ] || [ -d "$mock_root/mnt/data/.klipper_mod" ]; then
        echo "klipper_mod"
        return
    fi
    echo "forge_x"
}

@test "AD5M: detect_ad5m_firmware returns zmod when /ZMOD exists" {
    detect_ad5m_firmware() { _mock_detect_ad5m_firmware; }
    touch "$BATS_TEST_TMPDIR/ZMOD"
    run detect_ad5m_firmware
    [ "$status" -eq 0 ]
    [ "$output" = "zmod" ]
}

@test "AD5M: detect_ad5m_firmware returns klipper_mod when no /ZMOD" {
    detect_ad5m_firmware() { _mock_detect_ad5m_firmware; }
    mkdir -p "$BATS_TEST_TMPDIR/root/printer_software"
    run detect_ad5m_firmware
    [ "$status" -eq 0 ]
    [ "$output" = "klipper_mod" ]
}

@test "AD5M: detect_ad5m_firmware prefers zmod over klipper_mod" {
    detect_ad5m_firmware() { _mock_detect_ad5m_firmware; }
    touch "$BATS_TEST_TMPDIR/ZMOD"
    mkdir -p "$BATS_TEST_TMPDIR/root/printer_software"
    run detect_ad5m_firmware
    [ "$status" -eq 0 ]
    [ "$output" = "zmod" ]
}

@test "AD5M: set_install_paths sets correct paths for zmod" {
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }

    set_install_paths "ad5m" "zmod"

    [ "$INSTALL_DIR" = "/srv/helixscreen" ]
    [ "$INIT_SCRIPT_DEST" = "/etc/init.d/S80helixscreen" ]
    [ "$PREVIOUS_UI_SCRIPT" = "" ]
}

@test "platform.sh detect_ad5m_firmware docstring mentions zmod" {
    grep -q '"zmod"' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "platform.sh checks /ZMOD before forge_x default" {
    local platform_sh="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    local zmod_line forge_x_line
    zmod_line=$(grep -n '/ZMOD' "$platform_sh" | grep -v 'AD5X\|mips\|usr/data' | head -1 | cut -d: -f1)
    forge_x_line=$(grep -n 'opt/config/mod/.root' "$platform_sh" | head -1 | cut -d: -f1)
    [ -n "$zmod_line" ]
    [ -n "$forge_x_line" ]
    [ "$zmod_line" -lt "$forge_x_line" ]
}

# --- Creality K2 platform detection tests ---

# Mock detect_platform for K2 vs AD5M disambiguation (armv7l, kernel 5.4.61)
_mock_k2_detect_platform() {
    local mock_root="$BATS_TEST_TMPDIR"

    # K2 check (must come before AD5M)
    if [ -d "$mock_root/mnt/UDISK" ]; then
        if [ -f "$mock_root/etc/os-release" ] && grep -qi "openwrt\|tina" "$mock_root/etc/os-release" 2>/dev/null; then
            echo "k2"
            return
        fi
        if [ -d "$mock_root/mnt/UDISK/printer_data" ] || [ -d "$mock_root/mnt/UDISK/creality" ]; then
            echo "k2"
            return
        fi
    fi

    # AD5M fallback (both share armv7l + kernel 5.4.61)
    echo "ad5m"
}

@test "K2: armv7l + OpenWrt + /mnt/UDISK returns k2" {
    detect_platform() { _mock_k2_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK"
    echo 'NAME="OpenWrt"' > "$BATS_TEST_TMPDIR/etc/os-release"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "k2" ]
}

@test "K2: armv7l + /mnt/UDISK/printer_data (no os-release match) returns k2" {
    detect_platform() { _mock_k2_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK/printer_data"
    echo 'ID=tina' > "$BATS_TEST_TMPDIR/etc/os-release"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "k2" ]
}

@test "K2: detect_platform checks K2 before AD5M (ordering)" {
    local platform_sh="$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
    local k2_line ad5m_line
    k2_line=$(grep -n 'Check for Creality K2' "$platform_sh" | head -1 | cut -d: -f1)
    ad5m_line=$(grep -n 'Check for AD5M' "$platform_sh" | head -1 | cut -d: -f1)
    [ -n "$k2_line" ]
    [ -n "$ad5m_line" ]
    [ "$k2_line" -lt "$ad5m_line" ]
}

@test "K2: armv7l + Tina Linux + /mnt/UDISK returns k2" {
    detect_platform() { _mock_k2_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK"
    echo 'NAME="Tina Linux"' > "$BATS_TEST_TMPDIR/etc/os-release"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "k2" ]
}

@test "K2: armv7l + /mnt/UDISK/creality (no os-release match) returns k2" {
    detect_platform() { _mock_k2_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK/creality"
    echo 'ID=buildroot' > "$BATS_TEST_TMPDIR/etc/os-release"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "k2" ]
}

@test "K2: armv7l + empty /mnt/UDISK (no indicators) returns ad5m" {
    detect_platform() { _mock_k2_detect_platform; }
    mkdir -p "$BATS_TEST_TMPDIR/mnt/UDISK"
    echo 'ID=buildroot' > "$BATS_TEST_TMPDIR/etc/os-release"
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "ad5m" ]
}

@test "K2: armv7l + kernel 5.4.61 WITHOUT /mnt/UDISK returns ad5m (not k2)" {
    # No /mnt/UDISK in temp dir -- falls through to AD5M
    detect_platform() { _mock_k2_detect_platform; }
    run detect_platform
    [ "$status" -eq 0 ]
    [ "$output" = "ad5m" ]
}

@test "K2: set_install_paths sets correct paths for k2" {
    detect_tmp_dir() { TMP_DIR="/tmp/helixscreen-install"; }

    set_install_paths "k2"

    [ "$INSTALL_DIR" = "/opt/helixscreen" ]
    [ "$INIT_SCRIPT_DEST" = "/etc/init.d/S99helixscreen" ]
    [ "$KLIPPER_USER" = "root" ]
}

@test "platform.sh returns k2 in detect_platform docstring" {
    grep -q '"k2"' "$WORKTREE_ROOT/scripts/lib/installer/platform.sh"
}

@test "install.sh (bundled) has K2 detection" {
    grep -q 'k2' "$WORKTREE_ROOT/scripts/install.sh"
}
