#!/usr/bin/env bash
# package.sh — turn a built Neutron wine tree into a versioned release artifact.
#
# The Proton-GE model: `build.sh` produces a complete Wine tree; this script wraps
# that tree into the tarball + checksum + manifest that get attached to a GitHub
# Release and resolved by Neutron's `neutron runtime install`.
#
# Outputs (into ./dist/):
#   neutron-wine-<version>.tar.xz            the compiled Wine fork (the "wine core")
#   neutron-wine-<version>.tar.xz.sha256     checksum (sha256sum -c compatible)
#   neutron-wine-<version>.manifest.json     machine-readable release manifest
#   neutron-wine-<version>-source.tar.xz     complete source of this release (build/source-archive.sh)
#
# Usage:
#   build/package.sh [path-to-wine-tree]
# Env:
#   NEUTRON_WINE_TREE=<dir>   the built Wine tree (contains bin/, lib/, share/).
#                             Default: the tree build.sh leaves under ./_work.
#   NEUTRON_REPO_SLUG=<o/r>   GitHub owner/repo for the download URL
#                             (default: Nico-LaFoucate/neutron-wine).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(cat "$REPO/build/VERSION")"
WINE_BASE="$(cat "$REPO/build/WINE_BASE")"

# NEUTRON: refuse to package a tree whose build did not succeed for THIS version.
#
# 2026-08-31: build.sh failed applying a patch and exited 1. package.sh then packaged the stale
# tree left from the PREVIOUS version and produced a complete, correctly-checksummed
# "neutron-wine-11.10-69.tar.xz" that actually contained 11.10-68. `sha256sum -c` said OK, the
# file list looked right, and nothing anywhere said the build had failed -- it was caught only
# because the build's exit code happened to be checked by hand. That is the 11.10-61 failure mode
# reached by a different route, so it gets a GATE, not a rule in a document.
#
# ⭐ A checksum proves a file arrived intact. It never proves the file contains what its name says.
_BUILD_OK="${NEUTRON_WINE_WORK:-$REPO/_work}/.build-ok"
if [ ! -f "$_BUILD_OK" ]; then
    echo "package.sh: no successful build recorded ($_BUILD_OK missing)." >&2
    echo "  Run build/build.sh first. Without this, a tree left from an earlier version would be" >&2
    echo "  packaged under the NEW version number and its checksum would verify perfectly." >&2
    exit 1
fi
_BUILT="$(cat "$_BUILD_OK")"
if [ "$_BUILT" != "$VERSION" ]; then
    echo "package.sh: the last successful build was $_BUILT, but build/VERSION says $VERSION." >&2
    echo "  Packaging now would ship the $_BUILT tree labelled $VERSION. Re-run build/build.sh." >&2
    exit 1
fi
SLUG="${NEUTRON_REPO_SLUG:-Nico-LaFoucate/neutron-wine}"
NAME="neutron-wine-$VERSION"
DIST="$REPO/dist"

# --- locate the built Wine tree -------------------------------------------------
TREE="${1:-${NEUTRON_WINE_TREE:-}}"
if [ -z "$TREE" ]; then
    # default: the COMPLETE installed tree build.sh leaves under _work (bin/ lib/ share/).
    # Prefer non-makepkg-builds/<flavor> (the `make install` output) over src/*-build/wine
    # (a raw build dir; the glob there can also match the 32-bit stub build first).
    TREE="$(echo "$REPO"/_work/wine-tkg-git/wine-tkg-git/non-makepkg-builds/*/bin/wine | awk '{print $1}')"
    TREE="${TREE%/bin/wine}"
    if [ ! -x "$TREE/bin/wine" ]; then
        TREE="$(echo "$REPO"/_work/wine-tkg-git/wine-tkg-git/src/*-build/wine | awk '{print $1}')"
    fi
fi
if [ ! -d "$TREE" ] || { [ ! -x "$TREE/bin/wine" ] && [ ! -x "$TREE/bin/wine64" ]; }; then
    echo "error: no built Wine tree at: ${TREE:-<unset>}" >&2
    echo "       run build/build.sh first, or pass the tree path / set NEUTRON_WINE_TREE." >&2
    exit 1
fi
TREE="$(cd "$TREE" && pwd)"

# --- DXVK, vkd3d-proton, NVIDIA wrappers, ucrtbase shim -------------------------
# Built from external/sources.conf + external/patches and installed into the tree (lib/wine/<project>/,
# share/neutron/natives.json, licenses/<project>/) BEFORE the gates below, so they cover these too.
bash "$REPO/external/build-external.sh" "$TREE" || { echo "error: external/build-external.sh failed" >&2; exit 8; }

