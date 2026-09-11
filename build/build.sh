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

# 2a. NEUTRON: build on DISK, not tmpfs.
# Upstream wine-tkg hardcodes _build_in_tmpfs="true" and symlinks src -> /tmp/wine-tkg/src.
# That makes builds fast but the tree evaporates on reboot: a full ~1h rebuild is then needed
# for any change, and an incremental relink is impossible. It cost an hour on 2026-09-06 and
# again on 09-07. Disk has terabytes; RAM does not. Idempotent (the pattern stops matching).
if [ -L "$TKGDIR/src" ]; then
    echo "neutron: removing the tmpfs src symlink ($(readlink "$TKGDIR/src"))"
    rm -f "$TKGDIR/src"
fi
sed -i 's|^_build_in_tmpfs="true"$|_build_in_tmpfs="false"|' "$TKGDIR/non-makepkg-build.sh"
grep -q '^_build_in_tmpfs="false"$' "$TKGDIR/non-makepkg-build.sh" \
    || { echo "neutron: FAILED to disable tmpfs builds -- upstream may have changed line 43"; exit 1; }
echo "neutron: build tree on disk -> $TKGDIR/src"

# 3. Apply the Neutron patch set (wine-tkg applies every *.mypatch in userpatches).
# Prune first: a stale *.mypatch left from a previous build (one we've since renamed
# or dropped) would still be applied and can fail/conflict against the current set.
mkdir -p "$TKGDIR/wine-tkg-userpatches"
rm -f "$TKGDIR"/wine-tkg-userpatches/*.mypatch "$TKGDIR"/*.mypatch
cp "$REPO"/patches/*.mypatch "$TKGDIR/wine-tkg-userpatches/"
if [ "${INCLUDE_DIAGNOSTIC:-0}" = "1" ]; then
    cp "$REPO"/patches/diagnostic/*.mypatch "$TKGDIR/wine-tkg-userpatches/"
fi

# 3a. GATE: verify patch ORDERING before spending a build on it.
# wine-tkg applies userpatches in the SYSTEM LOCALE's collation, which ignores punctuation
# at the primary level -- so "zzz2-" sorts BEFORE "zzz-" even though ASCII says otherwise.
# That cost a build on 2026-07-30, and it failed dangerously: only one hunk errored while
# the rest applied with fuzz at offsets of -89..-163 lines against the wrong base. A
# luckier misapply would have built silently-wrong binaries.
ORDER_CHECK="$HOME/neutron/preserved-fixes/harnesses/check-patch-order.py"
if [ -f "$ORDER_CHECK" ]; then
    if ! python3 "$ORDER_CHECK" "$REPO/patches"; then
        echo "build.sh: patch ordering check FAILED -- fix the names before building." >&2
        exit 3
    fi
else
    echo "build.sh: WARNING -- $ORDER_CHECK not found, ordering unverified" >&2
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
#
# 🚨 PIN THE COLLATION. wine-tkg applies userpatches in whatever order the shell globs them, which
# uses LC_COLLATE. Our entire `neutron-zzz…` naming scheme encodes APPLY ORDER and assumes C
# collation (byte order, punctuation significant). Under the machine's en_US.UTF-8 locale
# punctuation is IGNORED, which REVERSES pairs that differ only by a separator:
#
#   C            : neutron-zzzzzzzzzz-diagnostics-gated-for-release  then  neutron-zzzzzzzzzza-show-trace-gate
#   en_US.UTF-8  : neutron-zzzzzzzzzza-show-trace-gate               then  neutron-zzzzzzzzzz-diagnostics-gated-for-release
#
# The second patch REFACTORS code the first one introduces, so the reversed order fails
# ("Hunk #4 FAILED at 5262") and the whole prepare aborts. This bit on 2026-08-25, the first
# from-scratch prepare after a reboot wiped /tmp — earlier builds reused an already-patched tree
# and never re-applied, so it stayed hidden. `mkpatch.sh emit` had been WARNING about this exact
# pair for weeks ("order differs between collations AND hunks are close").
#
# LC_COLLATE only affects sort/glob order, so this cannot change what gets compiled.
export LC_COLLATE=C

# --- toolbar artwork ------------------------------------------------------------------------
# Wine's comctl32 history/view toolbar strips are glossy Tango bitmaps — the bright green arrows
# in the file dialog every Adobe app raises. We replace them with flat glyphs, in two palettes
# (light glyphs for dark chrome, dark for light); comctl32 picks by COLOR_BTNFACE luminance.
#
# ⛔ WHY THIS IS A BUILD STEP AND NOT PART OF THE PATCH. Two independent reasons:
#   1. wine-tkg applies userpatches with `patch -Np1`, which CANNOT apply binary diffs, and the
#      artwork that actually ships is .bmp.
#   2. Wine only regenerates .bmp from .svg in MAINTAINER MODE, which additionally requires an
#      in-tree build (`srcdir = .`). Ours is out-of-tree, so that path is closed —
#      RSVG/CONVERT/ICOTOOL are all empty in our generated Makefile. Verified, not assumed.
# So the generator writes both the .svg (our source) and the .bmp (what the resource compiler
# eats) straight into the tree, and the .mypatch carries only the C/rc/h that reference them.
_ART="$REPO/build/art/gen_toolbar_icons.py"
_ART_DEST="$WORK/wine-tkg-git/wine-tkg-git/src/wine-git/dlls/comctl32"
if [ -f "$_ART" ] && [ -d "$_ART_DEST" ]; then
    for _t in rsvg-convert magick; do
        command -v "$_t" >/dev/null || { echo "build.sh: need $_t to generate toolbar artwork" >&2; exit 6; }
    done
    echo "generating comctl32 toolbar artwork"
    python3 "$_ART" "$_ART_DEST" >/dev/null || { echo "build.sh: toolbar artwork generation FAILED" >&2; exit 6; }
elif [ -f "$_ART" ]; then
    # First bootstrap: the source tree does not exist yet, so there is nothing to write into.
    # The stock artwork gets compiled this once; the next build replaces it.
    echo "build.sh: NOTE - no source tree yet, skipping toolbar artwork (stock icons this build)" >&2
fi

cd "$TKGDIR"
./non-makepkg-build.sh

echo
echo "Built. The Neutron wine is under: $TKGDIR/src/*-build/wine"
echo "Use it:  NEUTRON_WINE=\"$TKGDIR/src/<flavor>-build/wine\" neutron launch premiere"
# NEUTRON: record that THIS version built successfully. package.sh refuses without it.
# 2026-08-31: a build failed on a rejected patch and package.sh cheerfully packaged the STALE tree
# from the previous version, producing a complete, correctly-checksummed "11.10-69" tarball that
# was actually 11.10-68. Nothing in its output said so. Only checking build.sh's exit code caught
# it. A checksum proves a file arrived intact, never that it contains what its name claims.
printf '%s\n' "$(cat "$REPO/build/VERSION")" > "$WORK/.build-ok"

echo
echo "To cut a release, package the tree into a versioned tarball + manifest:"
echo "  build/package.sh          # -> dist/  (see RELEASING.md)"
