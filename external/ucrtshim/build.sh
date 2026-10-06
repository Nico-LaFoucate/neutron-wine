#!/usr/bin/env bash
# Build the Neutron ucrtbase FH4 + "\\?\"-strip shim.
#
#   build.sh [--memprobe] <ucrtbase-*.exports | ucrtbase_orig.dll> <out ucrtbase.dll>
#
# The input is either the committed export-NAME list (ucrtbase-10.0.10586.15.exports, one name
# per line; what external/build-external.sh uses) or Microsoft's ucrtbase.dll itself, whose
# export names are read with objdump. Either way the shim forwards to ucrtbase_orig.dll, which
# `neutron setup` installs from Microsoft's own download (pinned in ucrtbase_orig.sha256).
# See stub.c for what the two fixes are and why. Requires the mingw-w64 toolchain
# (x86_64-w64-mingw32-gcc + objdump). 64-bit only: the FH4 proxy and the MainConcept muxer are
# 64-bit; the 32-bit ucrtbase is Microsoft's real one, installed untouched.
set -euo pipefail

# --memprobe builds the DIAGNOSTIC variant: memmove/memcpy become local wrappers that
# record their callers (see stub.c). The production shim must stay byte-for-byte what it
# was, so this is opt-in and everything it changes is asserted at the end.
MEMPROBE=0
if [ "${1:-}" = "--memprobe" ]; then MEMPROBE=1; shift; fi

ORIG="${1:?usage: build.sh [--memprobe] <ucrtbase-*.exports | ucrtbase_orig.dll> <out ucrtbase.dll>}"
OUT="${2:?usage: build.sh [--memprobe] <ucrtbase-*.exports | ucrtbase_orig.dll> <out ucrtbase.dll>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CC=x86_64-w64-mingw32-gcc
OBJDUMP=x86_64-w64-mingw32-objdump

command -v "$CC"      >/dev/null 2>&1 || { echo "build-ucrtshim: $CC not found (install mingw-w64)" >&2; exit 2; }
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "build-ucrtshim: $OBJDUMP not found (install mingw-w64)" >&2; exit 2; }
[ -f "$ORIG" ] || { echo "build-ucrtshim: input not found: $ORIG" >&2; exit 2; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
DEF="$TMP/ucrt.def"

# .def: add __CxxFrameHandler4 (from vcruntime140_1) + our local _wstat64, then forward
# every OTHER real export back to ucrtbase_orig.
# Names we implement LOCALLY must be excluded from the forward-everything pass, or the
# forwarder would win and the local implementation would never be called.
LOCAL="__CxxFrameHandler4 _wstat64"
[ "$MEMPROBE" = 1 ] && LOCAL="$LOCAL memmove memcpy"

{
    echo 'LIBRARY "ucrtbase.dll"'
    echo 'EXPORTS'
    echo '    __CxxFrameHandler4 = vcruntime140_1.__CxxFrameHandler4'
    echo '    _wstat64'
    [ "$MEMPROBE" = 1 ] && { echo '    memmove'; echo '    memcpy'; }
    # Export names, one per line. From a DLL: only the name-table rows of `objdump -p`
    # ("[i] +base[n]  hint name"); the export-address rows end in "Export RVA" and must not
    # become a bogus "RVA" export.
    case "$ORIG" in
        *.exports) cat "$ORIG" ;;
        *) "$OBJDUMP" -p "$ORIG" | awk '/\+base\[/ && $(NF-1) ~ /^[0-9a-f][0-9a-f][0-9a-f][0-9a-f]$/ { print $NF }' ;;
    esac | awk -v local="$LOCAL" '
        BEGIN { split(local, a, " "); for (i in a) skip[a[i]] = 1 }
        {
            n = $1
            if (n ~ /^[A-Za-z_?@]/ && !(n in skip))
                printf "    %s = ucrtbase_orig.%s\n", n, n
        }'
} > "$DEF"

CFLAGS=""
[ "$MEMPROBE" = 1 ] && CFLAGS="-DNEUTRON_MEMPROBE"
# -ffreestanding: without it GCC may turn our own byte loop back into a call to memmove.
"$CC" -shared -s -nostdlib -ffreestanding -O2 $CFLAGS -o "$OUT" "$HERE/stub.c" "$DEF" -Wl,-e,DllMain -lkernel32

# Sanity: FH4 must be a forwarder to vcruntime140_1, and _wstat64 must be LOCAL (our
# override), not a forwarder back to ucrtbase_orig. (Dump to a file first — piping into
# `grep -q` closes the pipe early and, under `set -o pipefail`, misreports SIGPIPE as a
# build failure.)
DUMP="$TMP/out.exports"
"$OBJDUMP" -p "$OUT" > "$DUMP"
grep -q "Forwarder RVA -- vcruntime140_1.__CxxFrameHandler4" "$DUMP" \
    || { echo "build-ucrtshim: __CxxFrameHandler4 forwarder missing in output" >&2; exit 3; }
if grep -qE "Forwarder RVA -- ucrtbase_orig\._wstat64$" "$DUMP"; then
    echo "build-ucrtshim: _wstat64 is still forwarded (override not applied)" >&2; exit 3
fi

# The diagnostic variant must actually intercept; the production one must NOT contain it.
if [ "$MEMPROBE" = 1 ]; then
    for sym in memmove memcpy; do
        grep -qE "Forwarder RVA -- ucrtbase_orig\.$sym\$" "$DUMP" \
            && { echo "build-ucrtshim: $sym still forwarded -- probe would never run" >&2; exit 3; }
    done
    echo "build-ucrtshim: MEMPROBE build -- memmove/memcpy are local wrappers ⚠️ diagnostic only"
else
    for sym in memmove memcpy; do
        grep -qE "Forwarder RVA -- ucrtbase_orig\.$sym\$" "$DUMP" \
            || { echo "build-ucrtshim: $sym is NOT forwarded in a production build" >&2; exit 3; }
    done
fi

echo "build-ucrtshim: built $OUT ($(stat -c%s "$OUT") bytes) from $(basename "$ORIG")"