# ---------------------------------------------------------------------------
# NO-HOME-PATH GATE. Nothing we ship may carry the builder's home directory. Wine compiles its
# install prefix into ntdll.so (BINDIR/LIBDIR), so a tree built under $HOME embeds it in the
# binaries themselves. Build releases from a neutral work dir:
#     NEUTRON_WINE_WORK=/var/tmp/neutron-wine build/build.sh
# Checked on the BUILT files, not the source: that is where 11.10-97 was caught.
# ---------------------------------------------------------------------------
_homehits="$(grep -rlF "$HOME/" "$TREE" 2>/dev/null | head -5 || true)"
if [ -n "$_homehits" ]; then
    echo "package.sh: ⛔ the built tree contains the home path ($HOME/):" >&2
    printf '    %s\n' $_homehits >&2
    echo "  Rebuild from a neutral work dir: NEUTRON_WINE_WORK=/var/tmp/neutron-wine build/build.sh" >&2
    exit 6
fi

# ---------------------------------------------------------------------------
# BASE-DRIFT GATE. `share/wine/wine.inf` is GENERATED from wine.inf.in at build time, so it is a
# faithful fingerprint of the base tree the build actually saw. Compare it against EVERY installed
# runtime, not just the newest.
#
# Why plural: cutting 11.10-62 its wine.inf differed from 61's, I compared only those two and
# concluded upstream had drifted. It had not — 62 is byte-identical to 45, 58, 59 and 60, and *61*
# was the outlier: an incremental `make` on a tree wine-tkg had already partially reverted in its
# exit cleanup, so wine.inf regenerated from partially-staged source. A single-predecessor
# comparison cannot tell you WHICH of the two is wrong. This one can.
# ---------------------------------------------------------------------------
_inf="$TREE/share/wine/wine.inf"
if [ ! -f "$_inf" ]; then
    echo "package: NOTE - no share/wine/wine.inf in the tree; base-drift gate skipped." >&2
fi
if [ -f "$_inf" ]; then
    _same=0; _diff=0; _difflist=""
    for _other in "$HOME"/.local/share/neutron/runtimes/neutron-wine-*/share/wine/wine.inf; do
        [ -f "$_other" ] || continue
        case "$_other" in *"neutron-wine-$VERSION/"*) continue ;; esac
        if cmp -s "$_inf" "$_other"; then _same=$((_same+1))
        else _diff=$((_diff+1))
             _difflist="$_difflist $(basename "$(dirname "$(dirname "$(dirname "$_other")")")")"
        fi
    done
    if [ "$_same" -eq 0 ] && [ "$_diff" -eq 0 ]; then
        # ⛔ SAY SO. The first run of this gate compared against a runtime dir that did not exist
        # yet (the version being packaged is not installed until after packaging), so every
        # comparison was skipped and the gate printed NOTHING -- indistinguishable from "all
        # clear". A gate that cannot run must never look like a gate that passed.
        echo "package: ⚠️  base-drift gate had NOTHING to compare against (no other runtime" >&2
        echo "   installed). This build's base is UNVERIFIED, not verified." >&2
    fi
    if [ "$_same" -eq 0 ] && [ "$_diff" -gt 0 ]; then
        echo "package: ⛔ BASE DRIFT — this build's wine.inf matches NO installed runtime ($_diff differ)." >&2
        echo "   The base tree changed under us. Investigate before shipping; do NOT pin the mtime." >&2
        exit 4
    fi
    if [ "$_diff" -gt 0 ]; then
        echo "package: NOTE — wine.inf matches $_same installed runtime(s) and differs from $_diff:$_difflist" >&2
        echo "   Matching the majority is the healthy case; the ones that differ are the suspect builds." >&2
    fi
fi
echo "packaging Wine tree: $TREE"
echo "release:             $NAME  (base: $WINE_BASE)"

# --- stamp a version marker inside the bundle root ------------------------------
printf '%s\n' "$VERSION" > "$TREE/NEUTRON_WINE_VERSION"

