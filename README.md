# neutron-wine

The patched Wine that powers **[Neutron](https://github.com/Nico-LaFoucate/Neutron)** — a
compatibility layer for running professional creative software (the Adobe suite) on Linux, in the spirit
of Valve's Proton.

This repository is the **Neutron wine fork as a patch set + build recipe** (the Proton-GE model): it is
**not** a full copy of the Wine source tree. It pins an upstream Wine base and a set of Neutron patches
that, applied and built, produce the Wine that Neutron's engine runs.

> Phase note: Neutron owns its wine (Proton-style). The Neutron CLI's launch path uses this build via
> `$NEUTRON_WINE` / a packaged dist path; today that resolves to a local build of this repo, later to a
> versioned, bundled runtime artifact.

## Upstream base

- **Wine:** `wine-11.10`, **staging**, with **ntsync** (TkG Staging-NTsync flavor).
- Built reference: `wine-11.10.r0.gf45e84d7`.
- Build framework: [`wine-tkg-git`](https://github.com/Frogging-Family/wine-tkg-git) — the config is
  pinned in [`build/customization.cfg`](build/customization.cfg).

## Patches

Applied on top of the upstream base. See [`patches/MANIFEST.md`](patches/MANIFEST.md) for a one-line
description of each. Production patches live in `patches/`; env-gated diagnostics live in
`patches/diagnostic/`.

| Patch | Purpose |
| --- | --- |
| `neutron-winewayland-fractional-scale` | Crisp HiDPI UI under `winewayland.drv` via `wp_fractional_scale_v1` (1:1 buffer→output). |
| `neutron-winewayland-xdg-popup` | Menu-bar dropdowns as `xdg_popup` (not `wl_subsurface`) so KWin stops clipping their bottom. |
| `neutron-present-pacer` | X11 software frame pacer in `winex11.drv` (kills program-monitor playback judder). |
| `neutron-dcomp-bridge` | DirectComposition device impl + DXVK/vkd3d swapchain→window present bridge. |
| `neutron-jsonobject-homescreen` | UXP home-screen locale/i18n fix. |
| `windows-web-jsonobjectstatics` | Supporting JSONObject statics fix. |
| `diagnostic/neutron-present-timing` | Present-cadence timing probe (`NEUTRON_PRESENT_DEBUG`). |
| `diagnostic/neutron-uxp-present-probe` | UXP surface present probe. |

## Build

```sh
export NEUTRON_WINE_WORK=/var/tmp/neutron-wine   # a neutral path: Wine compiles its location in
build/build.sh            # fetch wine-tkg at its pinned commit, apply patches/, build
build/package.sh          # build external/ (DXVK, vkd3d-proton, NVIDIA wrappers), package -> dist/
```

On Arch-based systems `build/build.sh` checks its build dependencies first and says what to
install; elsewhere you need Wine's usual build dependencies plus wine-tkg-git's, `meson`, `ninja`,
`glslang` and `mingw-w64-gcc`. A from-scratch build (downloads included) takes about **11.5
minutes** on a 24-thread desktop CPU (i9-12900KF); `package.sh` adds a few more for `external/` and
compression. The result is `dist/neutron-wine-<ver>.tar.xz` with its checksum, manifest and source
archive.

## Releases

Users don't build Wine: `neutron setup` downloads the release GitHub marks **Latest** and checks it
against its manifest. Each release carries four files: `neutron-wine-<ver>.tar.xz`, its `.sha256`,
`manifest.json`, and `neutron-wine-<ver>-source.tar.xz` (the complete source). The release version
is pinned in [`build/VERSION`](build/VERSION).

**Full packaging & release runbook:** [`RELEASING.md`](RELEASING.md) — how to cut a release, how the
engine consumes the manifest, and the patch-set/LGPL provenance notes. Per-patch descriptions live in
[`patches/MANIFEST.md`](patches/MANIFEST.md).

## License

LGPL-2.1-or-later, the same as Wine. See [LICENSE](LICENSE). The patches are derivative works of
Wine and are licensed accordingly. Neutron (the CLI) is a separate LGPL-2.1-or-later repo; Collider (the
GUI) and Mud Hut (the installer) are Apache-2.0.

## Status

Beta. Tested on three machines, all CachyOS with KDE Plasma (Wayland) and NVIDIA GPUs. AMD and Intel
GPUs aren't validated yet.

## Disclaimer

Neutron is an independent project by Nico LaFoucate and Ficus Media Group. Adobe and its product names
are trademarks of Adobe Inc. Neutron is not affiliated with or endorsed by Adobe.
