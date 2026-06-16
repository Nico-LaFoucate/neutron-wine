# Patch manifest

Patches are applied on top of the upstream Wine base (see `../build/WINE_BASE`) by `../build/build.sh`,
which drops them into `wine-tkg-git/wine-tkg-userpatches/` (wine-tkg applies every `*.mypatch` there).

Order is not significant — each patch touches distinct files/regions.

## Production (`patches/`)

### `neutron-winewayland-fractional-scale.mypatch`
Adds `wp_fractional_scale_v1` support to `winewayland.drv` so the UI buffer maps **1:1** to a
fractional-scaled output (e.g. 4K @ 1.7×) instead of being compositor-upscaled (blurry). Binds the
manager, listens for `preferred_scale`, and uses that exact scale for `conf->scale`. Pairs with the
engine setting `LogPixels = round(96 × scale)` so System-DPI-aware Adobe apps render at device resolution.
Touches: `Makefile.in`, `fractional-scale-v1.xml`, `wayland.c`, `wayland_surface.c`, `waylanddrv.h`,
`window.c`.

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
