# Patch manifest

Patches are applied on top of the upstream Wine base (see `../build/WINE_BASE`) by `../build/build.sh`,
which drops them into `wine-tkg-git/wine-tkg-userpatches/` (wine-tkg applies every `*.mypatch` there).

Order is insignificant — patches touch distinct files/regions, and each is generated against the
**staging-applied base** (not plain upstream), so every patch applies cleanly on the wine-tkg staging
tree in any order (verified: the full set reproduces the working source byte-for-byte). wine-tkg applies
`*.mypatch` alphabetically. The build requires `_user_patches_no_confirm="true"` in `customization.cfg`
(else userpatches are silently skipped on a non-interactive build).

## Production (`patches/`)

### `neutron-winewayland.mypatch`
Combined `winewayland.drv` patch — fractional-scale + xdg_popup menus + client-side decorations. These
three were originally separate patches but overlap heavily in the same `winewayland.drv` files and are
interdependent, so they're maintained as one patch that applies cleanly on the staging base.
- **fractional-scale**: `wp_fractional_scale_v1` so the UI buffer maps **1:1** to a fractional-scaled
  output (e.g. 4K @ 1.7×) instead of being compositor-upscaled (blurry). Binds the manager, uses
  `preferred_scale` for `conf->scale`. Pairs with engine `LogPixels = round(96 × scale)`.
- **xdg_popup menus**: renders Premiere's menu-bar dropdowns as `xdg_popup` (not `wl_subsurface`) so KWin
  positions/constrains/displays them fully (subsurface popups get bottom-clipped). Lightweight SHM menus
  only; bumps `xdg_wm_base` to v3 (set_offset/reposition; must not exceed v3). Backport/adaptation of
  Proton-EM / proton-cachyos 11.0-20260601 (@Etaash-mathamsetty).
- **CSD decoration**: Wine draws its own non-client frame, themed dark via the prefix Control Panel colors
  (default; `WAYLANDDRV_SSD=1` opts into compositor-drawn frames). Self-contained in the prefix, no
  multi-monitor overflow, independent of KWin's global decoration theme. Also implements
  `zxdg_decoration_manager_v1` + `pGetWindowStyleMasks` for the SSD opt-in.

Also carries the **owner-process consumer thread** for the cross-process CEF/WebView2 present path
(`dllmain.c`): it opens each registered host hwnd's shared pixel section and blits it same-process, the
counterpart to `neutron-dcomp-crossproc`. Plus `wayland_pointer.c` and `window_surface.c` present-path
updates from the proven dev tree. Touches: `dlls/winewayland.drv/{Makefile.in, fractional-scale-v1.xml,
xdg-decoration-unstable-v1.xml, wayland.c, wayland_surface.c, waylanddrv.h, waylanddrv_main.c, window.c,
dllmain.c, wayland_pointer.c, window_surface.c}`.

