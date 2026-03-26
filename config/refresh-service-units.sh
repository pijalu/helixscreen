#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Refresh systemd unit files from newly extracted install dir templates.
#
# Called by helixscreen-update.service after Moonraker extracts a new release.
# This script lives in the install dir (not /etc/systemd/system/) so Moonraker's
# extraction always updates it — solving the chicken-and-egg where the installer's
# global sed pass would corrupt @@placeholder@@ patterns embedded directly in the
# systemd unit file.
#
# Reads User/Group from the CURRENTLY installed helixscreen.service before
# overwriting, then templates all @@placeholders@@ in the new copies.
# Also refreshes the watcher units (update.service + update.path) so future
# Moonraker updates pick up any fixes to the watcher mechanism itself.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IDIR="$(dirname "$SCRIPT_DIR")"
PDIR="$(dirname "$IDIR")"

DEST="/etc/systemd/system/helixscreen.service"

# Nothing to do if main service isn't installed
[ -f "$DEST" ] || exit 0

# Read current identity from the installed service file BEFORE overwriting
USER_VAL="$(grep "^User=" "$DEST" | cut -d= -f2)"
GROUP_VAL="$(grep "^Group=" "$DEST" | cut -d= -f2)"

template_unit() {
    local src="$1" dest="$2"
    cp "$src" "$dest" || return 1
    sed -i \
        -e "s|@@HELIX_USER@@|${USER_VAL:-root}|g" \
        -e "s|@@HELIX_GROUP@@|${GROUP_VAL:-root}|g" \
        -e "s|@@INSTALL_DIR@@|${IDIR}|g" \
        -e "s|@@INSTALL_PARENT@@|${PDIR}|g" \
        "$dest"
}

# Refresh main service
SRC="${IDIR}/config/helixscreen.service"
[ -f "$SRC" ] && template_unit "$SRC" "$DEST"

# Refresh watcher units (this service + path unit)
for F in helixscreen-update.service helixscreen-update.path; do
    FSRC="${IDIR}/config/${F}"
    FDEST="/etc/systemd/system/${F}"
    [ -f "$FSRC" ] && template_unit "$FSRC" "$FDEST"
done

systemctl daemon-reload
