#!/usr/bin/env bash
# build-external.sh — build the non-Wine DLLs that ship inside the neutron-wine runtime and
# install them into a built Wine tree.
#
#   external/build-external.sh <wine-tree>
#
# Sources: sources.conf (upstream URL + pinned commit per project) + patches/<project>/*.patch,
# plus our ucrtbase shim in ucrtshim/. Installs into <wine-tree>:
#   lib/wine/dxvk/{x86_64,i386}-windows/{d3d8,d3d9,d3d10core,d3d11,dxgi}.dll
#   lib/wine/vkd3d-proton/{x86_64,i386}-windows/{d3d12,d3d12core}.dll
#   lib/wine/nvapi/x86_64-windows/{nvapi64,nvofapi64}.dll   lib/wine/nvapi/i386-windows/nvapi.dll
#   lib/wine/nvidia-libs/x86_64-unix/{nvcuda,nvcuvid,nvencodeapi64,nvoptix}.dll   (ELF winelib)
#   lib/wine/neutron/x86_64-windows/ucrtbase.dll
#   share/neutron/natives.json   what `neutron prefix provision` stages: file, prefix dir,
#                                DLL override, project, commit, sha256
#   licenses/<project>/          each project's license files, submodules included
# Wine never searches subdirectories of lib/wine for builtins, so none of these shadow Wine's
# own d3d11/dxgi/d3d12; provisioning copies them into each prefix's system32/syswow64.
#
# Env:
#   NEUTRON_EXT_WORK=<dir>   work dir (default /var/tmp/neutron-external). Never under $HOME:
#                            build paths end up inside the DLLs.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
TREE="$(cd "${1:?usage: build-external.sh <wine-tree>}" && pwd)"
WORK="${NEUTRON_EXT_WORK:-/var/tmp/neutron-external}"
die() { echo "build-external: $*" >&2; exit 1; }

case "$WORK/" in "$HOME/"*) die "NEUTRON_EXT_WORK ($WORK) is under \$HOME; build paths end up in the DLLs" ;; esac
[ -x "$TREE/bin/winegcc" ] && [ -x "$TREE/bin/widl" ] \
    || die "$TREE has no bin/winegcc + bin/widl (not a built Wine tree?)"
for t in git meson ninja glslangValidator python3 x86_64-w64-mingw32-gcc i686-w64-mingw32-gcc \
         x86_64-w64-mingw32-objdump x86_64-w64-mingw32-strip i686-w64-mingw32-strip strip; do
    command -v "$t" >/dev/null 2>&1 || die "missing build tool: $t"
done
# winegcc, winebuild, widl and the Wine headers come from OUR Wine, never the distro's.
export PATH="$TREE/bin:$PATH"

# --- sources ------------------------------------------------------------------
declare -A URL COMMIT PATCHES
NAMES=()
while read -r name url commit patches; do
    case "$name" in ''|'#'*) continue ;; esac
    URL[$name]="$url"; COMMIT[$name]="$commit"; PATCHES[$name]="$patches"; NAMES+=("$name")
done < "$HERE/sources.conf"

# Rebuild only when something that feeds the build changed.
KEY="$( { cat "$HERE/sources.conf" "$REPO/build/WINE_BASE"
          find "$HERE/patches" "$HERE/ucrtshim" -type f -print0 | sort -z | xargs -0 sha256sum
          sha256sum "$0"; } | sha256sum | cut -c1-16 )"
OUT="$WORK/out-$KEY"
B="$WORK/build"

