# Patch manifest

Patches are applied on top of the upstream Wine base (see `../build/WINE_BASE`) by `../build/build.sh`,
which drops them into `wine-tkg-git/wine-tkg-userpatches/` (wine-tkg applies every `*.mypatch` there).

Order is mostly insignificant — patches touch distinct files/regions — with one exception:
`neutron-winewayland-xdg-popup` applies on top of `neutron-winewayland-fractional-scale` (same
`winewayland.drv` files). wine-tkg applies `*.mypatch` alphabetically, which already orders
`fractional-scale` before `xdg-popup`, so no manual ordering is needed.

## Production (`patches/`)

### `neutron-winewayland-fractional-scale.mypatch`
Adds `wp_fractional_scale_v1` support to `winewayland.drv` so the UI buffer maps **1:1** to a
fractional-scaled output (e.g. 4K @ 1.7×) instead of being compositor-upscaled (blurry). Binds the
manager, listens for `preferred_scale`, and uses that exact scale for `conf->scale`. Pairs with the
engine setting `LogPixels = round(96 × scale)` so System-DPI-aware Adobe apps render at device resolution.
Touches: `Makefile.in`, `fractional-scale-v1.xml`, `wayland.c`, `wayland_surface.c`, `waylanddrv.h`,
`window.c`.

### `neutron-winewayland-xdg-popup.mypatch`
Renders Premiere's menu-bar dropdowns as **`xdg_popup`** surfaces instead of `wl_subsurface`s. KWin
clips the bottom of a desync subsurface popup even when it is fully painted, correctly sized and on
screen (proven: byte-identical menu shows fully under X11; clip persists at scale 1.0); `xdg_popup` lets
the compositor position, constrain and display it. Only lightweight SHM menus take the popup path —
GPU/client-surface windows (the splash) and windows already realized as subsurfaces keep the classic
subsurface-of-owner path, avoiding role churn / ghost frames. Bumps the `xdg_wm_base` bind from v2 to
**v3** (needed for `xdg_positioner.set_offset` + `xdg_popup.reposition`); must not exceed v3 or KWin emits
`xdg_toplevel` v4/v5 events the 2-entry listener can't dispatch (aborts libwayland). Also reparents a
client subsurface when its toplevel's `wl_surface` is recreated by a role change (fixes a torn splash).
Backport/adaptation of Proton-EM / proton-cachyos 11.0-20260601 (@Etaash-mathamsetty). Applies on top of
`neutron-winewayland-fractional-scale`. Touches: `wayland.c`, `wayland_surface.c`, `waylanddrv.h`,
`window.c`.

### `neutron-winewayland-decoration.mypatch`
Optional **server-side** (`xdg-decoration`) window decorations, **off by default**. Neutron uses
**client-side** decorations — Wine draws its own NC, themed dark via the prefix's Control Panel colors
(`neutron-decoration/neutron-premiere-dark.reg`) — so the title bar is part of the window surface:
self-contained in the prefix, never overflows onto a second monitor, independent of the compositor's
global decoration theme (KWin has no per-app decoration). `WAYLANDDRV_SSD=1` opts into compositor-drawn
frames. When enabled, implements `zxdg_decoration_manager_v1` + the `pGetWindowStyleMasks` hook (v2, or
v1 gated to KDE). Premiere fights compositor resize/maximize and overflows on multi-monitor under SSD,
hence CSD is the default. Applies on top of `neutron-winewayland-xdg-popup`. Touches: `Makefile.in`,
`xdg-decoration-unstable-v1.xml`, `wayland.c`, `waylanddrv.h`, `waylanddrv_main.c`, `window.c`,
`wayland_surface.c`.

### `neutron-present-pacer.mypatch`
X11 software frame pacer in `winex11.drv : X11DRV_client_surface_present`. Sleeps to the next refresh
tick (real per-monitor rate via xcb-randr) before the `StretchBlt`, gated to active program-monitor
playback, VRR-aware. Removes beat-frequency judder that the un-vsync'd X11 present causes. Enabled with
`NEUTRON_PRESENT_PACE=1`; quiet unless `NEUTRON_PRESENT_DEBUG=1`. (True vblank phase-lock is impossible for
an X11 client under a compositor — this is the universal X11 floor; Wayland gets native pacing for free.)
Touches: `winex11.drv/{neutron_pacer.c,init.c}`, `Makefile.in` (`-lxcb -lxcb-randr`).

### `neutron-dcomp-bridge.mypatch`
Working `IDCompositionDevice` implementation (replaces Wine's stub `dlls/dcomp/device.c`) + a present
bridge that DXVK/vkd3d hand their windowed swapchains to (`dcomp_register_external` /
`dcomp_set_pending_hwnd`), blitted to the window. Load-bearing for surfaces that bypass DirectComposition.

### `neutron-jsonobject-homescreen.mypatch`
UXP home-screen locale/i18n fix (storage-based loader) so the Premiere home screen loads.

### `windows-web-jsonobjectstatics.mypatch`
Supporting JSONObject statics fix (pairs with the home-screen fix).

## Diagnostic (`patches/diagnostic/`) — env-gated, safe in shipping builds

### `neutron-present-timing.mypatch`
Present-cadence + blit-duration timing probe behind `NEUTRON_PRESENT_DEBUG`. Used to measure judder /
A-B pacing strategies.

### `neutron-uxp-present-probe.mypatch`
Reads UXP surface pixels before the present blit (diagnostic for the home-screen render investigation).
