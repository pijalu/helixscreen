#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# yocto-docker.sh — build helixscreen inside OpenCentauri's cosmos Yocto env
#                   for the Centauri Carbon 1 (Allwinner r528 / ARM cortex-a7).
#
# Wraps `crops/poky` + the cosmos tree + the live helixscreen tree so we can
# iterate on the bitbake recipe and the Makefile without pushing commits to
# github each time.
#
# First-time setup (one-off, ~1-2GB):
#   git clone --recurse-submodules --jobs=8 \
#     https://github.com/OpenCentauri/cosmos.git "$YOCTO_COSMOS"
#   docker pull crops/poky:ubuntu-22.04
#
# Per-invocation env:
#   YOCTO_COSMOS   path to cosmos checkout        [default: ~/yocto-cosmos]
#   HELIX_SRC      path to this helixscreen tree  [default: script's repo root]
#   BITBAKE_ARGS   args passed to bitbake         [default: helixscreen]
#
# Examples:
#   ./scripts/yocto-docker.sh                           # build helixscreen
#   ./scripts/yocto-docker.sh helixscreen -c compile -f # force-recompile only
#   ./scripts/yocto-docker.sh -e helixscreen | less     # dump recipe env
#   ./scripts/yocto-docker.sh bash                      # interactive shell
#
# On first run, also write build/conf/auto.conf in $YOCTO_COSMOS — see
# docs/devel/YOCTO_BUILD.md for the contents.

set -euo pipefail

YOCTO_COSMOS="${YOCTO_COSMOS:-$HOME/yocto-cosmos}"
HELIX_SRC="${HELIX_SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if [[ ! -d "$YOCTO_COSMOS/poky" ]]; then
    echo "error: $YOCTO_COSMOS does not look like a cosmos checkout (no poky/)." >&2
    echo "  See docs/devel/YOCTO_BUILD.md for setup." >&2
    exit 1
fi

if [[ ! -f "$YOCTO_COSMOS/build/conf/auto.conf" ]]; then
    echo "error: $YOCTO_COSMOS/build/conf/auto.conf missing." >&2
    echo "  See docs/devel/YOCTO_BUILD.md for the contents." >&2
    exit 1
fi

# Default: bitbake helixscreen. Caller can override.
if [[ $# -eq 0 ]]; then
    set -- helixscreen
fi

# Special case: if first arg is "bash", drop into interactive shell instead.
if [[ "${1:-}" == "bash" ]]; then
    exec docker run --rm -it \
        -v "$YOCTO_COSMOS:/workdir" \
        -v "$HELIX_SRC:/workdir/helixscreen" \
        crops/poky:ubuntu-22.04 \
        --workdir=/workdir
fi

# Non-interactive: run bitbake with the given args.
exec docker run --rm \
    -v "$YOCTO_COSMOS:/workdir" \
    -v "$HELIX_SRC:/workdir/helixscreen" \
    crops/poky:ubuntu-22.04 \
    --workdir=/workdir \
    -- bash -c "source poky/oe-init-build-env build >/dev/null && bitbake $*"
