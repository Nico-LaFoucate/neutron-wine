#!/usr/bin/env bash
# mkpatch.sh — generate a Neutron *.mypatch against the CORRECT base tree.
#
# WHY THIS EXISTS
# ---------------
# Four builds were lost to generating patches against the wrong tree. The failure is silent:
# the patch looks fine, build.sh applies it with fuzz or rejects hunks, and you only find out
# an hour later. The traps, all of which actually happened:
#
#   1. THERE ARE SEVERAL wine-git TREES ON THIS MACHINE and they are not interchangeable:
#        <repo>/_work/wine-tkg-git/wine-tkg-git/src/wine-git   <- the ONLY correct base
#        ~/wine-tkg-git/wine-tkg-git/src/wine-git              <- hand-edited dev tree
#        /tmp/wine-tkg/src/wine-git                            <- stale, from an old
#                                                                 NEUTRON_WINE_WORK
#      Only the first is "staging base + every patches/*.mypatch", which is what a NEW patch
#      must apply on top of. This script resolves it the same way build.sh does and refuses to
#      run against any other.
#
#   2. `git diff HEAD` IN THE BASE TREE RE-EMITS EVERY OTHER PATCH'S HUNKS. The base tree is a
#      git checkout with all patches applied but uncommitted, so git diff shows the whole patch
#      set, not your change. (6/6 hunks rejected.) This script diffs a per-file SNAPSHOT taken
#      immediately before you edit, so the patch contains only your delta.
#
#   3. HAND-EDITING UNIFIED-DIFF HEADERS CORRUPTS THE PATCH. `tail -n +5` to strip a header ate
#      the second file's ---/+++ lines; recomputing @@ counts by hand produced "malformed patch
#      at line 294" twice. This script never edits diff output.
#
# USAGE
#   mkpatch.sh base                       show the resolved base tree (and flag stale twins)
#   mkpatch.sh snapshot FILE [FILE...]    BEFORE editing: save pristine copies
#                                         FILE is repo-relative, e.g. dlls/win32u/clipboard.c
#   mkpatch.sh status                     what is snapshotted / what changed
#   mkpatch.sh emit NAME [-m MSGFILE]     AFTER editing: write patches/NAME.mypatch + verify
#   mkpatch.sh reset                      discard edits, restore the snapshot
#   mkpatch.sh clean                      drop the snapshot without touching the tree
#
# Env: NEUTRON_WINE_WORK — same override build.sh honours.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${NEUTRON_WINE_WORK:-$REPO/_work}"
BASE="$WORK/wine-tkg-git/wine-tkg-git/src/wine-git"
SNAP="$WORK/.mkpatch-snapshot"
PATCHDIR="$REPO/patches"

die() { echo "mkpatch: $*" >&2; exit 1; }

# --- base tree resolution -----------------------------------------------------------------
# Any other wine-git tree on this box is a trap, not an alternative. Name them so a wrong
# base is caught here rather than an hour into a build.
DECOYS=(
    "$HOME/wine-tkg-git/wine-tkg-git/src/wine-git"
    "/tmp/wine-tkg/src/wine-git"
)

check_base() {
    [ -d "$BASE" ] || die "base tree not found: $BASE
  Run build/build.sh at least once — the base tree is created by the build."
    [ -f "$BASE/dlls/win32u/clipboard.c" ] || die "$BASE does not look like a wine tree"
}

