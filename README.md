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
build/build.sh            # fetch wine-tkg, drop in patches/, apply the pinned config, build
```
The result is a complete Wine tree; point Neutron at it with `NEUTRON_WINE=<…>/wine`.
See the script for prerequisites and detail. (Requires the usual Wine build deps + `wine-tkg-git`'s.)

## Releases

Users don't build Wine — they download a prebuilt, versioned runtime. `build/package.sh` wraps a
built tree into a release artifact (`neutron-wine-<ver>.tar.xz` + `.sha256` + `manifest.json`) that
attaches to a GitHub Release; Neutron's engine resolves and verifies it from the manifest. The
release version is pinned in [`build/VERSION`](build/VERSION). Full runbook: [`RELEASING.md`](RELEASING.md).

## License

LGPL-2.1 (Wine's license). See [LICENSE](LICENSE). The patches are derivative works of Wine and are
licensed accordingly. Neutron (the engine/CLI) is a separate LGPL-2.1 repo; Collider (the GUI) is Apache-2.0.

## Status

Experimental alpha. Target: Adobe Premiere Pro 2025 on Wayland (KDE) / X11, NVIDIA. Vendor-neutral by
design; AMD/Intel expected to need fewer workarounds.
