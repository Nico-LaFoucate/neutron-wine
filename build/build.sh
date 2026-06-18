#!/usr/bin/env bash
# build.sh — build the Neutron wine (patch set + pinned config on upstream wine-tkg).
#
# Produces a complete Wine tree; point Neutron at it with NEUTRON_WINE=<…>/wine.
# This is the Proton-GE-style recipe: fetch wine-tkg, drop in our patches + pinned
# config, build. Phase 2/3 will turn the output into a versioned, bundled runtime.
#
# Prereqs: the usual Wine build toolchain + wine-tkg-git's deps (see that project).
# Env:
#   NEUTRON_WINE_WORK=<dir>   where to clone/build (default: ./_work)
#   INCLUDE_DIAGNOSTIC=1      also apply patches/diagnostic/*.mypatch (env-gated probes)
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${NEUTRON_WINE_WORK:-$REPO/_work}"
TKG="$WORK/wine-tkg-git"

mkdir -p "$WORK"

# 1. Upstream wine-tkg build framework (pinned base is set via customization.cfg).
if [ ! -d "$TKG/.git" ]; then
    git clone https://github.com/Frogging-Family/wine-tkg-git "$TKG"
fi
TKGDIR="$TKG/wine-tkg-git"

# 2. Pin the wine base/flavor (wine-11.10 staging + ntsync — see ../build/WINE_BASE).
cp "$REPO/build/customization.cfg" "$TKGDIR/customization.cfg"

# 3. Apply the Neutron patch set (wine-tkg applies every *.mypatch in userpatches).
mkdir -p "$TKGDIR/wine-tkg-userpatches"
cp "$REPO"/patches/*.mypatch "$TKGDIR/wine-tkg-userpatches/"
if [ "${INCLUDE_DIAGNOSTIC:-0}" = "1" ]; then
    cp "$REPO"/patches/diagnostic/*.mypatch "$TKGDIR/wine-tkg-userpatches/"
fi

# 3b. Force non-interactive userpatch application. The default-tkg preset sources BOTH
# customization.cfg AND wine-tkg-profiles/advanced-customization.cfg, and the latter is
# sourced LAST and defaults _user_patches_no_confirm="false" — which overrides our
# customization.cfg and makes wine-tkg hit an interactive prompt that, with no TTY input,
# silently SKIPS every userpatch (producing a stock wine). Force the setting in every cfg
# the preset may read so the patches always auto-apply on a non-interactive build.
for _cfg in "$TKGDIR/customization.cfg" "$TKGDIR/wine-tkg-profiles/advanced-customization.cfg"; do
    [ -f "$_cfg" ] || continue
    sed -i 's/^_user_patches=.*/_user_patches="true"/'                       "$_cfg"
    sed -i 's/^_user_patches_no_confirm=.*/_user_patches_no_confirm="true"/' "$_cfg"
done

# 4. Build (non-makepkg flow). Output lands in src/<flavor>-build/.
cd "$TKGDIR"
./non-makepkg-build.sh

echo
echo "Built. The Neutron wine is under: $TKGDIR/src/*-build/wine"
echo "Use it:  NEUTRON_WINE=\"$TKGDIR/src/<flavor>-build/wine\" neutron launch premiere"
