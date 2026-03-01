#!/bin/bash

# Copyright (C) 2025-2026 356C LLC
# SPDX-License-Identifier: GPL-3.0-or-later

set -e

# Show help
show_help() {
    cat << 'EOF'
Usage: screenshot.sh [BINARY] [NAME] [PANEL] [FLAGS...]

Capture a screenshot of the HelixScreen UI.

Arguments:
  BINARY    Binary name in build/bin/ (default: helix-screen)
  NAME      Output filename suffix (default: timestamp)
            Screenshot saved to: /tmp/ui-screenshot-<NAME>.png
  PANEL     Panel to display (optional)
            Valid panels: home, controls, motion, nozzle-temp, bed-temp,
            extrusion, filament, fan, settings, advanced, print-select
  FLAGS     Additional flags passed to the binary (e.g., --test, -s large)

Environment Variables:
  HELIX_SCREENSHOT_DISPLAY   Display number to open window on (default: 1)
  HELIX_SCREENSHOT_TIMEOUT   Seconds before auto-quit (default: 3, use 15 for real printer)
  HELIX_SCREENSHOT_OPEN      If set, opens the screenshot in Preview (macOS)

Examples:
  ./scripts/screenshot.sh                           # Default binary, timestamp name
  ./scripts/screenshot.sh helix-screen home-panel home
  ./scripts/screenshot.sh helix-screen controls-test controls --test
  ./scripts/screenshot.sh helix-screen motion-large motion -s large

Output:
  Screenshots are saved to /tmp/ui-screenshot-<NAME>.png
  BMP files are automatically converted to PNG and cleaned up.

Dependencies:
  - ImageMagick (apt install imagemagick / brew install imagemagick)
EOF
    exit 0
}

# Check for help flag
case "${1:-}" in
    -h|--help|help)
        show_help
        ;;
esac

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print colored message
info() { echo -e "${BLUE}ℹ${NC} $1"; }
success() { echo -e "${GREEN}✓${NC} $1"; }
warn() { echo -e "${YELLOW}⚠${NC} $1"; }
error() { echo -e "${RED}✗${NC} $1"; }

# Detect script directory and change to project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Get binary name (first arg) or default
BINARY="${1:-helix-screen}"
BINARY_PATH="./build/bin/${BINARY}"

# Get unique name or use timestamp
NAME="${2:-$(date +%s)}"
BMP_FILE="/tmp/ui-screenshot-${NAME}.bmp"
PNG_FILE="/tmp/ui-screenshot-${NAME}.png"