fetch() {  # fetch <name>: $WORK/src/<name>, clean, at the pinned commit, submodules at their pins
    local n="$1" d="$WORK/src/$1" c="${COMMIT[$1]}"
    if [ ! -d "$d/.git" ]; then
        mkdir -p "$WORK/src"
        git clone -q --filter=blob:none --no-checkout "${URL[$n]}" "$d"
    fi
    git -C "$d" cat-file -e "$c^{commit}" 2>/dev/null || git -C "$d" fetch -q --tags origin
    git -C "$d" checkout -q -f --detach "$c"
    git -C "$d" clean -q -ffdx
    git -C "$d" submodule sync -q --recursive
    git -C "$d" submodule update -q --init --recursive --force
    git -C "$d" submodule foreach -q --recursive 'git clean -q -ffdx'
    [ "$(git -C "$d" rev-parse HEAD)" = "$c" ] || die "$n: HEAD is not the pinned $c"
    if git -C "$d" submodule status --recursive | grep -q '^[-+U]'; then
        die "$n: submodules do not match the commits $c pins"
    fi
    local p
    [ "${PATCHES[$n]}" = "-" ] && return 0
    for p in "$HERE/patches/${PATCHES[$n]}"/*.patch; do
        # git apply, not git am: the tree stays "dirty", so DXVK's version reads "...+" and
        # says plainly that this is a modified build.
        git -C "$d" apply --check "$p" || die "$n: $(basename "$p") does not apply to $c"
        git -C "$d" apply "$p"
        echo "  $n: applied $(basename "$p")"
    done
}

build() {  # build <name> <tag> <cross-file> [meson args...]
    local src="$WORK/src/$1" tag="$2" bd="$B/$2" cross="$3"; shift 3
    echo "  building $tag"
    meson setup "$bd" "$src" --cross-file "$src/$cross" --buildtype release --strip "$@" \
        > "$bd.setup.log" 2>&1 || { tail -30 "$bd.setup.log" >&2; die "$tag: meson setup failed (log: $bd.setup.log)"; }
    ninja -C "$bd" > "$bd.build.log" 2>&1 || { tail -30 "$bd.build.log" >&2; die "$tag: build failed (log: $bd.build.log)"; }
}

take() {  # take <tag> <file> <dest dir>: copy the one <file> the build produced, stripped
    local hits out
    hits="$(find "$B/$1" -name "$2" -type f -not -path '*/subprojects/*' -not -path '*/tests/*')"
    [ "$(printf '%s\n' "$hits" | grep -c .)" = 1 ] || die "$1: expected exactly one $2, found: ${hits:-none}"
    mkdir -p "$3"; out="$3/${2%.so}"; cp "$hits" "$out"
    # meson's --strip only applies to `meson install`; we take files from the build dir.
    case "$3" in
        *x86_64-windows) x86_64-w64-mingw32-strip "$out" ;;
        *i386-windows)   i686-w64-mingw32-strip "$out" ;;
        *-unix)          strip "$out" ;;
        *) die "take: no strip rule for $3" ;;
    esac
}

if [ -f "$OUT/.complete" ]; then
    echo "build-external: sources unchanged, reusing $OUT"
else
    echo "build-external: building into $OUT"
    rm -rf "$B" "$OUT"; mkdir -p "$B" "$OUT"
    for n in "${NAMES[@]}"; do fetch "$n"; done

    build dxvk          dxvk64        build-win64.txt
    build dxvk-stock    dxvk-stock64  build-win64.txt
    build dxvk-stock    dxvk-stock32  build-win32.txt
    build vkd3d-proton  vkd3d64       build-win64.txt
    build vkd3d-x86     vkd3d32       build-win32.txt
    build nvcuda        nvcuda        build-wine64.txt
    build nvenc         nvenc         build-wine64.txt
    build wine-nvoptix  nvoptix       build-wine64.txt
    build dxvk-nvapi    nvapi64       build-win64.txt -Denable_tests=false
    build dxvk-nvapi    nvapi32       build-win32.txt -Denable_tests=false

    L="$OUT/lib/wine"
    for f in d3d11 dxgi;             do take dxvk64       $f.dll "$L/dxvk/x86_64-windows"; done
    for f in d3d8 d3d9 d3d10core;    do take dxvk-stock64 $f.dll "$L/dxvk/x86_64-windows"; done
    for f in d3d8 d3d9 d3d10core d3d11 dxgi; do take dxvk-stock32 $f.dll "$L/dxvk/i386-windows"; done
    for f in d3d12 d3d12core;        do take vkd3d64      $f.dll "$L/vkd3d-proton/x86_64-windows"; done
    for f in d3d12 d3d12core;        do take vkd3d32      $f.dll "$L/vkd3d-proton/i386-windows"; done
    for f in nvapi64 nvofapi64;      do take nvapi64      $f.dll "$L/nvapi/x86_64-windows"; done
    take nvapi32 nvapi.dll "$L/nvapi/i386-windows"
    take nvcuda  nvcuda.dll.so        "$L/nvidia-libs/x86_64-unix"
    take nvenc   nvcuvid.dll.so       "$L/nvidia-libs/x86_64-unix"
    take nvenc   nvencodeapi64.dll.so "$L/nvidia-libs/x86_64-unix"
    take nvoptix nvoptix.dll.so       "$L/nvidia-libs/x86_64-unix"
    mkdir -p "$L/neutron/x86_64-windows"
    bash "$HERE/ucrtshim/build.sh" "$HERE/ucrtshim/ucrtbase-10.0.10586.15.exports" \
        "$L/neutron/x86_64-windows/ucrtbase.dll"

    # Licenses: every LICENSE/COPYING/NOTICE file in each source tree, submodules included.
    declare -A PROJ=( [dxvk]=dxvk [dxvk-stock]=dxvk [vkd3d-proton]=vkd3d-proton [vkd3d-x86]=vkd3d-proton
                      [nvcuda]=nvcuda [nvenc]=nvenc [wine-nvoptix]=wine-nvoptix [dxvk-nvapi]=dxvk-nvapi )
    for n in "${NAMES[@]}"; do
        ( cd "$WORK/src/$n" && find . -maxdepth 5 -type f \
              \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' \) \
              -not -path './.git/*' -not -path '*/test*' -print0 ) \
        | while IFS= read -r -d '' f; do
              mkdir -p "$OUT/licenses/${PROJ[$n]}/$(dirname "$f")"
              cp -n "$WORK/src/$n/$f" "$OUT/licenses/${PROJ[$n]}/$f"
          done
    done
    mkdir -p "$OUT/licenses/ucrtshim"
    printf 'The ucrtbase shim is part of neutron-wine (external/ucrtshim): LGPL-2.1-or-later, see\nlicenses/wine/COPYING.LIB.\n' \
        > "$OUT/licenses/ucrtshim/README"

    # --- gates + natives.json (fail the build, never warn) ----------------------
    SRCJSON="$(for n in "${NAMES[@]}"; do
        ps=""; [ "${PATCHES[$n]}" != "-" ] && ps="$(cat "$HERE/patches/${PATCHES[$n]}"/*.patch | sha256sum | cut -c1-64)"
        printf '%s\t%s\t%s\t%s\n' "$n" "${URL[$n]}" "${COMMIT[$n]}" "$ps"; done)"
    SRCJSON="$SRCJSON" python3 - "$OUT" "$HOME" "$(cat "$HERE/ucrtshim/ucrtbase_orig.sha256")" <<'PY'
