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
SLUG="${NEUTRON_REPO_SLUG:-Nico-LaFoucate/neutron-wine}"
NAME="neutron-wine-$VERSION"
DIST="$REPO/dist"

# --- locate the built Wine tree -------------------------------------------------
TREE="${1:-${NEUTRON_WINE_TREE:-}}"
if [ -z "$TREE" ]; then
    # default: what build.sh produces under _work
    TREE="$(echo "$REPO"/_work/wine-tkg-git/wine-tkg-git/src/*-build/wine | awk '{print $1}')"
fi
if [ ! -d "$TREE" ] || { [ ! -x "$TREE/bin/wine" ] && [ ! -x "$TREE/bin/wine64" ]; }; then
    echo "error: no built Wine tree at: ${TREE:-<unset>}" >&2
    echo "       run build/build.sh first, or pass the tree path / set NEUTRON_WINE_TREE." >&2
    exit 1
fi
TREE="$(cd "$TREE" && pwd)"
echo "packaging Wine tree: $TREE"
echo "release:             $NAME  (base: $WINE_BASE)"

# --- stamp a version marker inside the bundle root ------------------------------
printf '%s\n' "$VERSION" > "$TREE/NEUTRON_WINE_VERSION"

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
  "unpack_root": "$NAME"
}
JSON

echo
echo "done. dist/:"
ls -1 "$DIST" | sed 's/^/  /'
echo
echo "Next: create a GitHub release tagged v$VERSION and upload the three dist/ files."
echo "      (see RELEASING.md)"
