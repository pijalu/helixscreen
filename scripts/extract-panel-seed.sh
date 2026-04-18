#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Extract a panel_widgets layout from a live device's settings.json and save
# it as a preset seed under assets/config/panel_widgets/<preset>/<panel>.json.
# Fresh installs on that preset will boot with the extracted layout.
#
# Usage: scripts/extract-panel-seed.sh <preset> <panel> <ssh_target> <settings_path>
#
# Example:
#   scripts/extract-panel-seed.sh cc1 home root@192.168.1.52 \
#       /user-resource/helixscreen/config/settings.json
#
# Requires: ssh (keys or sshpass), python3, scp -O for legacy SSH servers.

set -euo pipefail

if [[ $# -ne 4 ]]; then
    echo "Usage: $0 <preset> <panel> <ssh_target> <settings_path>" >&2
    exit 1
fi

PRESET="$1"
PANEL="$2"
SSH_TARGET="$3"
SETTINGS_PATH="$4"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${REPO_ROOT}/assets/config/panel_widgets/${PRESET}"
OUT_FILE="${OUT_DIR}/${PANEL}.json"
TMP_FILE="$(mktemp -t helix-settings-XXXXXX.json)"
trap 'rm -f "${TMP_FILE}"' EXIT

echo "Pulling ${SSH_TARGET}:${SETTINGS_PATH}..."
scp -O -o StrictHostKeyChecking=no "${SSH_TARGET}:${SETTINGS_PATH}" "${TMP_FILE}"

mkdir -p "${OUT_DIR}"

python3 - "${TMP_FILE}" "${PANEL}" "${OUT_FILE}" <<'PY'
import json, sys
src, panel, dst = sys.argv[1], sys.argv[2], sys.argv[3]
with open(src) as f:
    data = json.load(f)

printers = data.get("printers", {})
# printers may contain bool siblings (e.g. show_printer_switcher); skip those.
candidates = [pid for pid, p in printers.items() if isinstance(p, dict)]
if not candidates:
    sys.exit("no printer entries in settings.json")

# Prefer 'default', else first dict entry.
pid = "default" if "default" in candidates else candidates[0]
widget_tree = printers[pid].get("panel_widgets", {}).get(panel)
if widget_tree is None:
    sys.exit(f"printers.{pid}.panel_widgets.{panel} not found in settings")

with open(dst, "w") as f:
    json.dump(widget_tree, f, indent=2, sort_keys=True)
    f.write("\n")
print(f"Wrote {dst}")
PY