import hashlib, json, os, re, sys
out, home, orig_sha = sys.argv[1], sys.argv[2], sys.argv[3].strip()
src = {}
for line in os.environ["SRCJSON"].splitlines():
    n, url, commit, psha = line.split("\t")
    src[n] = {"url": url, "commit": commit, **({"patch_sha256": psha} if psha else {})}
L = os.path.join(out, "lib", "wine")
errors = []

def data(rel):
    with open(os.path.join(L, rel), "rb") as f:
        return f.read()

# (file under lib/wine, source, override key, override value)
rows = []
for a, d in (("x86_64-windows", "system32"), ("i386-windows", "syswow64")):
    for dll in ("d3d8", "d3d9", "d3d10core", "d3d11", "dxgi"):
        s = "dxvk" if (a == "x86_64-windows" and dll in ("d3d11", "dxgi")) else "dxvk-stock"
        rows.append((f"dxvk/{a}/{dll}.dll", s, dll, "native", d))
    for dll in ("d3d12", "d3d12core"):
        rows.append((f"vkd3d-proton/{a}/{dll}.dll", "vkd3d-proton" if a == "x86_64-windows" else "vkd3d-x86", dll, "native", d))
for dll in ("nvapi64", "nvofapi64"):
    rows.append((f"nvapi/x86_64-windows/{dll}.dll", "dxvk-nvapi", dll, "native", "system32"))
rows.append(("nvapi/i386-windows/nvapi.dll", "dxvk-nvapi", "nvapi", "native", "syswow64"))
for dll, s in (("nvcuda", "nvcuda"), ("nvcuvid", "nvenc"), ("nvencodeapi64", "nvenc"), ("nvoptix", "wine-nvoptix")):
    rows.append((f"nvidia-libs/x86_64-unix/{dll}.dll", s, dll, "native", "system32"))
rows.append(("neutron/x86_64-windows/ucrtbase.dll", "ucrtshim", "*ucrtbase", "native,builtin", "system32"))