### `neutron-caption-buttons.mypatch`
Flat, dark caption buttons for the client-side decorations, so the `_ [] X` buttons match modern
Adobe/Windows chrome instead of the classic raised 3D bevel. A released, non-hot button uses
`COLOR_ACTIVECAPTION` so it blends into the title bar (only the Marlett glyph shows); the **close**
button turns Windows-red on hover/press with a forced-white glyph; minimize/maximize get a subtle
`COLOR_BTNHIGHLIGHT` hover shade; pressed non-close buttons darken via `COLOR_BTNSHADOW`. Rendered
through the **`NtUserDrawNonClientButton` user-mode callback** (the path uxtheme / MDI menu buttons use)
rather than inline in win32u — `win32u/defwnd.c`'s `draw_{close,max,min}_button` pack `(type, down,
grayed, hot)` into a new `draw_caption_button` and the flat renderer lives in `user32`
(`user_draw_caption_button`). **Optional custom icons** (off by default): when
`HKCU\Software\Neutron\Caption` `Enabled`=1, `user_draw_caption_button` draws `IconDir\{close,min,max,
restore}.ico` (optional `_hover`/`_press` variants) over the themed background via `LoadImageW` +
`NtUserDrawIconEx` (`.ico` only — no WIC/COM in NC paint), falling back to the Marlett glyph when a
file is missing. Collider writes the keys and ships/normalizes the `.ico` sets. Adds `CAPTION_*` to
`enum NONCLIENT_BUTTON_TYPE` and a trailing `hot` to the
`pNonClientButtonDraw` hook + `draw_non_client_button_params`. Hover via a `(hwnd, hittest)` hot-button
pair driven by `handle_nc_mouse_move` (arming `NtUserTrackMouseEvent` `TME_NONCLIENT|TME_LEAVE`) and
cleared by `handle_nc_mouse_leave`; `set_nc_hot_button()`/`redraw_nc_button()` repaint only the affected
button. `uxtheme` routes `CAPTION_*` to the default (user32) drawer so the flat buttons survive an active
visual style. Pairs with `neutron-winewayland-decoration` + the prefix's dark Control Panel colors.
Touches: `include/winuser.h`, `include/ntuser.h`, `dlls/win32u/defwnd.c`,
`dlls/user32/{nonclient.c,user_main.c,user_private.h}`, `dlls/uxtheme/{window.c,uxthemedll.h}`.

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

### `neutron-dcomp-crossproc.mypatch`
**Cross-process present path for CEF/WebView2 panels — the fix for the BLANK Adobe sign-in.** Applies on
top of `neutron-dcomp-bridge` (touches the same `dlls/dcomp/device.c`, so it sorts/applies after it).
Adobe's in-app sign-in (and CEP panels) run their renderer/compositor in a **separate process**
(`msedgewebview2.exe` / `CEPHtmlEngine`), but the host window belongs to the Adobe app process. Wine's
`window_surface` is per-process, so a cross-process `GetDC`+`StretchDIBits` writes into a **detached DC**
(verified: `GetPixel` readback = `CLR_INVALID`) and never reaches the owner's surface → **the sign-in
paints white**. This patch hands the swapchain pixels to the owner via a named shared section keyed by the
host hwnd (`__neutron_dcomp_px_<hwnd>` + a shared hwnd registry); the owner-process consumer
(`neutron-winewayland` `dllmain.c` thread) blits them same-process and flushes so the subsurface commits.
Adds `dcomp_get_pending_hwnd` to the spec. This present path is present in the proven dev-tree wine but was
**missing from the v11.10-1 packaged runtime**, which is exactly why the packaged build rendered the
WebView2 sign-in blank while the dev-tree build rendered it. Pairs with `neutron-cep-child-surface`.

### `neutron-cep-child-surface.mypatch`
The owner-process half of the cross-process present path: in `win32u/window.c`, CEF/CEP/WebView2 panel-host
**child** windows are denied their own `window_surface` (`needs_surface = FALSE`) so the consumer blits the
shared pixels through the child's DC into the **toplevel** surface, which GDI clips to the child's visible
region and tracks as the panel moves/resizes/occludes — content renders **inside** the panel, not as an
on-top overlay subsurface. Also carries the OWL welcome-screen dismiss-on-doc-appear logic. Touches:
`dlls/win32u/window.c`.

### `neutron-mshtml-querycommandsupported.mypatch`
`mshtml` `IHTMLDocument2::queryCommandSupported` returned `E_NOTIMPL`, which throws an uncaught JS exception
in pages that feature-detect clipboard support via `document.queryCommandSupported("copy")` (e.g. Adobe's
`darq` sign-in page), halting their init. Return `VARIANT_FALSE`/`S_OK` ("not supported") so the page's
fallback path runs. Touches: `dlls/mshtml/htmldoc.c`.

### `neutron-win32k-ntoskrnl-forwards.mypatch`
Fills in `win32k.sys` exports that were `stub`s (`RtlLookupFunctionEntry`, `RtlVirtualUnwind`,
`RtlUnwindEx`, `RtlPcToFileHeader`, `__C_specific_handler`, `__chkstk`, `RtlRestoreContext`,
`RtlCopyMemoryNonTemporal`) as forwards to `ntoskrnl.exe`, arch-gated `-arch=!i386`. Touches:
`dlls/win32k.sys/win32k.sys.spec`.

### `neutron-jsonobject-homescreen.mypatch`
UXP home-screen locale/i18n fix (storage-based loader) so the Premiere home screen loads.

### `windows-web-jsonobjectstatics.mypatch`
Supporting JSONObject statics fix (pairs with the home-screen fix).

### `neutron-iocp-completion.mypatch`
Wineserver fix in `async_set_result()` (`server/async.c`): queue an IOCP completion packet for
async I/O on an fd bound to a completion port that has **no APC context**. Premiere's UXP layer
(libuv) drives file reads off its IOCP without an APC context; stock wine never queues the packet,
so libuv's loop blocks forever → **blank UXP home screen**. Reuses `add_async_completion` (no-ops
without a port), honors `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`. A per-completion debug trace is gated
behind `NEUTRON_IOCP_DEBUG` (off by default — it spams stderr and drags playback otherwise). Was
previously a manual wineserver build; captured here so package rebuilds include it. Load-bearing.
Touches: `server/async.c`.

### `neutron-dxcore-xpuinfo.mypatch`
Implements the `DXCoreAdapterProperty` values Adobe **LibXPUInfo.dll** queries during GPU/compute
enumeration (`DedicatedAdapterMemory`, `DedicatedSystemMemory`, `SharedSystemMemory`, `IsIntegrated`,
`IsDetachable`). Upstream Wine left these unimplemented → `get_property_size` returned
`DXGI_ERROR_INVALID_CALL` → LibXPUInfo threw an unhandled C++ exception → **Photoshop 2025 aborted at
init** ("An unrecoverable problem has occurred"). Memory figures come from the wined3d adapter
identifier; integrated/detachable default to FALSE (discrete desktop GPU). Touches: `dlls/dxcore/dxcore.c`.

### `neutron-user32-windowfeedbacksetting.mypatch`
Adds the missing `SetWindowFeedbackSetting` (USER32) export as a no-op touch/pen feedback stub.
Photoshop calls it during document-window creation; without a real export the import stub raises an
unimplemented-function exception and **crashes the app on File → New**. Touches: `dlls/user32/input.c`,
`dlls/user32/user32.spec`.

### `neutron-adobe-winrt-launch.mypatch`
The WinRT/launch fixes genuine Photoshop 2026 (27.8) needs to reach its licensed home screen
(snapshot 2026-07-08, converted from `neutron` repo `patches/staging-20260708-adobe-winrt/`):
- **windows.graphics**: implements the `DisplayInformation` WinRT class (`GetForCurrentView`,
  DPI/orientation properties) with a `weakref.{c,h}` helper (copied verbatim from upstream
  `dlls/windows.ui/weakref.{c,h}`). PS queries display info via WinRT at startup.
- **windows.ui/inputpane.c**: `InputPane` gains `IWeakReferenceSource` (weakref-managed lifetime)
  so Adobe's touch-keyboard probing doesn't fail QueryInterface.
- **dwmapi**: `DwmGetWindowAttribute` `DWMWA_CAPTION_BUTTON_BOUNDS` (attr 5) returns real
  caption-button bounds instead of `E_NOTIMPL`.
- **d2d1**: `DrawGeometryRealization` implemented (was a semi-stub) — Wine keeps the realization's
  source geometry, so it renders as Fill/DrawGeometry on that geometry.
Touches: `dlls/d2d1/device.c`, `dlls/dwmapi/dwmapi_main.c`,
`dlls/windows.graphics/{Makefile.in,main.c,private.h,weakref.c,weakref.h}`,
`dlls/windows.ui/inputpane.c`.

### `neutron-win32u-wmpaint-circuitbreaker.mypatch`
A `WM_PAINT` circuit-breaker in `win32u`. Once Photoshop's home screen shows, OWL's menu bar can storm
millions of unvalidated `WM_PAINT`s while holding the AdobeOwl critical section, wedging the whole app
shell so it never finishes loading. Drains the paint after a threshold of unvalidated repaints; a real
`NtUserBeginPaint` resets the counter so legitimate paint/resize loops never trip it. Touches:
`dlls/win32u/dce.c`, `dlls/win32u/message.c`.

## Diagnostic (`patches/diagnostic/`) — env-gated, safe in shipping builds

### `neutron-present-timing.mypatch`
Present-cadence + blit-duration timing probe behind `NEUTRON_PRESENT_DEBUG`. Used to measure judder /
A-B pacing strategies.

### `neutron-uxp-present-probe.mypatch`
Reads UXP surface pixels before the present blit (diagnostic for the home-screen render investigation).

### `neutron-winewayland-dl-instrument.mypatch`
Temporary `ERR("NEUTRON-DL …")` markers in `winewayland.drv` (`window.c`, `wayland_surface.c`) tracing
`WindowPosChanging`/`WindowPosChanged` for the Photoshop `win_data_mutex` deadlock investigation.
Self-labeled **NOT for production** — re-apply only to re-instrument.

### ~~`neutron-adobe-libxml2-embedded-decl.mypatch`~~ (REMOVED — now in wine-staging v11.10)
Wine's bundled libxml2 rejects `<?xml …?>` declarations embedded inside elements; Adobe's Creative
Cloud / HyperDrive installer emits exactly that in its config XML, so the install aborts. The fix
(tolerate embedded XML declarations in `xmlParsePITarget`/`xmlParsePI`, as Windows MSXML does — from
PhialsBasement/wine-adobe-installers) **now ships in wine-staging v11.10 itself** as
`patches/mshtml-adobe/0002-libs-xml2-Tolerate-embedded-XML-declarations-inside-.patch`, which the
wine-tkg staging pass applies before userpatches — our copy then fails as "previously applied" and
aborts the build. Removed as redundant; the fix is still in every build via staging. If a future
`WINE_BASE` bump drops the staging patchset, re-add it from git history.

### `neutron-adobe-msvcrt-findframe-null.mypatch`
`msvcrt!_FindAndUnlinkFrame` dereferenced NULL when the frame list is empty and the unlinked frame
isn't the head — hit during the Adobe installer's C++ exception unwinding. One-line guard. Ported from
PhialsBasement/wine-adobe-installers.