# Fingerprint of the patch set the base tree was built from. If this changes between snapshot
# and emit, the base moved under you and the delta is no longer trustworthy.
patchset_fingerprint() {
    local exclude="${1:-}"
    local f
    for f in "$PATCHDIR"/*.mypatch; do
        [ -e "$f" ] || continue
        [ -n "$exclude" ] && [ "$(basename "$f")" = "$exclude" ] && continue
        printf '%s  %s\n' "$(md5sum < "$f" | cut -d' ' -f1)" "$(basename "$f")"
    done | sort | md5sum | cut -d' ' -f1
}

cmd_base() {
    check_base
    echo "base tree (correct):  $BASE"
    if [ -d "$BASE/.git" ]; then
        echo "  wine HEAD:          $(git -C "$BASE" rev-parse --short HEAD 2>/dev/null || echo '?')"
    fi
    echo "  patches applied:    $(ls -1 "$PATCHDIR"/*.mypatch 2>/dev/null | wc -l) (fingerprint $(patchset_fingerprint))"
    local d found=0
    for d in "${DECOYS[@]}"; do
        if [ -d "$d" ]; then
            [ $found -eq 0 ] && echo && echo "  ⛔ DO NOT diff against these — they are NOT the build base:"
            found=1
            echo "     $d"
        fi
    done
}

# --- snapshot -----------------------------------------------------------------------------
cmd_snapshot() {
    check_base
    [ $# -gt 0 ] || die "snapshot needs at least one repo-relative file path"
    mkdir -p "$SNAP/files"
    local rel
    for rel in "$@"; do
        rel="${rel#./}"
        # Accept an absolute path inside the base tree and normalise it.
        case "$rel" in "$BASE"/*) rel="${rel#"$BASE"/}" ;; esac
        if [ ! -f "$BASE/$rel" ]; then
            # NEW FILE: nothing to copy. Record it so emit diffs it against /dev/null, which
            # `patch -Np1` applies as a file creation (hunk @@ -0,0 +1,N @@).
            mkdir -p "$SNAP/new/$(dirname "$rel")"
            : > "$SNAP/new/$rel"
            echo "  + $rel (NEW FILE - will be diffed against /dev/null)"
            echo "$rel" >> "$SNAP/manifest"
            continue
        fi
        if [ -f "$SNAP/files/$rel" ]; then
            echo "  = $rel (already snapshotted, keeping the ORIGINAL pristine copy)"
            continue
        fi
        mkdir -p "$(dirname "$SNAP/files/$rel")"
        cp -p "$BASE/$rel" "$SNAP/files/$rel"
        echo "  + $rel"
        echo "$rel" >> "$SNAP/manifest"
    done
    sort -u -o "$SNAP/manifest" "$SNAP/manifest"
    patchset_fingerprint > "$SNAP/fingerprint"
    echo "$BASE" > "$SNAP/base"
    echo
    echo "Snapshot taken. Edit the files IN THE BASE TREE:"
    echo "  $BASE"
    echo "Then: build/mkpatch.sh emit <patch-name>"
}

require_snapshot() {
    [ -f "$SNAP/manifest" ] || die "no snapshot — run 'mkpatch.sh snapshot <files>' BEFORE editing"
    local snapbase; snapbase="$(cat "$SNAP/base")"
    [ "$snapbase" = "$BASE" ] || die "snapshot was taken against a different base tree:
  snapshot: $snapbase
  now:      $BASE"
}

# A file snapshotted while absent from the base tree is a NEW file: its pristine copy is /dev/null.
is_new() { [ -e "$SNAP/new/$1" ]; }
snap_path() { if is_new "$1"; then echo /dev/null; else echo "$SNAP/files/$1"; fi; }

cmd_status() {
    check_base; require_snapshot
    local rel changed=0
    while read -r rel; do
        if is_new "$rel"; then
            if [ -f "$BASE/$rel" ]; then echo "  A $rel (new file)"; changed=1
            else echo "  = $rel (new file, not created yet)"; fi
            continue
        fi
        if cmp -s "$SNAP/files/$rel" "$BASE/$rel"; then
            echo "  = $rel (unchanged)"
        else
            echo "  M $rel  ($(diff -u "$SNAP/files/$rel" "$BASE/$rel" | command grep -c '^@@') hunks)"
            changed=1
        fi
    done < "$SNAP/manifest"
    [ $changed -eq 1 ] || echo "  (nothing edited yet)"
}

cmd_reset() {
    check_base; require_snapshot
    local rel
    while read -r rel; do
        if is_new "$rel"; then rm -f "$BASE/$rel"; echo "  removed $rel (new file)"; continue; fi
        cp -p "$SNAP/files/$rel" "$BASE/$rel"
        echo "  restored $rel"
    done < "$SNAP/manifest"
}

cmd_clean() { rm -rf "$SNAP"; echo "snapshot cleared"; }

# --- emit ---------------------------------------------------------------------------------
cmd_emit() {
    check_base; require_snapshot
    local name="${1:-}" msgfile=""
    [ -n "$name" ] || die "emit needs a patch name (without .mypatch)"
    shift
    if [ "${1:-}" = "-m" ]; then msgfile="${2:-}"; fi
    name="${name%.mypatch}"
    local out="$PATCHDIR/$name.mypatch"

    # The base must not have moved: if the rest of the patch set changed since the snapshot,
    # the tree we diffed is no longer what build.sh will produce.
    # NB: not named `then` -- that is a bash reserved word and silently breaks the script.
    local now_fp; now_fp="$(patchset_fingerprint "$name.mypatch")"
    local snap_fp; snap_fp="$(cat "$SNAP/fingerprint")"
    if [ "$now_fp" != "$snap_fp" ]; then
        # Recompute the snapshot-time value the same way (excluding this patch if it existed).
        echo "mkpatch: WARNING — patches/ changed since the snapshot was taken." >&2
        echo "  The base tree may no longer match. Re-run build.sh, re-snapshot, re-edit." >&2
        echo "  Continuing anyway would risk another wasted build." >&2
        [ "${MKPATCH_FORCE:-0}" = "1" ] || die "refusing to emit (set MKPATCH_FORCE=1 to override)"
    fi

    local tmp; tmp="$(mktemp)"
    if [ -n "$msgfile" ]; then
        [ -f "$msgfile" ] || die "message file not found: $msgfile"
        cat "$msgfile" >> "$tmp"
        echo >> "$tmp"
    fi

    local rel n=0 files=0 src
    while read -r rel; do
        src="$(snap_path "$rel")"
        if is_new "$rel"; then [ -f "$BASE/$rel" ] || continue
        else cmp -s "$src" "$BASE/$rel" && continue; fi
        # Never hand-edit diff output: let diff write the labels itself.
        diff -u --label "a/$rel" --label "b/$rel" \
             "$src" "$BASE/$rel" >> "$tmp" || true
        # NB: diff exits 1 when the files differ, which is the normal case here. With
        # `set -o pipefail` that status propagates out of the command substitution and, under
        # `set -e`, kills the script mid-loop with no message. Swallow it explicitly.
        local hunks
        hunks="$( { diff -u "$src" "$BASE/$rel" || true; } | command grep -c '^@@' )" || hunks=0
        n=$(( n + hunks ))
        files=$(( files + 1 ))
    done < "$SNAP/manifest"

    [ "$files" -gt 0 ] || { rm -f "$tmp"; die "no edits found in the snapshotted files — nothing to emit"; }

    # Verify by applying to pristine copies, exactly as build.sh's `patch -Np1` would.
    local vdir; vdir="$(mktemp -d)"
    while read -r rel; do
        mkdir -p "$vdir/$(dirname "$rel")"
        is_new "$rel" && continue      # created by the patch itself
        cp "$SNAP/files/$rel" "$vdir/$rel"
    done < "$SNAP/manifest"
    if ! patch -p1 -d "$vdir" --dry-run < "$tmp" > "$vdir/.dryrun" 2>&1; then
        echo "mkpatch: DRY RUN FAILED — patch would not apply:" >&2
        cat "$vdir/.dryrun" >&2
        rm -rf "$vdir"; rm -f "$tmp"
        die "patch rejected, not written"
    fi
    if command grep -qi 'fuzz\|offset' "$vdir/.dryrun"; then
        echo "mkpatch: WARNING — dry run reported fuzz/offset:" >&2
        cat "$vdir/.dryrun" >&2
    fi
    # And that applying it really reproduces the edited files byte-for-byte.
    patch -p1 -d "$vdir" --silent < "$tmp"
    while read -r rel; do
        if is_new "$rel" && [ ! -f "$BASE/$rel" ]; then continue; fi
        cmp -s "$vdir/$rel" "$BASE/$rel" || { rm -rf "$vdir"; rm -f "$tmp"; \
            die "applied patch does not reproduce $rel — refusing to write"; }
    done < "$SNAP/manifest"
    rm -rf "$vdir"

    mv "$tmp" "$out"
    chmod 644 "$out"
    echo "wrote $out"
    echo "  files: $files   hunks: $n"
    echo "  verified: applies clean to the base tree and reproduces the edits exactly"

    local ORDER_CHECK="$HOME/neutron/preserved-fixes/harnesses/check-patch-order.py"
    if [ -f "$ORDER_CHECK" ]; then
        echo
        python3 "$ORDER_CHECK" "$PATCHDIR" || die "patch ORDER check failed — rename before building"
    fi
}

case "${1:-}" in
    base)     shift; cmd_base "$@" ;;
    snapshot) shift; cmd_snapshot "$@" ;;
    status)   shift; cmd_status "$@" ;;
    emit)     shift; cmd_emit "$@" ;;
    reset)    shift; cmd_reset "$@" ;;
    clean)    shift; cmd_clean "$@" ;;
    *) command sed -n '2,40p' "$0"; exit 1 ;;
esac