# --- stage version-matched Wine Mono + Gecko INTO the bundle --------------------
# Without this the tester gets a "Wine Mono Installer" popup on first boot / prefix
# update, because wineboot cannot find a version-matched tree: our build asks for a
# specific wine-mono version and the host's /usr/share/wine/mono is whatever the
# distro shipped (here: 11.2.0 vs the 11.1.0 our build wants). The popup blocks an
# unattended install and, if dismissed, leaves .NET-backed Adobe bits broken.
#
# ⚠ Derive the versions from the BUILT BINARIES, never hardcode them — a skew between
# what the build expects and what we ship is the whole failure mode. appwiz.cpl and
# mshtml.dll are PE files, so the strings are UTF-16 (`strings -el`).
MONO_VER="$(strings -a -el "$TREE/lib/wine/x86_64-windows/appwiz.cpl" 2>/dev/null \
            | command grep -oP 'wine-mono-\K[0-9.]+(?=-x86\.msi)' | head -1)"
GECKO_VER="$(strings -a -el "$TREE/lib/wine/x86_64-windows/mshtml.dll" 2>/dev/null \
            | command grep -oP 'wine-gecko-\K[0-9.]+(?=-x86_64)' | head -1)"
[ -n "$MONO_VER" ]  || { echo "error: could not read the expected wine-mono version from appwiz.cpl" >&2; exit 4; }
[ -n "$GECKO_VER" ] || { echo "error: could not read the expected wine-gecko version from mshtml.dll" >&2; exit 4; }
echo "wine-mono:           $MONO_VER (required by this build)"
echo "wine-gecko:          $GECKO_VER (required by this build)"

# Source trees, in priority order. These are runtime artifacts, NOT in git — keep them
# staged under ~/neutron/{mono,gecko} (same trees `neutron runtime capture` uses).
find_tree() {  # find_tree <subdir> <leafname>
    local d
    for d in "${NEUTRON_MONO_GECKO_SRC:-}" "$HOME/neutron/dist/$1" "$HOME/neutron/$1" \
             "/usr/share/wine/$1" "/opt/wine/$1"; do
        [ -n "$d" ] && [ -d "$d/$2" ] && { echo "$d/$2"; return 0; }
    done
    return 1
}

stage() {  # stage <subdir> <leafname>
    local src dst="$TREE/share/wine/$1/$2"
    if [ -d "$dst" ]; then echo "  = share/wine/$1/$2 (already staged)"; return 0; fi
    if ! src="$(find_tree "$1" "$2")"; then
        # Not staged locally: fetch WineHQ's official build of exactly this version.
        local url cache="${XDG_CACHE_HOME:-$HOME/.cache}/neutron-wine/$1"
        case "$1" in
            mono)  url="https://dl.winehq.org/wine/wine-mono/$MONO_VER/$2-x86.tar.xz" ;;
            gecko) url="https://dl.winehq.org/wine/wine-gecko/$GECKO_VER/$2.tar.xz" ;;
        esac
        mkdir -p "$cache"
        echo "  fetching $url"
        if ! curl -fsSL --retry 3 -o "$cache/$2.tar.xz" "$url" \
           || ! tar -C "$cache" -xJf "$cache/$2.tar.xz"; then
            echo "error: no $2 tree found locally and the WineHQ download failed ($url)." >&2
            echo "       The release would pop the Wine Mono/Gecko installer on first boot." >&2
            exit 5
        fi
        src="$cache/$2"
        [ -d "$src" ] || { echo "error: $url did not unpack to $2/" >&2; exit 5; }
    fi
    mkdir -p "$(dirname "$dst")"
    cp -a "$src" "$dst"
    echo "  + share/wine/$1/$2  <- $src"
}

stage mono  "wine-mono-$MONO_VER"
stage gecko "wine-gecko-$GECKO_VER-x86"
stage gecko "wine-gecko-$GECKO_VER-x86_64"

# --- licenses/ + SOURCE --------------------------------------------------------
# Every component's license travels with the binaries, and SOURCE names the exact recipe this
# release was built from, so the published repo can rebuild what we shipped (LGPL).
WINE_SRC="${NEUTRON_WINE_WORK:-$REPO/_work}/wine-tkg-git/wine-tkg-git/src/wine-git"
rm -rf "$TREE/licenses/wine"; mkdir -p "$TREE/licenses/wine"
for f in COPYING.LIB LICENSE AUTHORS COPYING.arial COPYING.cour COPYING.msyh COPYING.times; do
    [ -f "$WINE_SRC/$f" ] && cp "$WINE_SRC/$f" "$TREE/licenses/wine/"
done
[ -f "$TREE/licenses/wine/COPYING.LIB" ] \
    || { echo "error: Wine's COPYING.LIB not found under $WINE_SRC" >&2; exit 7; }