# Our changes must be in the patched DLLs, and nowhere else.
must = {
    "dxvk/x86_64-windows/d3d11.dll": [b"NEUTRON_GDI_PRESENT", b"NEUTRON_DXVK_EXPORT_TEXTURES"],
    "dxvk/x86_64-windows/dxgi.dll": [b"NEUTRON_SC_TRACE"],
    "vkd3d-proton/x86_64-windows/d3d12core.dll": [b"NEUTRON_DISABLE_BTW", b"NEUTRON_VKD3D_RT_PROBE"],
    "nvidia-libs/x86_64-unix/nvcuda.dll": [b"NEUTRON_CUDA_PLAYBACK_EXPERIMENT"],
}
never = [b"neutron-rtlog"]
home_needles = [home.encode() + b"/", (home + "/").encode("utf-16-le"),
                home.replace("/", "\\").encode(), home.replace("/", "\\").encode("utf-16-le")]
# DXVK's version is `git describe --dirty=+`; the hash abbreviation length varies by clone.
dxvk_ver = {"dxvk": rb"v2\.7\.1-634-g3c6f508[0-9a-f]*\+", "dxvk-stock": rb"v2\.7\.1-609-g02bcc98[0-9a-f]*"}
files = []
for rel, s, key, val, dest in rows:
    p = os.path.join(L, rel)
    if not os.path.isfile(p):
        errors.append(f"missing {rel}"); continue
    b = data(rel)
    for m in must.get(rel, []):
        if m not in b: errors.append(f"{rel}: marker {m.decode()} missing")
    if rel not in must and s not in ("ucrtshim",) and b"NEUTRON_" in b:
        errors.append(f"{rel}: built from unmodified {s} but contains NEUTRON_ strings")
    for m in never:
        if m in b: errors.append(f"{rel}: contains {m.decode()}")
    for m in home_needles:
        if m in b: errors.append(f"{rel}: contains the builder's home path")
    if "-unix/" in rel:
        if b[:4] != b"\x7fELF" or b[4] != 2 or b[18] != 0x3e:
            errors.append(f"{rel}: not an ELF x86-64 winelib")
    else:
        pe = int.from_bytes(b[0x3c:0x40], "little")
        mach = int.from_bytes(b[pe + 4:pe + 6], "little") if b[:2] == b"MZ" else None
        want = 0x8664 if "x86_64-windows" in rel else 0x14c
        if mach != want: errors.append(f"{rel}: PE machine {mach!r}, expected {hex(want)}")
    if rel.startswith("dxvk/") and key in ("d3d9", "d3d11", "dxgi") and not re.search(dxvk_ver[s], b):
        errors.append(f"{rel}: version string is not {dxvk_ver[s].decode()}")
    row = {"dll": key.lstrip("*"), "file": "lib/wine/" + rel, "prefix_dir": dest,
           "override": {key: val}, "source": s, "sha256": hashlib.sha256(b).hexdigest()}
    if s in src:
        row["commit"] = src[s]["commit"]
        row["patched"] = "patch_sha256" in src[s]
    if key == "*ucrtbase":
        row["requires"] = {"file": "ucrtbase_orig.dll", "sha256": orig_sha}
    files.append(row)

if errors:
    print("build-external: ⛔ gate failures:", file=sys.stderr)
    for e in errors: print("   " + e, file=sys.stderr)
    sys.exit(1)
os.makedirs(os.path.join(out, "share", "neutron"), exist_ok=True)
with open(os.path.join(out, "share", "neutron", "natives.json"), "w") as f:
    json.dump({"format": 1, "sources": src, "files": files}, f, indent=2)
    f.write("\n")
print(f"build-external: gates passed, {len(files)} DLLs")
PY
    touch "$OUT/.complete"
fi

# --- install into the Wine tree -----------------------------------------------
rm -rf "$TREE"/lib/wine/{dxvk,vkd3d-proton,nvapi,nvidia-libs,neutron} "$TREE/share/neutron"
mkdir -p "$TREE/lib/wine" "$TREE/share" "$TREE/licenses"
cp -a "$OUT/lib/wine/." "$TREE/lib/wine/"
cp -a "$OUT/share/neutron" "$TREE/share/neutron"
for d in "$OUT"/licenses/*; do rm -rf "$TREE/licenses/$(basename "$d")"; cp -a "$d" "$TREE/licenses/"; done
echo "build-external: installed into $TREE"
