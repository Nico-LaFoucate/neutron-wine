#!/usr/bin/env bash
# check-build-deps.sh — pre-flight for build.sh on Arch-based machines (skips elsewhere).
#
# 2026-09-11: a routine `pacman -Rns $(pacman -Qdtq)` cleanup removed 72 orphans, and the whole
# toolchain that builds the runtime, Collider and Mud Hut went with them (rust, meson, ninja,
# fontforge, the ayatana libs): an AUR helper had installed them `--asdeps` months earlier. The
# durable fix is install reason: a package marked EXPLICIT is never an orphan. This reports both
# halves — what is already missing (fatal) and what the NEXT cleanup will take (warning).
# (Moved here from `neutron doctor`, which only checks what can affect running the apps.)
command -v pacman >/dev/null || exit 0
NEED=(
  # wine-tkg makedepends that are not part of base-devel
  meson ninja glslang fontforge mingw-w64-gcc
  # the Rust stack: Collider (Tauri) and Mud Hut
  rust rust-src rust-bindgen nodejs npm
  # Tauri's tray/indicator chain (Collider's BUILD.md)
  libayatana-appindicator libayatana-indicator ayatana-ido webkit2gtk-4.1 librsvg openssl
  # 32-bit audio for the i386 half of the runtime (only packages still in the repos)
  lib32-libpulse
)
INST="$(pacman -Qq 2>/dev/null)" || exit 0
ORPH="$(pacman -Qdtq 2>/dev/null || true)"
missing=(); at_risk=()
for d in "${NEED[@]}"; do
  if ! grep -qx "$d" <<<"$INST"; then missing+=("$d")
  elif grep -qx "$d" <<<"$ORPH"; then at_risk+=("$d"); fi
done
if [ ${#at_risk[@]} -gt 0 ]; then
  echo "build deps: ⚠️ orphan-flagged, the next cleanup will REMOVE: ${at_risk[*]}" >&2
  echo "            pin with: sudo pacman -D --asexplicit ${at_risk[*]}" >&2
fi
if [ ${#missing[@]} -gt 0 ]; then
  echo "build deps: MISSING: ${missing[*]}" >&2
  echo "            install with: sudo pacman -S --needed --asexplicit ${missing[*]}" >&2
  exit 1
fi
echo "build deps: ${#NEED[@]} present"
