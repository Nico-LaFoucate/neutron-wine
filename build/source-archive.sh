#!/usr/bin/env bash
# source-archive.sh — the complete source of one neutron-wine release, attached to its GitHub
# release next to the binaries (LGPL: the source sits in the same place as the download).
#
#   build/source-archive.sh <out.tar.xz>
#
# Layout inside the archive (top dir = the archive's base name):
#   neutron-wine/    this repository as built (patches, build scripts, external/)
#   wine/            Wine at the pinned tag
#   wine-staging/    wine-staging at the pinned tag
#   wine-tkg-git/    wine-tkg at build/WINE_TKG_COMMIT
#   external/<name>/ every external project exactly as compiled: the pinned commit with our
#                    patches applied, submodules included
# Called by package.sh after build.sh and external/build-external.sh have run.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUTFILE="${1:?usage: source-archive.sh <out.tar.xz>}"
WORK="${NEUTRON_WINE_WORK:-$REPO/_work}"
EXT_WORK="${NEUTRON_EXT_WORK:-/var/tmp/neutron-external}"
TKG="$WORK/wine-tkg-git"
SRC="$TKG/wine-tkg-git/src"
die() { echo "source-archive: $*" >&2; exit 1; }

TOP="$(basename "$OUTFILE" .tar.xz)"
STAGE="$(mktemp -d "${TMPDIR:-/var/tmp}/neutron-source.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$TOP"

export_git() {  # export_git <repo> <rev> <dest>
    [ -d "$1/.git" ] || [ -f "$1/.git" ] || die "no git checkout at $1"
    mkdir -p "$STAGE/$TOP/$3"
    git -C "$1" archive --format=tar "$2" | tar -C "$STAGE/$TOP/$3" -xf -
}

# This repo: the working tree as built (tracked + untracked, minus ignored), not just HEAD.
mkdir -p "$STAGE/$TOP/neutron-wine"
git -C "$REPO" ls-files -z -co --exclude-standard \
    | tar -C "$REPO" --null -T - -cf - | tar -C "$STAGE/$TOP/neutron-wine" -xf -

PLAIN="$(sed -n 's/^_plain_version="\(.*\)"/\1/p' "$REPO/build/customization.cfg")"
STAGING="$(sed -n 's/^_staging_version="\(.*\)"/\1/p' "$REPO/build/customization.cfg")"
[ -n "$PLAIN" ] && [ -n "$STAGING" ] || die "could not read _plain_version/_staging_version from customization.cfg"
export_git "$SRC/wine-git"         "refs/tags/$PLAIN"   wine
export_git "$SRC/wine-staging-git" "refs/tags/$STAGING" wine-staging
export_git "$TKG"                  "$(cat "$REPO/build/WINE_TKG_COMMIT")" wine-tkg-git

[ -d "$EXT_WORK/src" ] || die "no external sources at $EXT_WORK/src (run external/build-external.sh first)"
for d in "$EXT_WORK"/src/*/; do
    n="$(basename "$d")"
    mkdir -p "$STAGE/$TOP/external/$n"
    tar -C "$d" --exclude=.git --exclude='.git/*' -cf - . | tar -C "$STAGE/$TOP/external/$n" -xf -
done

cat > "$STAGE/$TOP/README" <<README
Complete source for $TOP.

neutron-wine/   the neutron-wine repository as built: Wine patches (patches/), build scripts
                (build/), and the DXVK / vkd3d-proton / NVIDIA wrapper sources and patches
                (external/). See neutron-wine/RELEASING.md to rebuild.
wine/           Wine $PLAIN
wine-staging/   wine-staging $STAGING
wine-tkg-git/   wine-tkg $(cat "$REPO/build/WINE_TKG_COMMIT")
external/       each external project as compiled (pinned commit + neutron-wine/external/patches)
README

echo "source-archive: compressing -> $OUTFILE"
XZ_OPT="${XZ_OPT:--T0 -6}" tar -C "$STAGE" --owner=0 --group=0 --sort=name -cJf "$OUTFILE" "$TOP"
echo "source-archive: $(du -h "$OUTFILE" | cut -f1)"