# Get panel name (third arg) or treat as flags if starts with -
# This allows: ./screenshot.sh binary name panel [flags]
#          OR: ./screenshot.sh binary name [flags] (no panel)
PANEL=""
EXTRA_ARGS=""
if [ $# -ge 3 ]; then
    if [[ "${3}" == -* ]]; then
        # Third arg is a flag, treat it and everything after as extra args
        shift 2
        EXTRA_ARGS="$@"
    else
        # Third arg is a panel name
        PANEL="${3}"
        shift 3 2>/dev/null || true
        EXTRA_ARGS="$@"
    fi
else
    shift 2 2>/dev/null || true
    EXTRA_ARGS="$@"
fi

# Note: Panel validation is handled by the binary itself

# Detect which display to use
if [ -z "$HELIX_SCREENSHOT_DISPLAY" ]; then
    # Default to display 1 (assumes terminal is on display 0)
    HELIX_SCREENSHOT_DISPLAY=1
    info "Opening UI on display $HELIX_SCREENSHOT_DISPLAY (override with HELIX_SCREENSHOT_DISPLAY env var)"
else
    info "Using display $HELIX_SCREENSHOT_DISPLAY from HELIX_SCREENSHOT_DISPLAY env var"
fi

# Timeout configuration (override with HELIX_SCREENSHOT_TIMEOUT env var)
SCREENSHOT_TIMEOUT="${HELIX_SCREENSHOT_TIMEOUT:-3}"
SCREENSHOT_DELAY=$((SCREENSHOT_TIMEOUT - 1))
if [ "$SCREENSHOT_DELAY" -lt 1 ]; then
    SCREENSHOT_DELAY=1
fi

# Add display, screenshot, timeout, and skip-splash arguments to extra args
# Screenshot 1 second before timeout, then auto-quit
# Skip splash screen for faster automation
EXTRA_ARGS="--display $HELIX_SCREENSHOT_DISPLAY --screenshot $SCREENSHOT_DELAY --timeout $SCREENSHOT_TIMEOUT --skip-splash $EXTRA_ARGS"

# Check dependencies — ImageMagick v7 uses 'magick', v6 uses 'convert'
info "Checking dependencies..."
MAGICK_CMD=""
if command -v magick &> /dev/null; then
    MAGICK_CMD="magick"
elif command -v convert &> /dev/null; then
    MAGICK_CMD="convert"
else
    error "ImageMagick not found (install with: sudo apt install imagemagick)"
    exit 1
fi

# Verify binary exists
if [ ! -f "$BINARY_PATH" ]; then
    error "Binary not found: $BINARY_PATH"
    info "Build the binary first with: make"
    ls -la build/bin/ 2>/dev/null || info "build/bin/ directory doesn't exist yet"
    exit 1
fi

if [ ! -x "$BINARY_PATH" ]; then
    error "Binary not executable: $BINARY_PATH"
    chmod +x "$BINARY_PATH"
    success "Made binary executable"
fi

# Clean old screenshots
rm -f /tmp/ui-screenshot-*.bmp 2>/dev/null || true

# Prepare run command and args
if [ -n "$PANEL" ]; then
    info "Running ${BINARY} with panel: ${PANEL} (auto-quit after ${SCREENSHOT_TIMEOUT}s)..."
    PANEL_ARG="-p ${PANEL}"
else
    info "Running ${BINARY} (auto-quit after ${SCREENSHOT_TIMEOUT}s)..."
    PANEL_ARG=""
fi

# Run and capture output (binary will auto-quit after timeout)
# IMPORTANT: Panel arg must come BEFORE other args for correct parsing
RUN_OUTPUT=$(${BINARY_PATH} ${PANEL_ARG} ${EXTRA_ARGS} 2>&1 || true)

# Check for errors in output
if echo "$RUN_OUTPUT" | grep -qi "error"; then
    warn "Errors detected during run:"
    echo "$RUN_OUTPUT" | grep -i "error"
fi

# Show relevant output
echo "$RUN_OUTPUT" | grep -E "(LVGL initialized|Screenshot saved|Window centered|display)" || true

# Find the most recent screenshot
info "Looking for screenshot..."
LATEST_BMP=$(ls -t /tmp/ui-screenshot-*.bmp 2>/dev/null | head -1)

if [ -z "$LATEST_BMP" ]; then
    error "Screenshot not captured"
    warn "Binary should take screenshot after 2 seconds and quit after 3 seconds"
    echo ""
    echo "Last 10 lines of output:"
    echo "$RUN_OUTPUT" | tail -10
    exit 1
fi

# Rename to requested name
if [ "$LATEST_BMP" != "$BMP_FILE" ]; then
    mv "$LATEST_BMP" "$BMP_FILE"
fi

BMP_SIZE=$(ls -lh "$BMP_FILE" | awk '{print $5}')
success "Screenshot captured: $BMP_FILE ($BMP_SIZE)"

# Convert to PNG
info "Converting BMP to PNG..."
if ! $MAGICK_CMD "$BMP_FILE" "$PNG_FILE" 2>/dev/null; then
    error "PNG conversion failed"
    warn "BMP file available at: $BMP_FILE"
    exit 1
fi

# Cleanup BMP
rm -f "$BMP_FILE"

# Show result
PNG_SIZE=$(ls -lh "$PNG_FILE" | awk '{print $5}')
echo ""
success "Screenshot ready!"
echo "  File: $PNG_FILE"
echo "  Size: $PNG_SIZE"
echo "  Panel: ${PANEL:-default}"
echo "  Display: $HELIX_SCREENSHOT_DISPLAY"
echo ""

# Optional: open screenshot in viewer
if [ -n "$HELIX_SCREENSHOT_OPEN" ]; then
    info "Opening screenshot..."
    if command -v open &> /dev/null; then
        open "$PNG_FILE"
    elif command -v xdg-open &> /dev/null; then
        xdg-open "$PNG_FILE"
    fi
fi
