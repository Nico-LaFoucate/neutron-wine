# neutron-wine

The patched Wine that powers **[Neutron](https://github.com/Nico-LaFoucate/Neutron)** — a
compatibility layer for running professional creative software (the Adobe suite) on Linux, in the spirit
of Valve's Proton.

This repository is the **Neutron wine fork as a patch set + build recipe** (the Proton-GE model): it is
**not** a full copy of the Wine source tree. It pins an upstream Wine base and a set of Neutron patches
that, applied and built, produce the Wine that Neutron's engine runs.

The runtime built from this repo also carries the graphics and NVIDIA translation layers the apps run
on (DXVK, vkd3d-proton, the NVIDIA wrappers) and a ucrtbase shim. They are built from upstream source
at pinned commits, with our patches, from [`external/`](external/). The current version is
**11.10-99** ([`build/VERSION`](build/VERSION)).

## Upstream base

- **Wine:** `wine-11.10`, **staging**, with **ntsync** (TkG Staging-NTsync flavor).
- Built reference: `wine-11.10.r0.gf45e84d7`.
- Build framework: [`wine-tkg-git`](https://github.com/Frogging-Family/wine-tkg-git), pinned to commit
  `8359b3d5` ([`build/WINE_TKG_COMMIT`](build/WINE_TKG_COMMIT)); its config is in
  [`build/customization.cfg`](build/customization.cfg).

## Patches

Applied on top of the upstream base by [`build/build.sh`](build/build.sh).
[`patches/MANIFEST.md`](patches/MANIFEST.md) has notes on some of them.

| Path | What it holds | Built? |
| --- | --- | --- |
| `patches/*.mypatch` | The production patch set. | Yes |
| `patches/diagnostic/` | Diagnostic probes, most of them off unless their environment variable is set. | Only with `INCLUDE_DIAGNOSTIC=1` |
| `patches/unverified/` | Work kept for later that hasn't been reviewed and tested yet. | No |
| `patches/disproven/`, `patches/retired/` | Patches kept for the record. | No |
| `external/patches/` | Our DXVK, vkd3d-proton and nvcuda patches, and SveSop's dxvk-nvapi commits. See [`external/README.md`](external/README.md). | Yes, by `build/package.sh` |

The production set covers:

- the Wayland driver (`winewayland.drv`): fractional scaling, popups and menus, subsurfaces, drag
  and drop, focus and the work area;
- Direct2D (`d2d1`): geometry, layers and masks, effects, WIC render targets;
- windowing and GDI (`win32u`, `user32`, `gdi32`);
- DirectComposition (`dcomp`) and `dxcore`;
- DirectWrite (`dwrite`) and WIC;
- the file dialog and common controls (`comdlg32`, `comctl32`), and the window decorations (caption
  buttons, frame and menu bar colors);
- smaller fixes in ntdll, wineserver, I/O completion ports, mshtml, WinRT, winhttp, WinTab (pen
  input), the IME, OpenGL/Direct3D interop and more.

## Build

```sh
export NEUTRON_WINE_WORK=/var/tmp/neutron-wine   # a neutral path: Wine compiles its location in
build/build.sh            # fetch wine-tkg at its pinned commit, apply patches/, build
build/package.sh          # build external/ (DXVK, vkd3d-proton, NVIDIA wrappers), package -> dist/
```

On Arch-based systems `build/build.sh` checks its build dependencies first and says what to
install; elsewhere you need Wine's usual build dependencies plus wine-tkg-git's, `meson`, `ninja`,
`glslang`, `mingw-w64-gcc`, and `rsvg-convert` (librsvg), `magick` (ImageMagick), `icotool` (icoutils)
and `python3` for the generated toolbar and shell icons. A from-scratch build (downloads included) takes about **11.5
minutes** on a 24-thread desktop CPU (i9-12900KF); `package.sh` adds a few more for `external/` and
compression. The result is `dist/neutron-wine-<ver>.tar.xz` with its checksum, manifest and source
archive.

## Releases

Users don't build Wine: `neutron setup` downloads the release GitHub marks **Latest** and checks it
against its manifest. Each release carries four files: `neutron-wine-<ver>.tar.xz`, its `.sha256`,
`neutron-wine-<ver>.manifest.json`, and `neutron-wine-<ver>-source.tar.xz` (the complete source). The release version
is pinned in [`build/VERSION`](build/VERSION).

**Full packaging & release runbook:** [`RELEASING.md`](RELEASING.md) — how to cut a release, how the
engine consumes the manifest, and the patch-set/LGPL provenance notes. Notes on some of the patches are in
[`patches/MANIFEST.md`](patches/MANIFEST.md); most patch files also open with their own description.

## Support Neutron

Neutron was made for free, and every donation helps keep the project alive and in development.

- [Patreon](https://patreon.com/neutronproject): monthly support
- [Ko-fi](https://ko-fi.com/neutroncollider): one-time or monthly

## License

neutron-wine is licensed under the **GNU Lesser General Public License, version 2.1 or later**
(`LGPL-2.1-or-later`), the same as Wine. See [`LICENSE`](LICENSE) for the full text. Each patch file
carries the license of the project it modifies: Wine's for `patches/`, and each upstream project's for
`external/patches/` (listed in [`external/README.md`](external/README.md)). Neutron (the CLI) is a
separate LGPL-2.1-or-later repo; Collider (the GUI) and Mud Hut (the installer) are Apache-2.0.

## Status

Beta. Tested on three machines, all CachyOS with KDE Plasma (Wayland) and NVIDIA GPUs. AMD and Intel
GPUs, other distributions and other desktops aren't validated yet.

## Reporting bugs

Report bugs in the neutron-wine runtime on this repository's
[Issues](https://github.com/Nico-LaFoucate/neutron-wine/issues/new/choose). If an Adobe app misbehaves
while running, report it on
[Neutron's Issues](https://github.com/Nico-LaFoucate/Neutron/issues/new/choose) instead: that is
where launching and running the apps are handled. Questions go to
[Discussions](https://github.com/Nico-LaFoucate/Neutron/discussions). Report security problems
privately: see [`SECURITY.md`](SECURITY.md). To contribute, see [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Disclaimer

Neutron is an independent project by Nico LaFoucate and Ficus Media Group. Adobe and its product names
are trademarks of Adobe Inc. Neutron is not affiliated with or endorsed by Adobe.