cat > "$TREE/licenses/README" <<LICENSES
neutron-wine $VERSION bundles the following components.

Wine (patched)    LGPL-2.1-or-later   licenses/wine/ (COPYING.LIB, LICENSE, AUTHORS, font licenses)
Wine Mono $MONO_VER   MIT and others     https://gitlab.winehq.org/mono/wine-mono
Wine Gecko $GECKO_VER  MPL-2.0            https://dl.winehq.org/wine/wine-gecko/$GECKO_VER/

DXVK              zlib                licenses/dxvk/          (d3d8, d3d9, d3d10core, d3d11, dxgi)
vkd3d-proton      LGPL-2.1            licenses/vkd3d-proton/  (d3d12, d3d12core)
dxvk-nvapi        MIT                 licenses/dxvk-nvapi/    (nvapi, nvapi64, nvofapi64)
nvcuda            LGPL-2.1            licenses/nvcuda/
nvenc             LGPL-2.1            licenses/nvenc/         (nvcuvid, nvencodeapi64)
wine-nvoptix      LGPL-2.1 and MIT    licenses/wine-nvoptix/  (nvoptix)
ucrtbase shim     LGPL-2.1-or-later   part of neutron-wine (external/ucrtshim)
Exact upstream commits, and which of these carry Neutron patches: share/neutron/natives.json.
The complete source of this release is attached to it as $NAME-source.tar.xz.

Neutron is an independent project by Nico LaFoucate and Ficus Media Group. Adobe and its product
names are trademarks of Adobe Inc. Neutron is not affiliated with or endorsed by Adobe.
LICENSES
SRC_COMMIT="$(git -C "$REPO" rev-parse HEAD)"
git -C "$REPO" diff --quiet HEAD -- patches build || SRC_COMMIT="$SRC_COMMIT (with uncommitted changes)"
TKG_COMMIT="$(cat "$REPO/build/WINE_TKG_COMMIT")"
cat > "$TREE/SOURCE" <<SOURCEFILE
neutron-wine $VERSION
Source:        https://github.com/$SLUG
Recipe commit: $SRC_COMMIT
Wine base:     $WINE_BASE
wine-tkg:      https://github.com/Frogging-Family/wine-tkg-git @ $TKG_COMMIT
External:      $(awk '!/^#/ && NF { printf "%s%s @ %s%s", sep, $2, $3, ($4 == "-" ? "" : " + external/patches/" $4); sep = "\n               " }' "$REPO/external/sources.conf")
Full source:   $NAME-source.tar.xz (attached to the release)
Rebuild:       see RELEASING.md in the source repository.
SOURCEFILE

# --- archive: contents land under  <NAME>/  ------------------------------------
mkdir -p "$DIST"
TARBALL="$DIST/$NAME.tar.xz"
BASE="$(basename "$TREE")"
echo "compressing -> $TARBALL"
tar -C "$(dirname "$TREE")" \
    --transform "s,^$BASE,$NAME," \
    --owner=0 --group=0 \
    -cJf "$TARBALL" "$BASE"

# --- checksum + manifest --------------------------------------------------------
bash "$REPO/build/source-archive.sh" "$DIST/$NAME-source.tar.xz" \
    || { echo "error: build/source-archive.sh failed" >&2; exit 9; }
( cd "$DIST" && sha256sum "$NAME.tar.xz" > "$NAME.tar.xz.sha256" )
SHA="$(awk '{print $1}' "$DIST/$NAME.tar.xz.sha256")"
SIZE="$(stat -c%s "$TARBALL")"
URL="https://github.com/$SLUG/releases/download/v$VERSION/$NAME.tar.xz"

cat > "$DIST/$NAME.manifest.json" <<JSON
{
  "name": "neutron-wine",
  "version": "$VERSION",
  "wine_base": "$WINE_BASE",
  "artifact": "$NAME.tar.xz",
  "sha256": "$SHA",
  "size_bytes": $SIZE,
  "url": "$URL",
  "wine_bin": "bin/wine",
  "unpack_root": "$NAME",
  "source_commit": "$SRC_COMMIT",
  "wine_tkg_commit": "$TKG_COMMIT",
  "source_archive": "$NAME-source.tar.xz"
}
JSON

echo
echo "done. dist/:"
ls -1 "$DIST" | sed 's/^/  /'
echo
echo "Next: create a GitHub release tagged v$VERSION and upload this version's four dist/ files."
echo "      (see RELEASING.md)"
