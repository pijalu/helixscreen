# Yocto Build (OpenCentauri COSMOS / Centauri Carbon 1)

HelixScreen ships as a Yocto recipe in
[OpenCentauri/cosmos](https://github.com/OpenCentauri/cosmos) under
`meta-opencentauri/recipes-apps/helixscreen/helixscreen_0.1.bb`. This guide
sets up a local dev loop so we can iterate on both the recipe and our
Makefile without pushing commits to GitHub each time.

Cross-target: Allwinner r528 SoC, ARM cortex-a7 hard-float NEON VFPv4, glibc,
fbdev (no GLES). The sunxi BSP lives in cosmos's `meta-sunxi` + `meta-opencentauri`.

## One-time setup

Requires Docker, git, and ~40GB free disk.

```bash
# 1. Clone cosmos (~1.5GB including submodules)
git clone --recurse-submodules --jobs=8 \
    https://github.com/OpenCentauri/cosmos.git ~/yocto-cosmos

# 2. Pull the official poky dev container
docker pull crops/poky:ubuntu-22.04

# 3. Drop a dev-only auto.conf into cosmos's build/conf/ (see below)
cat > ~/yocto-cosmos/build/conf/auto.conf <<'EOF'
# Public Yocto sstate mirror — pulls prebuilt gcc/glibc/pkgconfig-native/etc.
# for the scarthgap release, saving hours on the first build.
SSTATE_MIRRORS ?= "file://.* http://sstate.yoctoproject.org/all/PATH;downloadfilename=PATH"

# Build helixscreen from our live worktree mounted at /workdir/helixscreen
# (no push-to-github per iteration).
INHERIT += "externalsrc"
EXTERNALSRC:pn-helixscreen = "/workdir/helixscreen"
# B = S so the Makefile actually runs. Our Makefile builds into ./build/
# which is gitignored.
EXTERNALSRC_BUILD:pn-helixscreen = "/workdir/helixscreen"

BB_NUMBER_THREADS = "8"
PARALLEL_MAKE = "-j 8"
EOF
```

The `auto.conf` stays *outside* this repo — it's a user-local dev override
for the cosmos tree.

## Iteration loop

From anywhere inside the helixscreen repo (including a worktree):

```bash
./scripts/yocto-docker.sh                           # full build
./scripts/yocto-docker.sh helixscreen -c compile -f # force-recompile only (fastest)
./scripts/yocto-docker.sh -e helixscreen | less     # dump recipe env
./scripts/yocto-docker.sh bash                      # interactive poky shell
```

The script mounts whichever tree it was invoked from (via `$HELIX_SRC`, auto-
detected) at `/workdir/helixscreen`. Edit Makefile / code locally → rerun →
see errors in seconds-to-minutes.

First build will also pull sstate from yoctoproject.org's public mirror for
gcc-cross, glibc, pkgconfig-native, etc. — expect 10-30 minutes. Subsequent
incremental builds take seconds.

## Recipe location

```
~/yocto-cosmos/meta-opencentauri/recipes-apps/helixscreen/helixscreen_0.1.bb
```

When iterating on recipe changes, edit that file directly. When it's right,
we open a PR against `OpenCentauri/cosmos` with the updated recipe.

## Known issues (in-flight)

- Our Makefile's default `PLATFORM_TARGET=native` tries to build vendored
  SDL2/libhv/etc. from submodules. We need a `PLATFORM_TARGET=yocto` mode
  that honors bitbake's CC/CXX/CFLAGS env and `DEPENDS` on system packages.
- `-Wpsabi` warnings may trip `-Werror`; our Makefile should disable them on
  ARM psABI-transition targets.
