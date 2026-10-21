# Patch manifest

Patches are applied on top of the upstream Wine base (see `../build/WINE_BASE`) by `../build/build.sh`,
which drops them into `wine-tkg-git/wine-tkg-userpatches/` (wine-tkg applies every `*.mypatch` there).

⚠️ **Order matters for the `neutron-d2d1-*` set** (2026-07-29): those patches touch the
same regions of `geometry.c`/`device.c` and build on each other — e.g.
`neutron-d2d1-stroke-join-bound-miter-limit` edits the join code introduced by
`neutron-d2d1-outline-join-collinear-spur`. wine-tkg applies `*.mypatch` **in C (byte) order**, and the names are
chosen so that order is correct (hence the `zz-` prefix). **Verify the d2d1 set in C (byte) order, not the
order you generated them in** — a `g`-named stencil patch failed 6 of 8 hunks before being renamed.

Order matters across the rest of the set too: later `zz…` patches build on code that earlier ones
introduce, and the `zz…` prefixes encode the apply order. `build.sh` pins `LC_COLLATE=C` so wine-tkg
applies them in byte order (another locale's collation reverses pairs that differ only by a separator),
and `build/check-patch-order.py` fails the build when a patch does not sort after a patch its header
says it applies after. Each patch is generated against the **staging-applied base** plus every earlier
patch, not plain upstream. `build.sh` also forces `_user_patches_no_confirm="true"`, without which
userpatches are silently skipped on a non-interactive build.

## Production (`patches/`)

### `neutron-wgl-nv-dx-interop.mypatch`  🔑 NEW 2026-08-03
**`WGL_NV_DX_interop` / `_interop2`** — the OpenGL↔DirectX sharing extension. `dlls/win32u/opengl.c`
advertises it and implements the 8 entry points; `dlls/opengl32/wgl.c` adds the D3D-object
inspection it needs; `make_opengl` + the generated `thunks.c/h` + `include/wine/wgl.h` carry the
plumbing.

**Why it matters:** Premiere's display surface runs on the **OpenGL** backend
(`DS.DisableDirectXDisplay=true`), so without this extension its **Program Monitor renders BLACK**
while the rest of the UI looks perfectly fine. Bisected in both directions 2026-08-03: reverting
these three binaries alone blacks the monitor; reverting `dxcore` alone does not.

⛔ **The three binaries move as ONE SET** (`win32u.so`, `opengl32.so`, `opengl32.dll`) — they share
the `dlls/opengl32/unixlib.h` win32u↔opengl32 interface. Mixing vintages gives
`cuGLGetDevices ret=219` and a **"display surface initialization failed"** dialog at startup.

Base: plain upstream HEAD is correct here — **wine-staging touches none of these 7 files** (checked).
Verified: applies with no rejects and reproduces all 7 files **byte-for-byte**.

### `neutron-zzzzz-d2d1-effect-passthrough-scope.mypatch`  NEW 2026-08-03
Captures the d2d1 dev-tree work no patch covered. Chiefly the **regression fix** to
`d2d_effect_image_get_source()`: it returned `inputs[0]` for **any** effect with an input, and
`DrawImage()` follows that chain — so every app drawing through a **real** effect got the effect's
**INPUT drawn instead of its OUTPUT**. Invisible in AE (its `Unpremultiply` IS a stub, so the
fallback is correct); it blacked Premiere's Program Monitor. Now whitelisted to
`CLSID_D2D1UnPremultiply` / `CLSID_D2D1Premultiply` via `d2d_effect.neutron_passthrough`.
Also carries the previously-uncaptured `neutron_skipempty` DC-direct work.

**Named `zzzzz` deliberately: it must collate LAST among the d2d1 set.** It was generated as
base(HEAD + all 50 earlier patches) → dev tree, which is only valid for a patch nothing follows.
⚠️ `neutron-d2d1-unpremultiply-effect.mypatch` is a MIDDLE patch and therefore **cannot** be
regenerated this way — later patches also edit `d2d1_private.h`/`device.c`, so a regeneration
would swallow their hunks. Leave it alone; add to `zzzzz` instead.
Verified: full-set apply reproduces `effect.c` and `d2d1_private.h` byte-for-byte.

### Regenerating a patch
Rebuild the true base (HEAD + every earlier patch in **C collation order**, the `LC_COLLATE=C` that
`build.sh` pins, seeding every file they touch) and diff your working tree against it. **Only valid when no later patch touches the same
files** — otherwise you capture theirs too. Don't do it for a file wine-staging modifies, since HEAD
is then the wrong base. For a new patch, use [`../build/mkpatch.sh`](../build/mkpatch.sh).



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
updates from the proven dev tree.

**Photoshop 2026 layout/present fixes (2026-07-13):**
- **maximized bottom-strip fix** (`display.c`): reserve the compositor SSD titlebar in `rc_work`
  (`rc_work.top += round(32×mode/logical)`), so a maximized window's client no longer overhangs the
  compositor-constrained window and clips the bottom row (Photoshop's new-layer button / status bar).
- **snap-maximize positioning** (`window.c`): position a *compositor-initiated* maximize (KWin drag-to-top
  snap) at the target monitor's work-area origin — gated on `MAXIMIZED && !WS_MAXIMIZE` so the win32u
  maximize-button path (which already positioned it) is untouched and its restore rect isn't corrupted.
- **flush commit-gate** (`window_surface.c` + `wayland_surface.c` `wayland_surface_can_reconfigure` +
  `window.c` `wayland_window_can_commit`): skip the SHM buffer copy at the top of the flush when the xdg
  config isn't commit-compatible yet (the maximize/un-maximize transition), instead of copying tens of MB
  that get discarded — removes the multi-second maximize-settle lag. Output-neutral (win32u retries).
- **SHM buffer pool** (`window_surface.c`): pool 3→4 + a bounded non-blocking free-buffer wait (no second
  fd reader contending with the event thread) — fixes the interactive-resize freeze.
- **per-monitor DPI step ①** (`display.c`): report each output's real DPI (`round(96×mode/logical)`) to
  win32u's per-monitor-DPI engine instead of one system DPI (foundation for correct rendering on a
  mixed-scale multi-monitor desktop; harmless alone — unchanged on the primary).
- **gated diagnostics** (`window.c`): the `neutron-role`/`neutron-chain`/`neutron-rects`/`neutron-decor`
  probes are behind `NEUTRON_WL_DIAG=1` (off by default; the syscall-heavy loops no longer run in normal use).

Touches: `dlls/winewayland.drv/{Makefile.in, fractional-scale-v1.xml, xdg-decoration-unstable-v1.xml,
wayland.c, wayland_surface.c, waylanddrv.h, waylanddrv_main.c, window.c, dllmain.c, wayland_pointer.c,
window_surface.c, display.c}`.

### `neutron-wl-permonitor-scale.mypatch`
Per-monitor DPI, step ②: derive winewayland's presentation scale (`conf->scale` in
`wayland_win_data_get_config`) from the window's monitor **raw dpi** (`NtUserGetWinMonitorDpi`, which
reads win32u's now-correct per-monitor model — see `neutron-win32u-monitor-position`) instead of the
compositor's `preferred_scale`. Because win32u builds the driver buffer at that same monitor raw dpi,
`buffer_px / scale` is a dpi-independent **logical** size, and the compositor then maps it to physical px
per the *actual* output — so a window is presented at the correct **physical** size on whichever monitor
the surface is on (a 4K@1.7× app window dragged to a 1080p@1.0× renders correctly-sized, not 1.7× too
large). The compositor's exact fractional scale is preferred only when it agrees with win32u's monitor
dpi (within ~1 dpi). Named to sort **after** `neutron-winewayland` in any locale (same file, disjoint
`conf->scale` region). Touches: `dlls/winewayland.drv/window.c`.

### `neutron-wintab.mypatch`
Native pen/graphics-tablet (**WinTab**) support for winewayland — pressure-sensitive pen input from a
Wacom on Wayland, so Photoshop brush pressure works. Binds `zwp_tablet_manager_v2` + tablet-v2 tool
events (new `wayland_tablet.c`), bridges them to `wintab32` through the win32u `pWintabProc` seam
(`WAYLAND_WintabProc` + a per-tool packet FIFO in new `wintab.c`), advertises a `WTI_CURSORS` pen cursor
(apps classify the tool via cursor 0 — without it Photoshop won't build a pen stroke), and injects a
synthetic system mouse for cursor tracking + stroke arming (untagged, so PS samples it). Vendors
`tablet-v2.xml`. **Also fixes wintab32 itself** (`dlls/wintab32/wintab32.c`): the internal tablet
message-window is moved onto a dedicated **pump thread**. It previously lived on whichever app thread
first called `WTOpen`; an app that busy-polls `WTPacketsGet` inside an hwnd-filtered stroke loop
(Photoshop) never dispatched the `WT_PACKET` messages, so **zero** packets reached the app mid-stroke
(down+up only = a straight line). A private always-pumping thread makes delivery asynchronous to the
app, matching a real Windows WinTab driver. Sorts **after** `neutron-winewayland` (extends
`waylanddrv.h`/`wayland.c`/`Makefile.in`, removes the obsolete `zwp_tablet_tool_v2_interface` stub).
Touches: `dlls/winewayland.drv/` (`Makefile.in`, `waylanddrv.h`, `wayland.c`, `waylanddrv_main.c`,
`wayland_pointer.c`, new `wayland_tablet.c`/`wintab.c`/`tablet-v2.xml`), `dlls/wintab32/` (`wintab32.c`,
`context.c`, `wintab_internal.h`).
**Tilt + eraser** are implemented: tilt→`pkOrientation` (azimuth/altitude); the eraser end is a second
cursor (`CSR_TYPE_ERASER`, slot 2) so flipping the stylus switches Photoshop to the Eraser tool.
**First-stroke stray fix:** the WinTab system context declared `lcOutExt` in tablet units where a real
Wacom driver declares screen pixels, so Photoshop mapped the first stroke of a session from the wrong
origin; `lcOutExt` now equals `lcSysExt` (screen pixels). The proximity-enter packet is also kept
origin-neutral (`pkChanged=0`), so it cannot set the stroke anchor.

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
file is missing. `neutron theme icons` writes the keys and copies the `.ico` set into the prefix;
Collider ships the sets and calls it. Adds `CAPTION_*` to
`enum NONCLIENT_BUTTON_TYPE` and a trailing `hot` to the
`pNonClientButtonDraw` hook + `draw_non_client_button_params`. Hover via a `(hwnd, hittest)` hot-button
pair driven by `handle_nc_mouse_move` (arming `NtUserTrackMouseEvent` `TME_NONCLIENT|TME_LEAVE`) and
cleared by `handle_nc_mouse_leave`; `set_nc_hot_button()`/`redraw_nc_button()` repaint only the affected
button. `uxtheme` routes `CAPTION_*` to the default (user32) drawer so the flat buttons survive an active
visual style. Pairs with the CSD decoration in `neutron-winewayland` + the prefix's dark Control Panel colors.
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

### `neutron-dcomp-resize-fixes.mypatch`
Resize/maximize robustness for the dcomp GPU-canvas present path — applies on top of `neutron-dcomp-bridge`
+ `neutron-dcomp-crossproc` (same `dlls/dcomp/device.c`; sorts/applies after both). Three fixes in
`blit_swapchain_to_hwnd` / `presentations_proc`: (1) **SEH page-fault guard** around the swapchain read
(`GetBuffer`/`CopyResource`/`Map`) so a resize/recreate race on maximize/un-maximize drops the frame instead
of crashing deep in vkd3d `d3d12core` (the observed AV) — `mt_entered`/`have_pixels` are `volatile` so the
`ID3D10Multithread` lock is always released; (2) **pause blits while the target is mid-resize** (skip while
`GetClientRect` is changing) so a stale readback isn't stretched into the resizing window; (3) **cache the
staging texture**, recreate only on size/swapchain change instead of allocating ~33 MB every 16 ms tick.

### `neutron-dcomp-swapchain-reaper.mypatch`
Premiere Edit↔Export tab-switch stall fix — applies on top of the three dcomp patches above (same
`dlls/dcomp/device.c`; the name sorts last so it applies after `resize-fixes`, whose `p->staging` cache it
references). On a workspace switch CEF's GPU process (`CEPHtmlEngine --type=gpu-process`) recreates its
swapchain for the resized panel and `register_presentation` dropped the **last** reference to the retired
swapchain **synchronously on the app's Present thread, under `presentations_cs`**; DXVK's
`Presenter::destroyResources` then waits unbounded on an abandoned present that never completes → the
GPU-process main thread hung ≥120 s holding the lock, Chromium killed it (`exit_code=2`) + respawned it, and
the multi-second Vulkan re-init was the visible stall. Fix: swap the new swapchain in under the lock, then
hand the retired swapchain to a **one-shot reaper thread** off the Present thread / outside `presentations_cs`
(`release_swapchain_async` / `swapchain_reaper_proc`); same hazard fixed in `unregister_presentations_for_hwnd`.
Worst case on this side = one parked ~0%-CPU thread + one leaked presenter per switch instead of
GPU-process death; the DXVK side of that wait is bounded by the presenter drain-cancel in
`external/patches/dxvk`. Emits `ERR "neutron-reap: queued/released … in N ms"` for A/B observability.
Touches: `dlls/dcomp/device.c`.

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

### `neutron-srw-wake-fastpath.mypatch`
Suite-wide sync-primitive speedup in `dlls/ntdll/sync.c` (helps Chromium/CEF's `base::Lock`=SRWLOCK and
MSVC `std::mutex`/`condition_variable`-heavy Adobe apps; found while profiling the Premiere tab-switch
stall). Wine took a **global** 256-bucket futex spinlock on every `RtlReleaseSRWLock*` / `RtlWakeAddress*`
even with zero waiters — and all page-aligned locks hash to bucket 0, so a 339-thread app bounced that one
cacheline across all cores. Fix: no-waiter wake fast path — lock-free empty-bucket check in
`RtlWakeAddress*`, skip the wake in `RtlReleaseSRWLockShared` unless `exclusive_waiters` is set, and
register the waiter **before** comparing the value in `RtlWaitOnAddress` with a full barrier between
(store-buffer pairing) — lost-wakeup-safe (SB-litmus reasoned). Microbench (8 threads, worst-case same
bucket): uncontended SRW excl 22×, shared 38×, no-waiter `WakeConditionVariable` ~90×; contended handoff
~5–8% slower. Independent of every other patch (nothing else touches `sync.c`). Touches: `dlls/ntdll/sync.c`.

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

### `neutron-dxcore-event-notification.mypatch`
Implements `IDXCoreAdapterFactory`'s three notification methods, which upstream Wine left as stubs
(`IsNotificationTypeSupported` → FALSE, `Register`/`UnregisterEventNotification` → `E_NOTIMPL`).
**After Effects 2025** calls `RegisterEventNotification(AdapterBudgetChange)` while bringing up its
Direct3D 12 display layer and was refused outright. Accepting a registration that never fires is
correct — Windows invokes the callback only when the adapter set or memory budget actually changes,
and it is legal for that never to happen; refusing to register is not. Bookkeeping is real (64-slot
table, unique cookies, a reference held on the registered object, working Unregister), so
register/unregister/re-register behaves correctly; Wine simply has no event source yet.
⚠️ **This did NOT fix the AE composition viewer** (measured: still no swapchain, still blank) — it is
a real defect fixed on its own merits, not the cause of that bug. Applies on top of
`neutron-dxcore-xpuinfo.mypatch`. Touches: `dlls/dxcore/dxcore.c`.

### `neutron-user32-windowfeedbacksetting.mypatch`
Adds the missing `SetWindowFeedbackSetting` (USER32) export as a no-op touch/pen feedback stub.
Photoshop calls it during document-window creation; without a real export the import stub raises an
unimplemented-function exception and **crashes the app on File → New**. Touches: `dlls/user32/input.c`,
`dlls/user32/user32.spec`.

### `neutron-adobe-winrt-launch.mypatch`
The WinRT/launch fixes genuine Photoshop 2026 (27.8) needs to reach its licensed home screen
(snapshot 2026-07-08):
- **windows.graphics**: implements the `DisplayInformation` WinRT class (`GetForCurrentView`,
  DPI/orientation properties) with a `weakref.{c,h}` helper (copied verbatim from upstream
  `dlls/windows.ui/weakref.{c,h}`). PS queries display info via WinRT at startup.
- **windows.ui/inputpane.c**: `InputPane` gains `IWeakReferenceSource` (weakref-managed lifetime)
  so Adobe's touch-keyboard probing doesn't fail QueryInterface.
- **dwmapi**: `DwmGetWindowAttribute` `DWMWA_CAPTION_BUTTON_BOUNDS` (attr 5) returns real
  caption-button bounds instead of `E_NOTIMPL`. Hardened (11.10-3): the bounds are window-relative so
  only the window WIDTH matters; when `GetWindowRect` is degenerate (negative width — a maximized
  window can report a stale/garbage Win32 origin under winewayland, e.g. Photoshop 2026's main window
  reports `left`≈2.6e7) it falls back to the client width so the bounds stay on-window. NOTE: this is
  a correctness fix for the reported bounds. The black caption-button box itself was fixed separately
  (Wine drew nothing in the area Photoshop reserves for the buttons): `neutron-caption-buttons` and
  `neutron-zzzzzzzzzp-caption-buttons-use-caption-palette`.
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

### `neutron-win32u-surface-init-dark.mypatch`
Initialize a fresh `win32u` `window_surface` to a dark neutral (`0x20`) instead of white (`0xff`) in
`create_window_surface` (`dce.c`). The content is only visible transiently before a window first paints,
but `window_surface` recreation during an interactive resize (every 128px bucket) exposes it as a
full-window flash — white flashes hard against a dark (Adobe) UI, dark gray blends in. Single, isolated
one-line hunk (line ~573); disjoint from the wmpaint patch's regions so it applies cleanly in either order.
Touches: `dlls/win32u/dce.c`.

### `neutron-win32u-monitor-position.mypatch`
Preserve each display source's arranged desktop position through `win32u`'s virtual-mode path
(`sysparams.c` `add_modes`). The `physical` devmode is taken from a host mode (position `(0,0)`) and,
once per-monitor DPI makes `get_virtual_modes()` generate scaled modes, `current` is reassigned to a
locally-built `virtual` devmode that also carries `(0,0)` — so a non-primary monitor's real arranged
position is dropped, every extra monitor collapses onto the origin, the virtual desktop never extends,
and `MonitorFromWindow` can't disambiguate which monitor a window is on (per-monitor DPI can never
apply). Capture the driver-supplied `dmPosition` on entry and stamp it back onto `physical`/`virtual`.
Position is canonical (`EnumDisplaySettings` reads it too); no-op for the primary / single-monitor case.
Enables correct mixed-DPI multi-monitor arrangement (e.g. 4K@1.7× + 1080p@1.0× side by side) — the
prerequisite for per-monitor-DPI rendering. Gated diag via `NEUTRON_MON_DIAG=1`.
Touches: `dlls/win32u/sysparams.c`.

### `neutron-server-scale-dpi-sign.mypatch`
Upstream wineserver bug: `scale_dpi()` computes `val * dpi_to` with `int * unsigned`, promoting a
NEGATIVE coordinate to unsigned so it wraps to a huge positive value (a maximized window's physical
origin `-5,-5` scaled 163→96 dpi becomes `26349489` ≈ 2.6e7, and the reported rect goes degenerate,
`left > right`). Every maximized window has a negative origin (it extends past the screen edge by its
frame), so under a fractional-scale prefix (`LogPixels`≠96) every cross-process
`GetWindowRect`/`GetWindowPlacement`/`ClientToScreen` of a maximized window returned a garbage origin
(this is the "PS 2026 reports a ~2.6e7 corner" measurement). Do the arithmetic in signed 64-bit.
Touches: `server/user.h`.

### `neutron-winrt-memstream.mypatch`
Implements the WinRT in-memory stream stack that Adobe Camera Raw's `WML_LoadFromData` (AI Denoise,
Select Subject/Sky masks — and likely PS neural filters) builds on. Adobe's path: activate
`Windows.Storage.Streams.InMemoryRandomAccessStream` → wrap with a `DataWriter` → `WriteBytes(model)`
→ `StoreAsync` → `RandomAccessStreamReference.CreateFromStream` → `LearningModel.LoadFromStream(ref)`;
the real `Microsoft.AI.MachineLearning.dll` then `OpenReadAsync()`s the ref and reads the model bytes.
Upstream Wine had none of the producer half — `InMemoryRandomAccessStream` unregistered, `CreateFromStream`
an `E_NOTIMPL` stub, and wintypes `DataWriter` a pure `IActivationFactory` skeleton with **no**
`IDataWriterFactory` (so `RoGetActivationFactory(DataWriter, IDataWriterFactory)` → `E_NOINTERFACE`) —
so `WML_LoadFromData` failed with Adobe "code 1" on every attempt. Everything downstream (DirectML on
vkd3d-proton, D3D12 compute-queue device via the native `ILearningModelDeviceFactoryNative` route,
session, Evaluate) already worked. Adds `dlls/windows.storage/memstream.c` (the `InMemoryRandomAccessStream`
object over shared refcounted bytes — IRandomAccessStream + IInputStream/IOutputStream + IClosable +
IContentTypeProvider + IRandomAccessStreamWithContentType; the stream reference; `CreateFromStream`; and
four **born-completed** async operations that invoke the completion handler inline so C++/WinRT `.get()`
returns), wires it in `main.c`/`streams.c`/`private.h`/`classes.idl`/`Makefile.in`, and rewrites
`dlls/wintypes/storage.c` into a functional `DataWriter` (`CreateDataWriter` + WriteByte/Bytes/Buffer/
numeric/String buffering + `StoreAsync` flushing to the stream's `WriteAsync` via a wintypes `Buffer` +
DetachStream/DetachBuffer/FlushAsync). Fresh prefixes auto-register the new class via `classes.idl`
(`#pragma makedep register`) at wineboot; existing prefixes need the `ActivatableClassId` key added.
Validated end-to-end through the real WinML binary and confirmed live: LrC AI Denoise runs, "code 1"
gone. Upstreamable. Touches: `dlls/windows.storage/*`, `dlls/wintypes/storage.c`.

### `neutron-win32u-setwindowplacement-noclamp.mypatch`
Lets `SetWindowPlacement` honor full 32-bit rects, fixing scroll-content windows wider/taller than
32,767 px. Win32's `SetWindowPos` clamps coordinates to signed 16-bit — on Windows too — so apps that
need larger extents use `SetWindowPlacement` as the documented 32-bit escape hatch. Adobe's
scroll views (Lightroom's filmstrip, large grid views) size a scroll-content **child** window to the
full content extent this way (a 1113-photo filmstrip ≈ 131k px), then read `GetWindowRect` back to
compute the scrollbar. Wine's `NtUserSetWindowPlacement` forwards into `NtUserSetWindowPos`, which
clamped to 32,767 (in `set_window_pos` + `fixup_swp_flags`) — defeating Adobe's workaround and capping
the filmstrip at ~photo 278/1113. Fix: a private `SWP_NEUTRON_NOCLAMP` flag set only on the
`SetWindowPlacement` path skips the 16-bit clamp (leaving `SetWindowPos`'s clamp intact = matches
Windows) and is masked off before the app/server see `winpos->flags`; also skips the child-coord
`make_rect_onscreen` snap for `WS_CHILD`. Root cause traced to Lightroom's scroll views sizing a
child window through `SetWindowPlacement`; confirmed live — all 1113 filmstrip photos scroll. Upstreamable.
Touches: `dlls/win32u/window.c`. (Applies after `neutron-cep-child-surface` which also touches this
file; order-safe.)

### `neutron-version-datafile-fast.mypatch`
`GetFileVersionInfoSize`/`GetFileVersionInfo` map the target PE with `LOAD_LIBRARY_AS_DATAFILE` instead
of `LOAD_LIBRARY_AS_IMAGE_RESOURCE`. The `AS_IMAGE_RESOURCE` (`SEC_IMAGE`) mapping does eager
section-layout work proportional to the image's virtual size — ~60 ms on a 208 MB binary
(`Photoshop.exe`) — and the version APIs map twice (size + read). Lightroom Classic reads Photoshop's
version resource on every "Edit In" menu validation (~40× per submenu open), turning that into a ~10 s
stall. The datafile mapping is demand-paged (no section layout), ~190× faster (0.3 ms vs 60 ms), and
the version-resource `FindResource`/`LoadResource` path returns byte-identical data (verified: same
816-byte `VS_VERSION_INFO`, same parsed ProductVersion). Measured: Size+Get on Photoshop.exe
121.7 ms → 0.42 ms (288×); the LrC submenu stall dropped ~10 s → ~100 ms. Touches:
`dlls/kernelbase/version.c`. Upstreamable.

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

### `neutron-d2d1-unpremultiply-effect.mypatch` (2026-08-02) — After Effects composition viewer
Wine's `dlls/d2d1/effect.c` registered 17 built-in effects; `CLSID_D2D1UnPremultiply` was not one of
them ("premultiply" appeared nowhere in d2d1). After Effects' hardware blit
(in `aedisplay.dll`) calls
`ID2D1DeviceContext::CreateEffect(CLSID_D2D1UnPremultiply)` on every blit, turns the failed HRESULT
into `0xA00F0001`, and **throws it as a bare `int`**; the blit loop catches it,
logs `Permanent failure: -1609629695`, and sets a **process-wide
latch** that kills the hardware blit for the rest of the session. Measured:
`warn:d2d:d2d_effect_create Effect id {fb9ac489-…} not found.` ×13, matching 13 latch messages, on
AE's blit thread. No other D2D effect failed to be created: the blit needs exactly one missing effect,
this one.
Registers `CLSID_D2D1UnPremultiply` and `CLSID_D2D1Premultiply` in the same stub form as the other
built-ins, and adds `d2d_effect_image_get_source()` so a stub effect's output image can be resolved
to its source input.
✅ **CAPTURE NOW COMPLETE (2026-08-03).** The matching `d2d_device_context_DrawImage()` hunk is
included, so the patch matches the binary that was actually tested. The patch was also
**regenerated against the staging base** — the original was a `git diff` against the dev tree, and
its context carried `NEUTRON_API_TIME()` lines that `neutron-d2d1-layer-clip-dcdirect` already
adds, so hunk 3 FAILED to apply. That hunk was the one *defining* `d2d_effect_image_get_source`,
which the header declares and `device.c` calls ⇒ the build would not have linked, and this fix
would have been silently lost. **Cut patches against the patched tree, never the dev tree.**
✅ Verified with it: the whole D2D half of AE's blit succeeds — `CreateEffect(unpremultiply) hr=0`,
`SetTarget(dxgi-backed) 2734x1174`, `DrawImage(effect->source)`, `EndDraw(bitmap-target,drew,OK)`.
❌ Does NOT by itself make the composition appear — and the "second bug" named here was
**misdiagnosed**. AE's UI-overlay draw does not fail on D2D command lists: it fails because
`cuGraphicsD3D11RegisterResource` returns `CUDA_ERROR_UNKNOWN` (161 failures = 161 latches, 1:1).
Fixed outside Wine, in nvcuda + DXVK — see `external/patches/nvcuda` and `external/patches/dxvk`
([`../external/README.md`](../external/README.md)). ✅ With both, the composition
renders **and** the UI overlay draws (user-confirmed 2026-08-03).

### `neutron-zzzzzzzzzzzzze-d2d1-color-management.mypatch` (2026-10-09) — Lightroom (cloud) startup
Lightroom quit during startup with "CreateD2DDeviceResources failed. HResult: 0x88990028"
(`D2DERR_EFFECT_IS_NOT_REGISTERED`): Wine has no `CLSID_D2D1ColorManagement`. Registers it as a
pass-through stub like Unpremultiply (its six real properties, so the app can set and read them; no
color conversion yet), and implements color contexts, where all five `CreateColorContext*` methods
returned `E_NOTIMPL`. **Off unless `NEUTRON_D2D_COLOR_MANAGEMENT=1`**, which only the Lightroom
launch profile sets: Premiere, After Effects, Media Encoder, Photoshop and Illustrator reference the
effect too, and they keep their own fallback until the effect really converts colors. With the
switch off, d2d1 behaves exactly as before.

### `neutron-zzzzzzzzzzzzzf-winewayland-clickthrough-overlay-not-topmost.mypatch` (2026-10-10) — Lightroom dimming
Lightroom Classic dims its main window behind the Adobe sign-in and What's New with a click-through
layered popup that lacks `WS_EX_NOACTIVATE`, so it missed the click-through-overlay rule and became an
`xdg_popup`, which KWin keeps above everything: the sign-in window and every other application were
dimmed. `WS_EX_NOACTIVATE` is now required only for topmost windows; an owned window that is not
topmost stacks with its owner (a subsurface), as on Windows. Menus and tooltips are topmost.

### `neutron-zzzzzzzzzzzzzg-winewayland-owned-overlay-zorder.mypatch` (2026-10-10) — Lightroom first-run help froze the app
Lightroom Classic's first-run help stacks a help bubble, a highlight box, a dim layer and an opaque,
disabled input blocker over its main window. The bubble ended up underneath and the app waited for a
click nobody could make. The disabled blocker now counts as an overlay (subsurface, not popup); owned
subsurfaces are restacked in Win32 z-order rather than HWND order; and that restack now also runs when
an owned subsurface is shown, hidden, re-z-ordered or recreated, not only on GPU presents. Verified
with a test program that recreates the four windows (same styles, same creation order).

### `neutron-zzzzzzzzzzzzzh-winewayland-dnd-with-data-control.mypatch` (2026-10-10) — drag-and-drop crash on Hyprland
Dragging a file from the file manager onto Photoshop or Premiere on Hyprland closed the app as the
file entered its window. Hyprland also offers `zwlr_data_control_manager_v1` for the clipboard (KDE
does not), and the driver kept either a data-control device or a `wl_data_device` in one union,
deciding an offer's type by whether data-control was bound. Our drag-and-drop patch creates a
`wl_data_device` in every process when `wl_data_device_manager` is announced, which Hyprland does
long before data-control, so drag requests went out on the wrong object and Hyprland disconnected
the app; in the clipboard process the event thread also aborted on an unhandled `primary_selection`,
breaking copy and paste with Linux apps. Both devices now coexist (separate fields), each offer
records which device delivered it, both are created when both managers exist (so drops also arrive
on compositors that announce data-control before the seat, such as sway), and every listener slot
has a handler. Verified on a nested Hyprland 0.56.2 with a drag driven by a virtual pointer.

### `neutron-zzzzzzzzzzzzzi-winewayland-restack-all-client-subsurfaces.mypatch` (2026-10-11) — Lightroom Develop tips covered by the photo
In Lightroom Classic's Develop module, after "Next" on the first help tip, the photo drew over the
tip and the tip could not be closed; Alt put it back until the next "Next". The photo is a GPU child
deep in the main frame's 240+ windows, and the restack's Win32 tree walk stops at 64, so the photo's
subsurface was never placed -- and an unplaced sibling ends up above the chain the restack builds
from the parent up. Every "Next" recreates the highlighter, restacks, and lifted the photo over the
tip. The restack now also takes every client subsurface attached to the toplevel from the driver's
own tree and places the ones the walk missed above the ordered client surfaces and below the owned
windows. Verified with a test program (100 children, a still D3D11 child, the tip stack, a recreated
highlighter) and in Lightroom Classic.

### `neutron-zzzzzzzzzzzzzj-kernel32-unregister-app-recovery-callback.mypatch` (2026-10-11) — crash dialog on Lightroom's relaunch
Lightroom Classic's "Reset all preferences and relaunch" showed Adobe's crash dialog: before
relaunching it calls `UnregisterApplicationRecoveryCallback`, which kernel32 did not export (only
the Register half, as a stub faking success), so the call aborted ("unimplemented function").
Adds the counterpart, faking success the same way.

### `neutron-zzzzzzzzzzzzzk-user32-readable-control-text.mypatch` (2026-10-11) — Lightroom About box labels
Lightroom Classic's About box drew its labels and two buttons as black text on dark boxes: MFC sets
black text on `COLOR_BTNFACE`, which is light on Windows and 43 in Neutron's dark palette, and the
palette cannot be lightened without turning Lightroom's menus light (MFC caches it for both).
`DrawTextExW` now uses the palette's text color when the background is the palette's button face
(or window color), the app's text has under 2:1 contrast against it, and the DC belongs to a
Button/Static window or is a memory DC on the button face (MFC's button buffer). Readable text is
never touched; with Windows' default palette the rule never acts.

### `neutron-zzzzzzzzzzzzzl-d2d1-layer-unwrite-at-push-transform.mypatch` (2026-10-11) — Lightroom Navigator thumbnail cut off
Lightroom Classic's Develop Navigator thumbnail lost a strip on the right and at the bottom a few
seconds after entering Develop. Lightroom translates the thumbnail by (10,31), pushes two layers
masked to its rectangle, then restores its drawing state (identity) BEFORE popping them. The
path-mask stencil wrote each mask under the transform at PushLayer but unwrote it under the
transform at PopLayer, so the unwrite missed by (10,31) and left those strips promoted; the stencil
is not cleared between frames, so the next frame's thumbnail failed the mask test there. The
composed maskTransform x world matrix is now stored at push and used for the unwrite. Verified with
a test program that replays Lightroom's sequence (fails on 104, passes with the stencil off and with
this change).

### `neutron-zzzzzzzzzzzzzm-winewayland-remapped-control-key-vkey.mypatch` (2026-10-11) — remapped Escape/Caps Lock (Hyprland)
With Caps Lock and Escape swapped (xkb `caps:swapescape`), the key that types Escape did not close
Photoshop's menus but did leave a text box. winewayland took a key's character from the keymap
but its VK only from its physical position, so the physical Caps Lock key arrived as VK_CAPITAL
with an Escape character; menus close on VK_ESCAPE. When a key's base keysym is a control key
people remap (Escape, Caps Lock, BackSpace, Tab, Return, Shift, Control, Alt, Super), the keysym
now decides its VK, as in winex11. Character keys keep the layout tables. Verified with a test
program in a nested Hyprland (caps:swapescape, no option, ctrl:nocaps).

### `neutron-zzzzzzzzzzzzzn-winrt-mediasource-mediaplayer.mypatch` (2026-10-12) — Lightroom help videos (WinRT media)
The Assisted Culling help video in Lightroom Classic's Import dialog did not play: its video toolkit
opens clips with `Windows.Media.Core.MediaSource`, which Wine lacked (0x80040154 from
CreateMediaSource; the codec was fine). Adds `MediaSource` (CreateFromUri), the
`AdaptiveMediaSource` statics (Lightroom calls CreateFromUriAsync unchecked; it now completes with
UnsupportedManifestContentType, as on Windows for a plain file, and Lightroom falls back to
MediaSource and plays through IMFMediaEngine), and `MediaPlaybackItem`, `MediaPlaybackSession` and a
real `MediaPlayer` (IMFMediaEngine, frame server) in place of stubs, with the IDL they need. Paired
with `external/patches/dxvk/0002` (d3d11's WinRT interop exports). Verified with a test program that
plays the clip through the MediaPlayer frame server into a D3D11 surface, and in Lightroom Classic.

### `neutron-zzzzzzzzzzzzzo-cep-panel-host-container-shape.mypatch` (2026-10-12) — Audition Learn panel blank
Audition 2026's Learn panel (a CEP extension) stayed white while DevTools showed the page fully
rendered. Two gaps on the cross-process CEP present path. (1) dcomp found a panel's host window only
by PlugPlug class name (`WC_PLUGPLUG_HTMLEXTENSION` in Premiere, `PLUGPLUG_UI_NATIVE` in Animate);
Audition hosts CEP panels in its own DVA window (`{guid} <- DroverLord`), so no host was found and the
frames went to CEF's own window. A structural fallback now picks the window directly above the
`{guid}` container (CefBrowserWindow's parent, in the app's process) -- the position the PlugPlug
hosts occupy too -- only when neither class matches. (2) winewayland's consumer drew through
`GetDC(host)`, which honors `WS_CLIPCHILDREN`; Audition's host has it and its `{guid}` child covers it,
so every blit was clipped to nothing. The consumer no longer clips children. Verified in Audition:
clearing the style by hand made the panel appear, and the patched build renders it.

### `neutron-zzzzzzzzzzzzzp-teb-version-1e00-fiber-flag.mypatch` (2026-10-12) — InDesign crash after the home screen
InDesign 2026 crashed seconds after its home screen appeared (access violation in `PMRuntime.dll`,
RAX=0, from its Font Manager thread). `K2Memory::IsUIThread()` reads `gs:[0x20]` (NT_TIB.Version /
FiberData), returns early on 0x1e00 and otherwise dereferences it. Windows starts every thread at
0x1e00 and tracks fibers with `TEB.HasFiberData`; Wine left the slot NULL. `init_teb` now sets
0x1e00 (both TEBs), and kernelbase's fiber functions use `HasFiberData`, restoring 0x1e00 in
`ConvertFiberToThread`. Verified with `harnesses/teb-fiber-version.c` (107 fails 4 of 18 checks,
the patched build passes all) and in InDesign (new document, typing).

### `neutron-zzzzzzzzzzzzzq-winhttp-async-without-callback-is-sync.mypatch` (2026-10-12) — "Cannot display help." (Neutron#13)
Every Help menu page in Lightroom Classic failed with "Cannot display help." Adobe's Help client
(`ahclient.dll`, also in Premiere, Audition, InDesign, Animate, Media Encoder) checks it is online with a
WinHTTP session opened `WINHTTP_FLAG_ASYNC`, no status callback, and `WinHttpSendRequest` /
`WinHttpReceiveResponse` / `WinHttpQueryHeaders(STATUS_CODE)` back to back. Wine queued them, the status
query failed with `ERROR_WINHTTP_INCORRECT_HANDLE_STATE`, and the client took its offline path (ARH.exe,
not shipped) and gave up. A request with no status callback now runs send, receive, query-data-available,
read and write synchronously; requests with a callback are unchanged. That Windows completes these calls
is inferred from Adobe's Help working there (Microsoft documents only that `WinHttpReceiveResponse` "can
operate either synchronously or asynchronously" in async mode). Verified with
`harnesses/winhttp-async-nocallback.c` (108-pre fails the no-callback status query, the patched build
passes; the with-callback control passes on both) and in Lightroom Classic (Help reaches winebrowser).

### `neutron-zzzzzzzzzzzzzr-winewayland-oversized-popup-as-placed.mypatch` (2026-10-12) — Animate's first launch: black window covering the screen
Animate 2024's first-launch overlay ("New to Animate?") is an owned `WS_POPUP` Animate sizes to its main window's
area times its UI scale: 6714x3557 px at 175%, larger than any monitor (Animate's own arithmetic; every input to it
matches Windows). Two driver faults hid it. (1) The xdg_positioner asked KWin to SLIDE popups on-screen; a popup larger
than every output cannot fit, so KWin slid it until its bottom-right met the screen's, leaving only black backdrop on
screen and the card off it. Slide is now requested only along an axis where the popup fits an output (menus and
dropdowns unchanged). (2) win32u clips a window surface to the desktop, so the buffer was smaller than the window but
the viewport destination was the full window: the buffer was stretched 1.17x by 1.74x and the card was drawn away from
where Animate hit-tests it (buttons dead). A buffer smaller than its window is now presented 1:1, at attach and at
reconfigure (`wayland_surface_viewport_destination`). Not handled: a window extending past the desktop's left or top
(its buffer does not start at the window origin). Verified with `harnesses/popup-oversize.c` +
`popup-oversize-check.py` (108: `big` fails, `edge` passes; patched: both pass) and in Animate (card centered, buttons
work); regression pass in Premiere 2025, Lightroom Classic, Audition, InDesign (operator).

### `neutron-zzzzzzzzzzzzzs-win32u-owner-drawn-menu-readable-text.mypatch` (2026-10-12) — InDesign font menu unreadable
InDesign 2026's Type > Font menu drew the font names and "Sample" previews black on Neutron's dark menu (COLOR_MENU
43). InDesign owner-draws those rows by copying each row into its own bitmap, rasterizing black text there and copying
it back, so the text reaches Wine as pixels and cannot be recolored where it is drawn (as `DrawTextExW` does for
controls, patch `…zzzzzzzzzzzzzk`). After `WM_DRAWITEM`, the row is now corrected when the palette's menu color is dark
and neutral, at least half the row is still exactly that color, and the row holds near-black pixels: each neutral pixel
darker than the menu color is remapped from black-over-menu to menu-text-over-menu with the same coverage. Colored
pixels and anything lighter than the background are untouched; with Windows' default light palette it never acts.
Verified with `preserved-fixes/harnesses/odmenu-text.c` (three rows drawn InDesign's way: black, COLOR_MENUTEXT, red;
dark palette fails on 109 and passes patched, light palette passes both) and in InDesign. The highlighted font row
still draws another row's text over its own (parked, OPEN-ISSUES).

### `neutron-zzzzzzzzzzzzzt-ntdll-actctx-config-private-path.mypatch` (2026-10-12) — After Effects: Mocha AE plug-in failed to load
After Effects 2026 warned at every start that `mochashape4ae_adobe.aex` (Boris FX Mocha AE) failed to load (GitHub
Neutron#14). Its embedded manifest depends on the private assembly `BorisFX.qt.shared-dlls`, which lives in
`Resources\mochaui\bin` and which Windows finds through `mochashape4ae_adobe.aex.2.config`'s
`<probing privatePath=".\Resources\mochaui\bin"/>`; Wine never read application configuration files. An activation
context created from resource N of a module now reads `<module>.N.config` (`<exe>.config` for resource 1) and, after the
usual probing fails, searches each privatePath directory (relative to the module, at most 9, absolute ones ignored); the
file is also reported as the root configuration path. Verified with `preserved-fixes/harnesses/sxs-config-probing/`
(110 fails with and without the .config; patched loads only with it) and in After Effects 2026: no warning, Mocha AE
applies and its interface opens.

### `neutron-zzzzzzzzzzzzzu-winmm-capture-buffer-1s.mypatch` (2026-10-12) — Premiere voice-over recordings full of silent gaps
Premiere 2026's voice-over recordings came back as bursts separated by silence (GitHub Neutron#17). Premiere records through
MME (waveIn, CALLBACK_THREAD) with ~60 ms of small buffers and sometimes hands them back late; `WID_PullData` only reads
while buffers are queued, so the audio waits in the mmdevapi capture buffer — sized like playback, `AC_BUFLEN` = 100 ms —
and everything past it was lost; Premiere padded the missing time with 1024-sample blocks of silence. Capture now asks for
1 s (`AC_CAPTURE_BUFLEN`); no added latency while the app keeps up; playback unchanged. Verified with
`preserved-fixes/harnesses/capture-gaps.c` (ten 6 ms buffers + 250/400 ms stalls: 110 loses 1181/2289 ms, patched 14/22) and
Premiere 2026 recording a steady tone (110: gaps in 2/2 recordings; patched: 0 gaps in 3).

### `neutron-zzzzzzzzzzzzzv-dwrite-glyph-cache-rendering-mode.mypatch` (2026-10-12) — After Effects crashed while scrubbing
After Effects 2026 crashed 10-30 s into scrubbing a reporter's project (GitHub Neutron#14), always inside glyph-run
rendering (`glyphrunanalysis_render`) with a wild pointer. The glyph cache keyed rendered bitmaps by glyph, size and
transform, not by rendering mode: an ALIASED bitmap is 1 bit per pixel and the other modes 8, so a glyph rendered
antialiased and then aliased had the cached 8bpp bitmap copied into a 1bpp-sized buffer, overflowing the heap and
crashing later in varying places. (Our earlier patch that put the transform into the key made this reachable at every
HiDPI scale; upstream skipped the cache for any non-identity transform.) The rendering mode is now part of a bitmap's
key, and a cached bitmap whose size differs from the one requested is rendered again instead of copied. Verified with
`preserved-fixes/harnesses/dwrite-mode-cache.c` (110 corrupts the heap; patched stays valid and the aliased texture
equals a fresh render) and in After Effects 2026 with the reporter's project: crashed 2 of 2 runs scrubbing before,
0 of 2 patched.

### `neutron-zzzzzzzzzzzzzw-d2d1-dwrite-glyph-run-clip-to-target.mypatch` (2026-10-12) — After Effects crashed on its expression-error bar
After Effects 2026 sometimes crashed while the comp panel's expression-error bar was shown (GitHub Neutron#14, second
crash). Some draws of the bar's own text arrive with garbage advances (1e36, NaN) that put the rest of the run up to a
billion pixels away; d2d1 rasterized every run into one bitmap covering the whole run, so a run wider than 16384 px was
not drawn at all and wider ones overflowed the size arithmetic in d2d1 (32-bit, and float in `d2d_size_set`) and in
dwrite (the run bitmap could be far smaller than the glyphs copied into it). Windows draws only what is visible. d2d1 now
clips a run's bitmap to the target; dwrite renders a run whose whole bitmap would exceed 16 MiB straight into the
caller's texture (only the requested part), sizes textures without 32-bit overflow and clamps glyph origins to ±1e9 px.
Runs that fit are unchanged; `NEUTRON_GLYPH_CLIP=0` turns it off; the first 8 oversized runs are logged
(`neutron-glyphclip`). Verified with `preserved-fixes/harnesses/d2d-huge-glyphrun.c` (110 draws nothing for a
3000-glyph line or runs with a glyph 2e7/2e9 DIPs away; patched, the visible part is pixel-identical to a short run, in
grayscale, ClearType and aliased) and in After Effects 2026 with the reporter's project: fix off, 179 failed glyph draws
and a crash in 1 of 2 runs; fix on, 0 failed draws, no crash. ⚠️ Where the garbage advances come from is still open
(OPEN-ISSUES #14).

### `neutron-zzzzzzzzzzzzzx-d2d1-command-list-field-offsets.mypatch` (2026-10-13) — After Effects text garbled in places (expression-error bar)
The garbage glyph advances behind GitHub Neutron#14's second crash (1e36, NaN; 114 stopped them from crashing) came from
d2d1's command lists: `d2d_command_list_draw_glyph_run` copied a run's indices/advances/offsets and its description's
locale/string/cluster map into the command buffer and kept POINTERS to the copies (DrawBitmap, FillOpacityMask and
DrawImage kept their rectangles the same way), and the buffer is realloc'd as later commands are recorded — so a list that
grew afterwards replayed them from freed memory. Upstream Wine bug. The fields now hold offsets from the command;
`d2d_command_list_Stream` resolves them (`d2d_command_field`). A description without a locale name no longer calls
`wcslen(NULL)`. Verified with `preserved-fixes/harnesses/d2d-cmdlist-glyphrun.c` (114: replay after growth draws nothing /
stray glyphs; patched identical to a direct draw; no-growth control passes on both), Wine's d2d1 conformance tests (same 46
failures of 17069 before and after; hand-built, tests are disabled in our configure) and After Effects 2026 with the
reporter's project: 114 logged 8 oversized runs and drew the error text garbled in places; patched 0, no garbling, no crash.

### `neutron-zzzzzzzzzzzzzy-winewayland-primary-monitor-origin.mypatch` (2026-10-13) — Premiere clicks landed above the pointer (multi-monitor GNOME)
GitHub Neutron#16: on a reporter's GNOME desktop (4K @125% placed 443 logical px lower than the 1080p @100% beside it) the bottom
of Premiere's maximized window could not be clicked; clicks landed above the pointer. No output was at logical (0,0), so the
arrangement made the first output primary at (0,443) — Windows always has the primary at (0,0), and Premiere computes its
maximized position as if work-area coordinates were primary-relative, so it maximized 443 px lower, past the bottom of the
virtual screen, where every pointer position was clamped to the screen edge. After overlap resolution the layout is now
translated so the primary sits at the origin (others may be negative, as on Windows); origin-anchored and single-monitor layouts
are unchanged. Kill switch `NEUTRON_PRIMARY_ORIGIN=0`. Verified with `preserved-fixes/harnesses/monitors.c` and Premiere 2026 in
nested mutter 51 with the reporter's layout: before, primary (0,354) and Premiere maximized at (0,882)-(2240,2282) on a monitor
ending at 1843; patched, primary (0,0), Premiere maximized at exactly the monitor and the pointer reached the last row (y=1399).

### `neutron-zzzzzzzzzzzzzz-winepulse-endpoint-names-like-windows.mypatch` (2026-10-13) — Premiere's audio device names cut alike
Premiere's Audio Hardware list (MME only; Premiere has no WASAPI choice) showed every device cut to 31 characters and both
Scarlett Solo inputs as "Microphone (Scarlett Solo (3rd " (operator, while testing Neutron#17). MME's szPname is
MAXPNAMELEN WCHARs and winmm fills it with the endpoint's friendly name cut to fit — Windows cuts too, but its friendly
name is "<endpoint> (<adapter>)", so what tells a card's endpoints apart comes first; mmdevapi named every endpoint
"Microphone (<PulseAudio description>)" / "Speakers (…)", and that description is "<card> <endpoint>". winepulse now splits
the description into card (PipeWire `device.description`, or for PulseAudio the part before `device.profile.description`)
and endpoint and returns them for DEVPKEY_Device_DeviceDesc / DEVPKEY_DeviceInterface_FriendlyName (monitors keep
"Monitor of " on the endpoint part); mmdevapi builds "<endpoint> (<card>)" from them. Unsplittable descriptions (virtual
devices, the default "PulseAudio Input/Output") keep their names; endpoint GUIDs (keyed by node name) do not change. Kill
switch `NEUTRON_ENDPOINT_NAMES=0`. Verified with `preserved-fixes/harnesses/mme-device-names.c` on the operator's devices
(116: 12 of 14 cut, Scarlett inputs collide; patched: "Input 1 Mic (Scarlett Solo (3rd" / "Input 2 Inst/Line (Scarlett
Sol", "Analog Stereo (Built-in Audio)" whole, no collision; kill switch = 116 exactly), a PulseAudio-form null sink ("Line
Out (Neutron Test Card)") and Premiere 2025 in the clean room (new names, no duplicates). Long names are still cut at 31,
as on Windows. The Scarlett's "(Not working)" label in Premiere also appears with the kill switch: not this change.

### `neutron-zzzzzzzzzzzzzzz-dsound-description-wavedeviceid.mypatch` (2026-10-13) — Premiere: every named audio device "(Not working)"
Premiere's Audio Hardware showed "(Not working)" on every named device (Scarlett, Easy Effects, built-in) once selected and fell
back to System Default; a named Default Input kept the voice-over button greyed (operator, testing Neutron#17). Wine's MME was
fine (`harnesses/mme-open-each.c`). Premiere's audio backend ("DirectSoundHelper": Premiere 2025 dvaaudiodevice 25.3, and the
code in Premiere 2026's exe) maps MME devices to endpoints through dsound's CLSID_DirectSoundPrivate IKsPropertySet,
DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_W (id 6): WaveDeviceId = the MME number, DeviceId = the ID it stores. Wine never set
WaveDeviceId (or DataFlow), so named devices got the fallback ID "<MME name>-out" (seen in Premiere's memory), which matched no
endpoint's PKEY_AudioEndpoint_GUID on selection. DescriptionW now returns the waveOut/waveIn device whose
DRV_QUERYFUNCTIONINSTANCEID is the endpoint's ID, and its DataFlow, as Windows does (ENUMERATE_W already numbered this way).
Kill switch `NEUTRON_DSOUND_WAVE_ID=0`. Verified with `harnesses/dsound-wavedeviceid.c` (116: WaveDeviceId untouched for all 14
devices; patched 14/14; kill switch = 116) and Premiere 2025 in the clean room (Easy Effects output: no label, sound plays;
Scarlett Input 1: voice-over enabled).

### `neutron-zzzzzzzzzzzzzzzz-ntdll-appenv-per-app-environment.mypatch` (2026-10-13) — Dynamic Link: After Effects started by Premiere ran with Premiere's settings
Operator, clean room AE2026 on 117, Premiere 2026 → File → Adobe Dynamic Link → New After Effects Composition with After
Effects closed: *"AE opens, no splash screen, Composition panel is blank and black, UI appears fine."* AfterFX.exe carried
exactly Premiere's environment: a Wine child gets its parent's Windows block from the server (`init_startup_info`) and its
Unix environ through fork() (`spawn_process`), and `neutron launch` sets per-app settings only on the process it starts.
The profiles conflict (Premiere's NEUTRON_D2D_COMPOSEREAD paints AE's canvas white; AE needs NEUTRON_DXVK_EXPORT_TEXTURES
and NEUTRON_WL_ORPHAN_TOPLEVEL), so no single environment serves both. `init_startup_info` now calls
`neutron_apply_app_env`: it reads `<prefix>/neutron-appenv.conf` (written by `neutron launch` on every launch: one
section per installed app exe, named by its Windows image path, `KEY=VALUE` sets / `-KEY` clears, including
`NEUTRON_APP=<profile>`); when the image path names a section (case-insensitive) and the inherited NEUTRON_APP differs,
it applies the section to the Windows block (`set_env_var`) and the Unix environ (`setenv`/`unsetenv`) before any module
reads them, and logs one ERR line `neutron-appenv: <image> started from <app>: applied its own environment (N set, M
cleared)`. A direct launch already carries its own NEUTRON_APP and is left alone, so command-line overrides still win
there. Kill switch `NEUTRON_APPENV=0`. Verified: `preserved-fixes/harnesses/appenv-spawn-test.sh` (stock 117 fails the
apply / case / grandchild cases; patched 6/6: Windows block, getenv via WINEDLLOVERRIDES load order, and a grandchild's
/proc environ); operator on the clean room with the patched ntdll: *"splash shows, comp draws, scrubbing is smooth"*.
⚠️ `/proc/<pid>/environ` is an exec-time snapshot — the rewritten process itself still SHOWS the parent's values there;
check its children or the ERR line.

### `neutron-zzzzzzzzzzzzzzzzz-winewayland-clip-gpu-children-to-ancestors.mypatch` (2026-10-13) — Lightroom Classic What's New videos drawn outside the pop-up
Operator, clean room LrC 15.4.1: *"All of the videos are on screen and not clipping in the pop up like they should."* The
videos are D3D11 swapchains on children of a scrolled ClipView; winewayland shows each GPU child as a wl_subsurface at
its full rect, and a Wayland subsurface is not clipped by its parent (upstream does not clip either; X11 gets it from X
child windows). win32u now records each Vulkan present's buffer size on the client surface and brackets the present
with `client_surface.present_busy` (measured with WAYLAND_DEBUG: NVIDIA's WSI sends every request for the surface —
syncobj acquire/release points, attach, damage, commit — inside vkQueuePresentKHR, FIFO 462/462 and MAILBOX 458/458).
winewayland intersects every ancestor's client area (`neutron_ancestor_clip`): entirely outside → 1x1 placed BELOW the
toplevel (kept mapped: a detached surface wedges present-wait); an edge an ancestor cuts rounds inward with a matching
`wp_viewport_set_source` crop (only with a known buffer size: out_of_buffer is fatal); touching edges keep the outward
rounding (his call — inward measurably thickened LrC Develop's border, Premiere's old black-L mechanism). A crop change
made outside a present is committed at once, synchronized with the parent (set_sync / commit / parent commit /
set_desync), and one made during a present right after it — playing videos had flickered at the clip edge, a STOPPED
video kept a stale 1x1 crop and vanished. OpenGL client surfaces keep the old behavior. Kill switch
`NEUTRON_CLIENT_CLIP=0`. Verified: `preserved-fixes/harnesses/clipped-gpu-child.c` + `-check.py` (every pixel,
native `spectacle -m`): stock 118 fails all 8 checks, patched passes all (render thread, scrolling container, stopped
video, touching edges); 60 s MAILBOX stress scroll, ~61k presents, no protocol error; cost measured (diagnostic build):
ancestor walk ~1-2.5 us per update, 0 busy waits in Premiere, 1 of ~65k in the stress run. Operator, clean room: What's
New *"no flicker, videos stay inside, none disappear"*; LrC Develop pixel-identical to shipped at all four edges;
Premiere unchanged (A/B: same playback jitter on shipped).

### `neutron-zzzzzzzzzzzzzzzzzzzz-dxcore-real-adapter-identity.mypatch` (2026-10-14) — DXCore names the real GPU and answers its queries
Found on GitHub Neutron#18 (AMD; Photoshop "DirectX unavailable"). DXCore took wined3d's identity: with the GL renderer a
guess when the driver can't report PCI IDs or the card isn't in wined3d's table (RTX 5070 -> "GTX 470" 10de:06cd; RX 7900 XT
0x744c -> an older Radeon), while DXGI/D3DKMT/Vulkan name the real card; IsValid/IsAttributeSupported/IsPropertySupported were
FALSE stubs and CreateAdapterList ignored its filter. Users: Photoshop (LibXPUInfo, sniffer), Premiere (GPUSniffer, app), Media
Encoder, onnxruntime. Now: PCI IDs + name from the display device whose DEVPROPKEY_GPU_LUID matches (as Wine's win32u tests
map it); IsValid TRUE; D3D11/D3D12 graphics + D3D12 core compute supported for such a GPU (ML/media not); IsPropertySupported
= what GetProperty implements; CreateAdapterList filters on the modelled attributes only (ML/media filters keep listing every
adapter, so onnxruntime/DirectML see what they did before). Kill switch `NEUTRON_DXCORE_IDENTITY=0`. Verified on the RTX 5070:
`preserved-fixes/harnesses/gpu-report.c` (DXCore = DXGI = D3DKMT = Vulkan: RTX 5070 10de:2f04, IsValid/D3D11/D3D12/compute 1;
kill switch = old output); Photoshop 2026 still finds and uses the GPU (sniffer 2 displays + 1 device, D3D12 in sniffer and
app), adapter-list counts unchanged; Premiere 2026 CUDA initializes. NOT yet shown to fix AMD (waiting on gpu-report data).

### `neutron-zzzzzzzzzzzzzzzzzzz-winmm-keep-capture-remainder.mypatch` (2026-10-13) — mic audio dropped when capture packets are large
Found while chasing Premiere 2026's playback jumps (operator: *"the audio cuts out and jumps with the footage"*). Since
11.10-117 Premiere opens its MME input at startup and keeps it running. With `PULSE_LATENCY_MSEC=90` (set by the CLI
since 08-17) PipeWire gave the Scarlett's Mic 1 a 180 ms capture period; when Premiere's ~10 small waveIn buffers could
not take a whole 8640-frame packet, `WID_PullData` dropped the rest ("losing 7040-7936 frames", 7 in 75 s with Premiere
idle). It now keeps those frames for the next buffers the app queues (new audio waits in the 1 s device buffer);
`waveInReset` discards them, close frees them. The ACM (non-PCM) path still drops a converted packet's rest (unchanged).
Kill switch `NEUTRON_WINMM_KEEP_CAPTURE=0`. Verified: `preserved-fixes/harnesses/capture-gaps.exe mme NeutronToneSrc 20
10 20` + `PULSE_LATENCY_MSEC=180` + `tone-continuity.py WAV 997` — 11.10-119 lost audio in 6/7 runs (31-38 drops), patched
0/5 (0 drops, 0 phase jumps), kill switch 2/3; 10 ms packets identical either way. Premiere 2026 with it: 0 drops in a
33-minute session. ⚠️ It does NOT fix the playback jumps: those come from the 180 ms chunks themselves (operator, run 6
with `PULSE_LATENCY_MSEC` off: *"playback is smooth, no audio dropouts"*).

### `neutron-zzz-ps2026-complete.mypatch` (2026-07-20) — PS 2026 dev-tree delta (16 files) ⚠️ REVERTS FIVE EARLIER FIXES
Made by diffing the hand-edited dev tree against a reference build of the patch set (wiki PS2026-PERFECT.md), so it
carries the dev tree's fixes (dwrite HitTestPoint, welcome-dismiss, dock, the SetWinMonitor wiring) AND undoes every
patch-set change the dev tree never had. Found 2026-10-15 by listing the NEUTRON-commented lines it removes:
1. `server/user.h` `scale_dpi()` signed arithmetic (`neutron-server-scale-dpi-sign`): reverted. A DPI-unaware read of a
   maximized window again returns left = 25565278 (measured: LrC main frame, 168 dpi, `rectprobe`).
2. `dlls/win32u/dce.c` dark fresh-surface fill: reverted. Restored by `…-win32u-fresh-surface-dark` (2026-10-15).
3. `dlls/win32u/dce.c` BeginPaint → `neutron_break_validate()` (`neutron-win32u-wmpaint-circuitbreaker`): reverted. The
   WM_PAINT breaker's per-window count is never reset, so every window loses one legitimate paint per 2000; fires in
   Animate and Premiere logs on 124.
4. `dlls/dwmapi/dwmapi_main.c` P2c caption-button width fallback: reverted (a workaround for 1's garbage rects).
5. `dlls/win32u/window.c` NEUTRON_SHELL_FIX HSHELL_WINDOWREPLACED block: replaced by the newer "Path A" version on
   purpose (not a revert).
1 and 3 restored by `…-restore-ps2026-reverts` (2026-10-15); 4 left out (a workaround for 1).
AUDITED 2026-10-18 for reverts without a NEUTRON comment (`neutron/preserved-fixes/harnesses/revert-audit.py`: every line it
removes that an earlier patch added, every line it adds back that an earlier patch removed, in build order): 7 of 48 hunks.
Five are the items above; the other two keep the earlier code's behavior (winewayland `preferred_scale` set via a variable plus
the per-monitor DPI post; the OWL document match widened to PS 2026's `OWL.Document`). No further reverts. In the 136 source
the scale_dpi sign, breaker reset and dark fill are present; the doc-create geometry trace in this patch is env-gated off.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-dpi-walk.mypatch` (2026-10-15) — child window DPI computed once at the top-level; lock-free session block check
`get_win_monitor_dpi()` recursed up the parent chain and looked up the DPI awareness context (shared object, session
mutex) and the parent at every level, using only the top-level's. It now walks up and computes once (same result:
`harnesses/dpinest.c`, 36 values identical, 3 awareness contexts × 2 monitors). `find_shared_session_block()` checks
the last block lock-free (blocks are never changed or freed once listed). Depth 25: GetDpiForWindow 1144 → 531 ns,
MapWindowPoints 2467 → 1319 ns. LrC Select Subject (GPU on): get_win_monitor_dpi 10.4% → 5.5% of the UI thread,
find_shared_session_block 3.4% → 0.6%. Kill switches `NEUTRON_DPI_WALK=0`, `NEUTRON_SESSION_BLOCK_CACHE=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-copy-visible-bits-only.mypatch` (2026-10-15) — LrC "duplicated text lines": copy only the moved window's visible pixels
Windows copies a moved window's pixels only where it was visible before AND after the move, minus its update region,
and repaints the rest (ReactOS `co_WinPosSetWindowPos`, CopyRgn = VisAfter ∩ VisBefore). The server already invalidates
the rest, but `move_window_bits()` excluded only the window's OWN update region; for a WS_CLIPCHILDREN container the
invalidation lands in its children, so the whole old rectangle was copied, including what a sibling had just been
moved over. LrC's Masks list rows (clip-sibling containers) move down one at a time; the first row's image ran down
the list ("Mask 7" in six rows for >300 ms in frame dumps). `apply_window_pos()` now takes the visible region before
the move (server `get_visible_region`, DCX_WINDOW) and the copy is clipped to it. Harness `rowcascade.c` (Windows: 1):
nested 5 → 1, plain 1 → 1, layered 1 → 1. Kill switch `NEUTRON_COPY_VISIBLE_ONLY=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-copy-before-shape.mypatch` (2026-10-15) — no blank frame when a layered window is resized
A resized window gets a new, empty surface; `update_surface_region()` (whose `window_surface_set_shape()` flushes)
ran before `move_window_bits_surface()` copied the old contents in, so the driver committed the empty surface first.
LrC's Masks panel (layered) showed a whole-panel blank frame on every resize. Now the shape update and flush follow
the copy. Harness `resize-blank.c`: layered 6 blank frames → 0, plain 0 → 0. Kill switch `NEUTRON_COPY_BEFORE_SHAPE=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-user32-window-info-shm.mypatch` (2026-10-15) — user32 answers the basic window queries from shared memory
Windows' user32 answers IsWindow, GetParent, GetWindow, GetWindowLong and GetWindowThreadProcessId from the handle
table and desktop heap mapped into every process; here each was a system call. The server now also publishes each
window's style, ex-style, id, instance, user data and extra bytes in `window_shm_t` (next to window-tree-shm's links);
user32 maps the session section read-only and answers this process's windows from it (handle-table queries and
GetWindow links for any window). Other processes' windows for GetParent/GetWindowLong, GWLP_WNDPROC, GWLP_HWNDPARENT,
DWLP_DLGPROC, GW_ENABLEDPOPUP and every error path still go to win32u. Protocol 946 (wineserver + ntdll + user32 move
together, both arches). Per call 51-85 → 5-10 ns. `NEUTRON_WININFO_VERIFY=1` cross-checks every answer with win32u:
0 mismatches in 22 M harness calls (`harnesses/wininfo-bench.c`, 64/32-bit, cross-thread/-process, 16-bit handles,
dialogs, destroyed windows) and 1.6 M LrC calls. Did NOT shorten LrC's Select Subject repaint delay (4 paired runs):
that is ~160 ms of UI-thread work, ~2/3 LrC's own. Kill switch `NEUTRON_WININFO_SHM=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-capture-tracking.mypatch` (2026-10-15) — no WM_MOUSELEAVE to a captured window
LrC's panel scroll bars (ScrollBar class, tracked by LrC) stopped "the moment the mouse hovers outside of the scrollbar"
while dragging: get_mouse_window() (TrackMouseEvent hover/leave) used the window under the cursor and posted
WM_MOUSELEAVE to the bar that held the capture. Windows/ReactOS track the window that gets the mouse messages — the
capture window, HTCLIENT — so the leave only comes after release. 100-px thumb drag drifting 0/30/90 px sideways: moves
100/100/100 (switch off: 100/22/16); hover highlight still clears after release. Kill switch `NEUTRON_CAPTURE_TRACKING=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-noactivate-click.mypatch` (2026-10-15) — a click does not activate a WS_EX_NOACTIVATE window
"Does not become the foreground window when the user clicks it" (MSDN). process_mouse_message activated it on
MA_ACTIVATE regardless. LrC's Masks panel (NOACTIVATE) took the activation on every click; LrC then released its drag
handle's capture (~90 ms after the press) and re-activated the frame: dragging the panel "only moves a little then is
stuck". Now the panel follows a (−133,+98) drag exactly; switch off: it does not move. Kill switch
`NEUTRON_NOACTIVATE_CLICK=0`. (The existing NOACTIVATE rule covered SetActiveWindow/SetFocus only.)

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-winewayland-popup-topmost-only.mypatch` (2026-10-15) — LrC Masks panel no longer floats over other apps
Only a WS_EX_TOPMOST owned window becomes an xdg_popup (menus, dropdowns, tooltips are all topmost by the time they
show); a normal owned window is a subsurface of its owner, as an owned window stacks on Windows. The panel (ex 08080080,
not topmost) was an xdg_popup that KWin kept above every app. His check: panel stays behind other apps, menus fine.
Kill switch `NEUTRON_POPUP_TOPMOST_ONLY=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-caption-text-and-stale-redraw.mypatch` (2026-10-15) — LrC title bar one module behind
Lightroom retitles its frame with WS_VISIBLE cleared around SetWindowText (SetWindowLong off, SetWindowText, back on);
DefWindowProc(WM_SETTEXT) skipped the caption repaint of an invisible window, so the title bar showed the previous
module until the next switch. On Windows the compositor draws the caption from the text regardless. Now WM_SETTEXT
without WS_VISIBLE marks the caption stale (`WIN_CAPTION_STALE`) and WS_VISIBLE coming back (set_window_long,
set_window_style_bits) redraws it (WM_NCPAINT). The caption text is the window's own text, not a WM_GETTEXT sent to
the app, as on Windows (`harnesses/caption-cache.c`: ALPHA→BRAVO). Kill switch for the text source only:
`NEUTRON_CAPTION_TEXT_SEND=1`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-window-tree-shm.mypatch` (2026-10-15) — GetWindow and friends from shared memory ⚠️ PROTOCOL BUMP
The rest of LrC's Masking-open cost was ~220,000 wineserver requests/s, 165,000 of them get_window_tree: its UI
framework calls GetWindow ~135,000×/s (GW_CHILD/GW_HWNDNEXT) and GetDlgItem ~30,000×/s. Windows reads the tree from the
desktop heap in user mode. `window_shm_t` now carries parent/owner/next/prev/first/last child + has_region +
pixel_format, refreshed by the server on every list change; win32u answers get_window_relative, the
siblings/children NtUserBuildHwndList lists, the no-region get_window_region and the plain surface clip from it, and
apply_window_pos no longer computes an owner hint (a WindowFromPoint request) for child windows. Masking open 0.9 / 0.8 s
(kill switch 2.5 s; 13.5 s before today). `NEUTRON_TREE_VERIFY=1` cross-checks every shared answer with the server
(0 mismatches). Kill switch `NEUTRON_TREE_SHM=0`. **Protocol version bumped — wineserver, ntdll and win32u of BOTH
arches must come from one build.**

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-restore-ps2026-reverts.mypatch` (2026-10-15) — re-apply scale_dpi sign + breaker reset
Re-applies the two fixes `neutron-zzz-ps2026-complete` reverted: `server/user.h` scale_dpi signed arithmetic (unaware
GetWindowRect of LrC's maximized frame: left 25565278 → −3) and BeginPaint → `neutron_break_validate()` (`harnesses/
paint-breaker.c`: 2 of 4500 paints drained → 0). Not restored: dwmapi P2c (only worked around the garbage rects).

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-winewayland-zorder-reuse.mypatch` (2026-10-15) — Lightroom Masking lag, part 1
"Significant lag when I click on masking": the first Masking open of a session builds ~90 windows and took 13.5–14.5 s
(nested KWin, clean-room LrC 15.6, runtime 124). The subsurface restack ran ~800×/s and changed nothing; 48% of the UI
thread was its phase-A walk of the Win32 tree (a GetProp by name, a monitor-dpi lookup + GetWindowRect and two
GetWindowRelative per window, each a wineserver round trip). A process-wide `neutron_zorder_generation`, bumped on
every change the mirror follows (z-order move, show/hide, destroy, style, subsurface role, client wl_subsurface), lets
`neutron_zorder_get()` reuse the last walk per toplevel; rects are read only for the opt-in temporal rule, the CEF-host
property only for client-surface windows (re-read on every reuse). Phase B unchanged. 5.1 / 4.5 s. Kill switch
`NEUTRON_ZORDER_REUSE=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-winewayland-roleless-flush.mypatch` (2026-10-15) — idle CPU: role-less windows reflushed forever
The role gate (2026-09-01, xdg-shell: no buffer before the role) refused a role-less window's flush by FAILING it; win32u
keeps the bounds dirty and retries on every message-loop pass, forever for a window never shown. Lightroom's four hidden
"Shadow" windows (layered, per-pixel alpha) were reflushed ~460×/s each and held its UI thread at 45% CPU at idle (90%
in `window_surface_flush`, the alpha-shape pass). The gate fired in 63 of the operator's 65 recent logs. Now the flush
is consumed, `wayland_window_surface_note_roleless()` records the unpublished contents, and the role assignment requests
an expose of the whole surface (same moment, same pixels as the retry). Only never-shown windows are role-less (a hide
keeps the role). LrC idle 45% → 8.6%. `harnesses/roleless-show.c` (per-monitor aware; a DPI-unaware window goes through
win32u's scaled wrapper, whose flush always succeeds, and never retried): never-shown + kill switch 1570 ms CPU / 3 s,
fixed 50 ms; contents correct on show in all four cases. Kill switch `NEUTRON_ROLELESS_CONSUME=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-winewayland-zorder-scan-reuse.mypatch` (2026-10-15) — Lightroom Masking lag, part 2; `NEUTRON_ZORDER_VERIFY`
Phase B's two whole-tree scans (`wayland_client_subsurfaces_of`, `wayland_owned_subsurfaces_of`) were then the largest
restack cost: 34% of LrC's UI thread after "Select Subject". Reused per (kind, parent, generation); every way a window
JOINS a set bumps the generation after the change is in place (the `make_subsurface` bump moved below its field
assignments; `set_client_surface` added). Both lists only propose windows; restack_apply re-validates live, so a stale
extra is dropped. `NEUTRON_ZORDER_VERIFY=1` (test switch, off) recomputes every reused walk and scan and logs
`neutron-zverify: STALE`: 0 through Masking, Select Subject, close/reopen, photo switching, Library↔Develop. Masking open
4.1 / 4.1 s; Select Subject busy period 2.5 → 1.5 s. Off with `NEUTRON_ZORDER_REUSE=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-fresh-surface-dark.mypatch` (2026-10-15) — Lightroom "empty white boxes"; restores a lost fix
The white boxes in the Masks panel during Select Subject are the fresh-surface fill: the flyout grows by a row while the
UI thread is busy, win32u copies the old contents into the larger surface, and the rest stays the fill until the app
paints (a diagnostic magenta fill turned the boxes magenta). The fill is now opaque dark grey (0xff202020; 0x20 bytes
below 32 bpp). This restores "initialise a fresh surface to a dark neutral", which `neutron-zzz-ps2026-complete`
reverted by accident on 2026-07-20 (see its entry above). No kill switch (a fill colour).

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzz-opencl-command-queue-with-properties.mypatch` (2026-10-15) — OpenCL 2.0 queue export (GitHub Neutron#18)
Premiere 2026 crashed at startup on AMD with ROCm's OpenCL 2.1: its GPU check (GPUSniffer) created a context and died
with c06d007f (delay-load procedure not found). Wine's OpenCL.dll exported only the 1.0–1.2 API; Windows' (the Khronos
loader) exports every version, and `clCreateCommandQueueWithProperties` is the only OpenCL function Premiere, After
Effects, Media Encoder, Photoshop, Audition or Lightroom Classic import that Wine lacked. `make_opencl` now emits it
(`%extra_newer_functions`, with `cl_properties`/`cl_queue_properties`); regenerated from the pinned registry (the
unmodified generator reproduces the current files exactly). No kill switch: an added export changes nothing for code
that doesn't call it. NVIDIA: `opencl-probe` creates its queue through it and runs a kernel correctly; GPUSniffer and
Photoshop's sniffer unchanged. AMD/ROCm (reporter, RX 6750 XT, 10-15): crash gone; GPUSniffer lists `gfx1030`
(Vendor AMD, 12272 MB) as a GPU-computation device, and Premiere's Renderer reads "Mercury Playback Engine GPU
Acceleration (OpenCL)".

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzz-mixed-dpi-maximize.mypatch` (2026-10-14) — maximize on a non-primary monitor at mixed DPI
Illustrator maximized on a monitor that was not Wine's primary, on a desktop whose monitors differ in DPI, came out
portrait-width with its configure rejected or too tall with its menu bar above the screen (240+ rejections). Three
causes, three kill switches: (1) win32u `monitor_from_rect()` (upstream) compared a per-monitor-aware caller's RAW
rect with monitors at the SYSTEM dpi, so the landscape monitor's origin resolved to the portrait monitor and the
#20 follow-output never moved the window — now raw vs raw (`NEUTRON_MONITOR_RAW_LOOKUP=0`); (2) follow-output
converted the compositor's size at the scale of the monitor being left (823x1463 → 1646x2926) — now at the output's
own scale (`NEUTRON_FOLLOW_SCALE=0`); (3) the maximized work-area measurement was one value for every output — now
per output name, persisted in the prefix file after the legacy first line (`NEUTRON_WORKAREA_PER_OUTPUT=0`).
Nested KWin, #20 layout at 175%/200%: every case clean once the output has a measurement; the first maximize ever on
a panel-less output can still come out misplaced in that session. His desk: the 4K unchanged.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzz-winewayland-kde-primary.mypatch` (2026-10-14) — Wine's primary monitor = the desktop's primary (KDE)
Wine made the output at the layout's origin primary; Windows' primary is the one the user chose. On KDE the desktop
process now reads `kde_output_order_v1` (first output = primary) on a throwaway connection, because KDE marks the
protocol internal and an unknown event would kill the connection it arrives on; any failure keeps the old rule. The
existing translation puts the primary at (0,0), others possibly negative. App processes do not ask KDE: they take the
primary whose arrangement matches win32u's monitors, so a change of primary mid-session (KWin sends clients nothing
for it) cannot split them from the monitor list; it takes effect when the prefix's Wine restarts. Kill switch
`NEUTRON_KDE_PRIMARY=0`. Verified on a nested KWin: #20 layout → DISPLAY1 = KDE's landscape primary and Illustrator
maximizes full size there (=0: the #20 portrait-width window at mixed scales); the operator's desk unchanged.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzz-winewayland-maximized-crop.mypatch` (2026-10-14) — a maximized window's frame on the next monitor
Operator: *"I'm seeing a sliver of app on my right monitor again."* A maximized window's rect overhangs its work area
by the frame (as on Windows); the surface carried the overhang and xdg geometry does not clip, so KWin drew the frame
on the neighboring monitor (5 px from Audition and Lightroom, 3 px leftward from a window maximized on the right-hand
monitor). The frame-spill crop stopped once the maximized configure was acked, so any app that repainted got it back.
Windows (DWM) keeps the rect but does not draw the overhanging border: a maximized toplevel now shows only its
work-area part plus its client area. Owned windows larger than their monitor (Premiere's "DroverLord" main UI, a
subsurface, 4 px) get the same crop. Popups of a cropped window are placed from its surface-local origin (Audition's
File menu was 4 px off its menu-bar item). Kill switch `NEUTRON_MAX_CROP=0`. Verified on a nested KWin laid out like
the operator's desk with clean-room Audition, Lightroom Classic, Premiere, InDesign and harnesses/maxsliver.c.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzz-winewayland-follow-output.mypatch` (2026-10-14) — a maximized window sized for the wrong monitor
Reporter (Neutron#20, portrait screen at the layout origin, KDE primary = the landscape one): *"it takes the 1080x1920
size for the maximized window"*. On Wayland the compositor picks the output; win32u kept its own idea (Wine's primary,
the portrait screen), Illustrator maximized for it, and KWin showed the window on the landscape output at portrait size.
`wl_surface.enter/leave` now track a surface's outputs. A maximized or fullscreen toplevel on exactly one output whose
win32u monitor differs is moved onto that output's monitor (frame overhang kept, or the compositor's size at the work
area origin), with SWP_FRAMECHANGED so an app that computes its own client area recomputes it. Kill switch
`NEUTRON_FOLLOW_OUTPUT=0`. Verified on a nested KWin with clean-room Illustrator: 1440 px wide before, 2560x1440 after.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzz-mountmgr-removable-media-type.mypatch` (2026-10-14) — SD cards and USB sticks are removable drives
udisks flash/thumb (or removable) media came out DEVICE_UNKNOWN, i.e. DRIVE_FIXED; Windows says DRIVE_REMOVABLE.
Lightroom Classic listed a card under Files instead of Devices and did not select it in Import. Now DEVICE_HARDDISK
(DRIVE_REMOVABLE), as the macOS backend maps removable media; a USB hard disk stays fixed. Kill switch
`NEUTRON_MOUNT_REMOVABLE_TYPE=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzz-mountmgr-announce-on-mount.mypatch` (2026-10-14) — Lightroom Classic opened Import before the card was mounted
Reporter (Neutron#15): *"Lightroom pops up the import dialog. However, the card hasn't actually been mounted yet"*. Every
udisks add/PropertiesChanged was broadcast as DBT_DEVICEARRIVAL (6 arrivals for one card in the mock, one on UNMOUNT).
Now arrival when the drive gets a mount point and removal when it loses one. Kill switch `NEUTRON_MOUNT_NOTIFY_MOUNTED=0`,
read on the Unix side (services.exe gives winedevice a registry-built Windows environment). Harness
`preserved-fixes/harnesses/udisks2-mock/`. Operator, real SD card: passed.

### `neutron-zzzzzzzzzzzzzzzzzzzzzz-winewayland-deact-end-menu.mypatch` (2026-10-14) — dropdowns stay open after switching apps
Operator: *"the dropdowns are also persisting on the top of everything when switching between apps"*. The focus-leave
handler's WM_CANCELMODE only ends a menu owned by the foreground window; Lightroom Classic's panel dropdowns are popup
menus owned by a ComboBox child. Now NtUserEndMenu() when focus leaves the process and the thread is in menu mode.
Kill switch `NEUTRON_DEACT_ENDMENU=0`. Operator check (clean-room Lightroom Classic): passed.

### `neutron-zzzzzzzzzzzzzzzzzzzzz-winewayland-output-transform.mypatch` (2026-10-14) — rotated (portrait) monitors reported landscape at the wrong dpi
Reporter (Neutron#20, AMD, KDE): Illustrator 2026's menus are drawn at about half size inside an oversized white pop-up;
their second screen is a 1080x1920 portrait at (0,0). `wl_output.mode` is in the panel's hardware orientation, before the
output transform, and winewayland never read the transform (upstream 11.10 doesn't either): the portrait screen became a
1920x1080 monitor, our per-monitor dpi made it 96 x 1920/1080 = 171 dpi, and its rect pushed the other monitor sideways.
The transform from `wl_output.geometry` is now kept per output and every mode size that reaches win32u goes through
`wayland_output_mode_width/height()` (arrangement, monitor rect, dpi, work-area inset, mode list); modes carry
dmDisplayOrientation DMDO_90/180/270 as winex11 maps RandR rotations. Unrotated outputs are unchanged. Kill switch
`NEUTRON_OUTPUT_TRANSFORM=0`. Verified on a nested KWin with one rotated output
(`preserved-fixes/harnesses/kwin-nested/start.sh`): `monitors.exe pm` went from DISPLAY1 (0,0)-(2560,1440) dpi 171 +
DISPLAY2 at (2560,326) to (0,0)-(1440,2560) dpi 96 + (1440,581) dpi 96 (the compositor's layout); clean-room Illustrator
2026's File and Edit menus went from tiny text in a white box to normal size under the menu bar, at Wine 100% and 150%.

### `neutron-zzzzzzzzzzzzzzzzzz-winewayland-crop-origin.mypatch` (2026-10-13) — Premiere Program Monitor black line; pointer 5 px off in maximized Premiere
Operator: *"the black line is back on the left side of the program monitor"* (present on shipped 117 too). A maximized
Premiere keeps the frame-inflated window rect (-5,51) 3850x2114 (since 11.10-78's maxkeep), so the frame-spill crop
shows it from window x 5 (`neutron-overhang: full=3850x2114 clipped=3840x2109`). Client subsurfaces were positioned
from the window origin: the Program Monitor's layer at logical 965 instead of 962, leaving its first 5 columns
unpainted (screen cols 1684-1688 black) and covering 5 past its right edge. Same math for input: pointer motion landed 5
px left of the pointer. The crop origin applied in `wayland_surface_attach_shm` is now stored (`content_x/y`) and
`wayland_surface_content_origin()` is used by client and owned subsurface positions, pointer motion, confine and warp,
tablet and Wintab positions, and drag-and-drop. xdg_popup anchoring already accounted for it (maxgeom fix #2) and is
unchanged. Kill switch `NEUTRON_CROP_ORIGIN=0`. Verified: `preserved-fixes/harnesses/ptrcheck.c 180 overhang0`
(sized like maximized Premiere, crop on) with pointer screenshots — pointer tip minus crosshair dx 8 → 3 (exactly the
5-px crop; the rest is the arrow sprite); `clipped-gpu-child.c overhang` phase 6 spill past the container 6 px → within
the touching allowance. Operator, clean room Premiere 2026: *"black line is gone, clicks and tooltips line up.
playback is the same"*.

### `neutron-zzzz-winewayland-orphan-toplevel.mypatch` (2026-08-03) — After Effects splash screen
A `wl_subsurface` only displays when its PARENT surface is mapped. An owned window whose owner has
no `xdg_surface` yet became a subsurface of something not on screen, so it was never displayed at
all. AE shows its splash (an owned `#32770`, 1301x877) ~22,000 log lines before the main window
that owns it is mapped (`SHOW @ line 940` vs `configure_window hwnd=0x20064 @ line 22771`).
Promotes such a window to its own toplevel — but **only the outermost orphan**: the owner must be a
genuine root with no owner of its own. Without that test, a 503x764 dialog owned by the splash was
promoted too and appeared as a **ghost window behind it** (the failure the surrounding code's own
comment warns about). Ownership *depth* separates them; owner *visibility* does not, because both
are seen while their owner is still unmapped. Opt-in and off by default: `NEUTRON_WL_ORPHAN_TOPLEVEL=1`
turns it on, and only the After Effects launch profile sets it (on for every app, it ghosted Premiere's
splash).
⚠️ Named `zzzz-` deliberately: it must collate AFTER the four other patches touching
`dlls/winewayland.drv/window.c`, or both hunks fail.

## Diagnostic (`patches/diagnostic/`) — built only with `INCLUDE_DIAGNOSTIC=1`

### `neutron-present-timing.mypatch`
Present-cadence + blit-duration timing probe behind `NEUTRON_PRESENT_DEBUG`. Used to measure judder /
A-B pacing strategies.

### `neutron-uxp-present-probe.mypatch`
Reads UXP surface pixels before the present blit (diagnostic for the home-screen render investigation).

### `neutron-winewayland-dl-instrument.mypatch`
Temporary `ERR("NEUTRON-DL …")` markers in `winewayland.drv` (`window.c`, `wayland_surface.c`) tracing
`WindowPosChanging`/`WindowPosChanged` for the Photoshop `win_data_mutex` deadlock investigation.
Self-labeled **NOT for production** — re-apply only to re-instrument.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-winewayland-xrgb-opaque.mypatch` (2026-10-16) — every Wine window see-through where GDI drew, after Plasma 6.8
A plain (non-layered, unshaped) window's SHM buffer is XRGB8888, whose top byte is undefined; GDI leaves it 0 in
everything it paints (fills, text, blits), and winewayland copied it through unchanged. After the 2026-10-15 upgrade
to KDE Plasma 6.8.0 (KWin 6.8.0) that byte is composited as alpha: Lightroom Classic showed black panels, hollow
outlined text and no menu bar; Notepad, Photoshop and a bare GDI test window did not appear at all on the nested
desk. Every runtime back to 11.10-104 showed it, so not a code regression of ours; FreeType 2.15 (same upgrade) was
ruled out by running 2.14.3. The plain path now ORs 0xff000000 into every copied pixel, as the shaped path already
did. A/B on the same build, LrC on the nested KWin 6.8 desk: fixed (menu bar, identity plate, gray panels, solid
text) vs `NEUTRON_XRGB_OPAQUE=0` (all three symptoms back). Kill switch `NEUTRON_XRGB_OPAQUE=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-client-surface-subtree.mypatch` (2026-10-16) — LrC Masks panel boxes: update only the moved window's client surfaces
After every SetWindowPos win32u's `update_client_surfaces(toplevel)` updated EVERY client surface of the toplevel, and
winewayland's update walks each surface's ancestors (`neutron_ancestor_clip`, map_window_points per level). LrC's Masks
panel moves its rows one SetWindowPos at a time, so each row move redid every row's GPU thumbnail surface; during
Select Subject that kept the panel's grown area unpainted ~100-290 ms. A move that keeps z-order and visibility
(`SWP_NOZORDER`, no SHOW/HIDE) can only change `hwnd`'s own subtree, as on Windows, so only those surfaces are updated
now (z-order/visibility changes and the toplevel itself still update all). Nested KWin 6.8, clean-room LrC, GPU on,
4 Select Subject runs per arm from an empty panel: long resize->first-paint gaps 769 -> 531 ms total, UI-thread CPU
~1830 -> ~1510 ms per run; `wayland_client_surface_update` 50% -> 0.8% of Wine's samples in the gaps. Panel renders
correctly. Kill switch `NEUTRON_CLIENT_SURFACE_SUBTREE=0`.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-resize-hold.mypatch` (2026-10-16) — LrC Masks panel boxes: a grown window waits until it has painted
When a visible top-level window grows onto a new surface, the old pixels are copied in but the new area is empty
until the app paints, and publishing at once showed it as a box: LrC's Masks panel grows during Select Subject and
lays its rows out for ~100-200 ms before painting. Windows' compositor keeps showing a resized window's previous
frame until it redraws (INFERRED); now `window_surface_flush` holds such a surface (bounds stay dirty) until the
server reports nothing in the window's tree needs painting (`get_update_region` UPDATE_PAINT|UPDATE_ALLCHILDREN),
at most 250 ms. Frame dumps, 2 Select Subject runs per arm: hold off = grown frames with an unpainted area reached
the compositor; hold on = 4 holds released "painted" after 134-249 ms, first published frame fully painted. His
check on the clean-room LrC: *"The box is gone now"*. Not covered: a window's first show; frames while rows move.
Kill switch `NEUTRON_RESIZE_HOLD=0`; `NEUTRON_RESIZE_HOLD_TRACE=1` logs each hold.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-wic-seed-skip-stale-busy-generation.mypatch` (2026-10-16) — Premiere ruler numbers pile up after zooming (Display Format = Frames)
His report (10-13): the timeline ruler *"numbers pile up ... It happens after zooming the timeline"* with the sequence's
Display Format = Frames. Each zoom step left the previous step's numbers on the ruler, fading under the new ones, and
the ruler got lighter. Cause, ours: windowscodecs `BitmapImpl_Create` mallocs the bitmap and never set the v3 counters
(`write_gen`, `write_locks_held`), so a bitmap could start with a garbage nonzero lock count and answer ~0 ("write lock
outstanding") for its whole life; d2d1's seed skip (`…zzzzzzzzzze`) compared that ~0 with the ~0 it recorded last time,
took it for "untouched", and published its stale texture over the content Premiere had written through its own lock.
Fix: windowscodecs sets both counters at creation and draws generations from one process-wide counter (a freed
bitmap's reused address can no longer match a recorded generation); the write-lock bookkeeping runs only once the
lock is granted (a refused lock used to count as held forever); d2d1 never compares or records ~0.
MEASURED, clean-room Premiere 2026, Hideout_1 copy, 8 zoom-ins then 9 zoom-outs, ruler captured each step:
131 = pile-up 2/2 runs; `NEUTRON_WIC_SEED_SKIP=0` = clean; fix with the skip on = clean (3 runs). Log on 131: the
ruler's 1775x102 bitmap answered ~0 from its first acquisition with no lock ever refused; 209 of 210 acquisitions
skipped. Disproved on the way: primitive blend (Premiere never calls SetPrimitiveBlend with a non-default mode).
⚠️ Cost: the skip now fires 0% in Premiere (131: 62.8% in the same zoom test, all on the false ~0 match) because
Premiere write-locks these bitmaps between our draws, so the upload is genuinely needed. The 2026-08-24 same-session
A/B credited the skip with 22 -> 24 fps while scrolling; that gain came from this bug. Kill switch for the skip is
unchanged (`NEUTRON_WIC_SEED_SKIP=0`). His check: pending.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-advapi32-transacted-registry.mypatch` (2026-10-17) — Adobe CoreSync helper crash during updates
During his Photoshop 27.8 → 27.11 update, Wine's crash dialog appeared for `CoreSyncCustomHook.exe`. Adobe's installer runs the
OLD CoreSync 7.8 helper with `--uninstall=… --mode=update`; it calls `RegOpenKeyTransactedW` at once, which Wine did not export
at all ("Call from 7BF91BD8 to unimplemented function ADVAPI32.dll.RegOpenKeyTransactedW, aborting"). The new 7.10 helper's
install pass also failed every `RegCreateKeyTransactedW` with error 120 (a stub). Wine has no transaction manager:
`NtCreateTransaction` hands out a placeholder handle and `NtCommitTransaction` succeeds, i.e. transactions already behave as
"commit immediately". Now `RegCreate/Open/DeleteKeyTransacted{A,W}` do the same: the plain operation (+ winreg.h prototypes).
Known difference: changes apply at once, not at commit; a rollback cannot undo them (NtRollbackTransaction is unimplemented).
MEASURED (lab, copy of the pre-update prefix, scratch runtime = 132 + this advapi32, test desktop): the 7.8 CoreSync and
CoreSyncExtension uninstall passes went from `EXCEPTION_WINE_STUB`, exit 1, to exit 0 with no stub exception; the helper went on
to move and mark its DLLs as Adobe intends. NOT BUILT yet (his call: hold until the sign-in test finishes).

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-shell32-imagefactory-thumbnailonly.mypatch` (2026-10-17) — LrC Import: blank thumbnails for a card under Devices (Neutron#26)
Reporter: a card under **Devices** "shows the expected number of files but all the thumbnails are blank"; the same drive
under **Files** shows them. For a device source Lightroom Classic asks the shell for each photo's thumbnail with
`IShellItemImageFactory::GetImage({240,240}, SIIGBF_BIGGERSIZEOK|SIIGBF_THUMBNAILONLY)` (8 calls for 8 photos). Wine's
GetImage was a stub that returned S_OK with a blank 1x1 DIB, so Lightroom took the blank as the thumbnail. Windows fails a
thumbnail-only request when no handler produces a thumbnail; Wine has no thumbnail handlers, so it now returns
`WTS_E_FAILEDEXTRACTION` and no bitmap, and Lightroom falls back to reading the photo itself. Other flags unchanged (still
the blank DIB; an icon fallback would be the next step). Kill switch `NEUTRON_SHELL_THUMBONLY_FAIL=0` (PE-side getenv).
MEASURED (lab prefix on runtime 130 + this shell32 only, udisks2 mock card with 8 a6700 ARWs, test desktop): Devices
source shows real thumbnails; `=0` in the same setup brings back the white squares. His real-card check: PASSED (fix on: thumbnails; `=0`: white squares). SHIPPED in 11.10-134.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-ntdll-hang-report.mypatch` (2026-10-17) — hang report on the first critical-section timeout (Neutron#27, #19)
A timed-out critical section wait logged THAT a process was stuck ("blocked by 01b8"), never WHERE. Now, on the first
timeout in a process (5 s), `RtlpWaitForCriticalSection` also logs one `neutron-hang:` line per thread: thread name, where
it executes, and return addresses on its stack as `module+offset`, OWNER and WAITER marked. Per thread: NtOpenThread →
NtSuspendThread → NtGetContextThread + TEB StackBase → NtReadVirtualMemory (32 KB from SP) → NtResumeThread; analysis after
the resume. Frames = stack scan (value inside an executable MEM_IMAGE region AND preceded by a call instruction), module
names via MemoryMappedFilenameInformation, ranges cached (256). No heap / loader / PEB lock while a thread is suspended;
the gate read skips the PEB lock if that is the contended section. Once per process (InterlockedCompareExchange). Kill switch
`NEUTRON_HANG_REPORT=0`.
MEASURED (lab runtime = 134 + this ntdll, x86_64 + i386): `deadlock.exe` (`/var/tmp/neutron-hang/`) — OWNER line
`deadlock-x86_64.exe+0x1596` (holder_waits_forever) `+0x160e` (holder), WAITER `+0x15ad` (main_waits_for_section) `+0x16e1`
(main); i386 likewise (+0x1579/+0x1609, +0x1590/+0x1707). 202 threads reported in 89 ms; second timeout at 60 s → no second
report; `=0` → no report. Lightroom Classic normal session on it: 0 report lines, runs normally. SHIPPED in 11.10-135.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-winewayland-owner-hint-strict.mypatch` (2026-10-18) — PS main window drawn inside its own Open dialog (Neutron#28)
Reporter (Photoshop 2025 26.10, KDE Wayland, AMD, 11.10-135, launched from Collider): "The open file dialog isn't opened
correctly". Screenshot: the dialog's title bar, and under it the whole Photoshop window starting at the dialog's client origin
and running off the screen. The owned-subsurface rule (`zzzzzzzzzzm`) falls back to win32u's `owner_hint` when a managed window
has no GW_OWNER. For an unowned window win32u computes that hint as `NtUserWindowFromPoint(left - 1, top - 1)`, so a modal
dialog covering the main window's top-left corner became the main window's subsurface parent. Now the hint is used only for
windows the Windows taskbar would not list (`WS_EX_TOOLWINDOW` without `WS_EX_APPWINDOW`; the Button Editor the rule was written
for is NOACTIVATE|TOOLWINDOW|TOPMOST), and never when the window owns the hinted window. Logs `neutron-ownerhint:` when it
refuses. Kill switch `NEUTRON_OWNER_HINT_STRICT=0`.
MEASURED (`neutron/preserved-fixes/harnesses/ownerhint-dialog.c`, lab prefix `/var/tmp/neutron-i28/pfx`, nested KWin 1920x1080):
on 135 the main window logs `neutron-ownedsub ... SUBSURFACE of owner=<dialog>`, shows inside the dialog and is gone after it
closes (3/3, incl. `=0` on the fix); on 135 + this winewayland.so it stays in place and returns after the dialog (2/2, one
with a forced SetWindowPos). `tool` control (unowned Button Editor-style popup): still `neutron-ownedsub ... owner=<main>` on
both. With `NEUTRON_OWNED_SUBSURFACE=0` on 135 the main window also stays in place. Reporter check: pending. SHIPPED in 11.10-136
(packaged winewayland.so == the lab-tested build).

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-owner-drawn-menu-readable-selected.mypatch` (2026-10-18) — InDesign: hovered font row's text black
Operator, on InDesign run as Windows 10 (CLI c888fdd, so its font menu fills the hovered row with COLOR_MENUHILIGHT): "Why does
the highlighted text become black?" The readable-text rule (`zzzzzzzzzzzzzs`) only acts when half the row is exactly COLOR_MENU,
so the hovered row kept InDesign's black text on 64 gray. The selected item may now instead be mostly COLOR_MENUHILIGHT, mapped
to COLOR_HIGHLIGHTTEXT, under the same three conditions; rows that are mostly COLOR_MENU are unchanged; Windows' light palette
(blue highlight) never triggers it.
MEASURED: `odmenu-text.c` row 4 (selected, COLOR_MENUHILIGHT fill, black text): dark palette 136 FAIL (darkest 0), patched PASS
(64..255), rows 1-3 unchanged; light palette passes both. InDesign (lab LrcI19, Windows 10 mode, 175%): hovered row background
64, text to 255, nothing darker than 64. Lab-tested as a win32u.so swap on 136 (`/var/tmp/neutron-i19/rt136hl2`). SHIPPED in 11.10-137 (packaged x86_64 win32u.so
code == the lab-tested module).

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-menu-arrow-glyph.mypatch` (2026-10-18) — submenu arrow drawn in a blue box
Seen on the same hovered InDesign row: the submenu arrow sat in a box of COLOR_HIGHLIGHT (48,150,250), because `draw_popup_arrow`
SRCCOPYs the monochrome arrow with the DC's preset background. Now blitted as a mask (PSDPxax, brush = DC text color), so only
the glyph is drawn, as on Windows.
MEASURED: `odmenu-text.c` row 4 with a submenu: dark palette 136 FAIL (52 COLOR_HIGHLIGHT pixels around the glyph), patched PASS
(0); light palette passes both. InDesign: no blue around the hovered row's arrow; the Type menu (Wine-drawn items) is
pixel-identical to 136 apart from the submenu's edge. SHIPPED in 11.10-137.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-adapter-memory-cap.mypatch` (2026-10-19) — InDesign GPU Performance "not detected", 185MB
Found in the InDesign smoke test (10-18): Preferences > GPU Performance greyed out, "Compatible GPU and Monitor not detected",
Total Memory 185MB on a 12 GB card. `add_gpu` wrote `HardwareInformation.MemorySize` as `(ULONG)min(size, ULONG_MAX)`; in host
code ULONG_MAX is 2^64-1, so the cast truncated (12 GB -> 185 MB; multiples of 4 GB -> 0). WMI AdapterRAM reads it; Adobe's
AISniffer reads AdapterRAM; InDesign requires > 1000 MB (Layout UI GPU status check, decompiled). Capped at MAXDWORD.
MEASURED: `wmi-adapterram.c` 137 FAIL (185 MB), patched PASS (4095 MB). InDesign on the patched win32u.so: GPU detected, GPU
Preview renders like CPU (zoom levels, placed PNG), clean quit. Illustrator 2026 unchanged (still blocked: dxdiagn DirectX 9.0).
Lab-tested as a win32u.so swap on 137 (`/var/tmp/neutron-i19/rt137gpu`). SHIPPED in 11.10-138 (harness PASS on the package; FAIL on 137).

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-comdlg32-itemdlg-wrapped-label-height.mypatch` (2026-10-19) — InDesign Save As / Export labels clipped
InDesign Save As: "Always Save Preview Images with Documents" cut onto two clipped lines; Export: only "Name as the Output" of
"Use InDesign Document Name as the Output Filename" visible. IFileDialogCustomize controls have a fixed width; `ctrl_resize`
sized the height from '\n' count only (upstream FIXME). Now measures the wrapped label (DT_WORDBREAK, in the button's text room).
MEASURED: `fd-checkbox.c` 137 FAIL (needs 46, has 31), patched PASS (54; short label stays 31). InDesign: Save As 2 lines, Export
3 lines, Place unchanged. Lab-tested as a comdlg32.dll swap (`/var/tmp/neutron-i19/rt138pre`). SHIPPED in 11.10-138 (harness PASS on the package; FAIL on 137).

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-second-screen-dpi.mypatch` (2026-10-20) — Photoshop on a 100% screen next to a 175% one
Operator, Photoshop 2026 on his Cintiq 22HD (100%) beside a 4K at 175%: menus drew their items at 57% of their box ("render too
small within a box"), tool icons never highlighted on hover, and an open menu did not follow the cursor along the menu bar;
all fine on the 175% screen. Two Wine bugs for per-monitor-aware windows on a second screen with another scale:
(1) `create_window_surface` took `get_dpi_for_window()`, which reads the monitor of the window's CURRENT rect; a menu created on
the primary and shown on the other screen in one SetWindowPos got a 168->96 `scaled_surface`. `get_window_surface` now passes
the dpi of the rect being applied for monitor-aware top-levels (kill switch `NEUTRON_SURFACE_TARGET_DPI=0`).
(2) `window_from_point` sent the server the desktop's (primary's) dpi with a per-monitor caller's raw point; the server rescaled
it and WindowFromPoint(GetCursorPos) returned the desktop. win32u now sends dpi 0 for that case and the server
(`all_windows_from_point`) tests each top-level at its own monitor's dpi, as `shallow_window_from_point` does for input
(kill switch `NEUTRON_POINT_MONITOR_DPI=0`). Touches server/window.c: wineserver changes too.
MEASURED (nested KWin, 175% + 100%): `menupm.c` items fill 56% of the box on 138, 98% patched; `mousepos.c`
WindowFromPoint wrong 27/27 on 138, 0/27 patched. Operator, lab copy on his Cintiq (with NEUTRON_DPI_34=1): "menus are readable
now", then "all three work now, menus and drags still good". Lab-tested as win32u.so x2 + wineserver swapped into 138
(`/var/tmp/neutron-i19/rt138std`). SHIPPED in 11.10-139 (package: menupm 98% vs 56% on 138; mousepos 0 mismatches vs all on 138).

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-dpichanged-on-by-default.mypatch` (2026-10-20) — WM_DPICHANGED on a cross-monitor move, by default
The sender (`NEUTRON_DPI_34`, in `neutron-zzz-ps2026-complete`) was off since the 124 work, where it had destabilized Photoshop's
floating windows. Without it Photoshop on a 100% screen next to a 175% one kept the 175% layout of its floating options bar at
the bottom of the screen (947x56 instead of 541x32). Windows sends WM_DPICHANGED here; his call: on for every app. Kill switch
`NEUTRON_DPI_34=0`.
MEASURED: operator, lab copy on his Cintiq with the sender on: bar normal size, drags between the screens floating and maximized
clean. Nested desk with no env set: the bar ("Start Bar" state) got WM_DPICHANGED 168->96 and is 459x32 (32 = 100% height).
SHIPPED in 11.10-139.

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-imapi2-disc-master.mypatch` (2026-10-20) — Lightroom Classic crash on Export To: CD/DVD (Neutron#30)
Reporter: *"Crash on export creating user preset"* (log: `class {2735412e-…} not registered`, then a NULL read). Lightroom's
DiscBurning.dll creates MsftDiscMaster2 and uses it unchecked; Wine had no imapi2. New `dlls/imapi2` (new DLL: the build's
`tools/make_makefiles` + `autoreconf` pick it up; wineboot registers the class from `imapi2_classes.idl`): MsftDiscMaster2 =
supported environment, 0 recorders, empty enumeration. Recording is not implemented.
MEASURED: `imapi2-master.c` 139 FAIL (0x80040154), patched PASS. Lightroom Classic 15.6 lab: CD/DVD export shows its own "cannot
locate a drive that supports burning" message instead of Adobe's error report. Lab-tested as imapi2.dll (x86_64) added to 139
(`/var/tmp/neutron-i19/rt139imapi`) + `wineboot -u`. SHIPPED in 11.10-140 (package: imapi2-master PASS after `wineboot -u`).

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-create-on-second-screen.mypatch` (2026-10-20) — windows created on a 100% screen beside a 175% one
Found reproducing #30: Lightroom Classic's Export dialog was invisible on a 100% second screen, placed at (15626,2462), past the
desktop's right edge. NtUserCreateWindowEx took get_dpi_for_window() of a window with no rect yet (= the primary's dpi); the
requested rect was scaled 96->168 and read back at 168 (x 3.0625). Now the dpi of the requested rect's monitor
(`NEUTRON_CREATE_TARGET_DPI=0`). Also: no WM_DPICHANGED for a never-sized window's first placement (`NEUTRON_DPICHG_SKIP_UNPLACED=0`).
MEASURED: `createpos.c` 139 6/6 FAIL, patched 6/6 PASS, kill switch FAIL; primary-monitor windows 6/6 PASS both. Lightroom: the
dialog appears (nested desk; his Cintiq). NOT fixed by it: Lightroom's UI at 175% on the 100% screen (also on 139) and a stale copy
of the Export dialog's settings panel. Lab runtime `/var/tmp/neutron-i19/rt140pre`. SHIPPED in 11.10-140 (package: createpos 6/6 on both screens).

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-win32u-winewayland-mixed-dpi-screens.mypatch` (2026-10-21) — windows on, and moved between, screens of different scale
His words: *"The UI scale is totally messed up and it's cutting off the export dialog"* (Lightroom Classic on the 100% Cintiq),
*"It launched on my main monitor at 100% scale instead of the 175% it should be"*, *"every other fucking app has no problem
with scaling between the two screens"*. Lightroom reads its scale once at launch and ignores WM_DPICHANGED (Fable trace,
`~/neutron/preserved-fixes/lrc-scale-investigation-2026-10-21.md`); Windows handles it with DPI virtualization (the "System"
high-DPI override). Five fixes, each with its own kill switch:
- `NEUTRON_SURFACE_CROP_DPI` (win32u get_surface_rect): crop against the virtual screen in the thread's coordinates. A
  system-aware window on the 96-dpi screen was drawn into 640x1280 of 3840x2160.
- `NEUTRON_RAW_WINDOW_MONITOR` (win32u set_raw_window_pos -> new map_window_rect_raw_to_virt): map through the monitor the
  window is mostly on, not its top-left (a maximized frame overhangs onto the neighbor). A system-aware or dpi-unaware window
  maximized on the second screen never appeared (driver resize came back 1254x687, nothing committed).
- `NEUTRON_NORMAL_FOLLOWS_MONITOR` (win32u): a maximized per-monitor-aware window moved by the driver keeps its restore rect
  on the new monitor (from 10-20; Windows behavior).
- `NEUTRON_ATTACH_DEST` (winewayland attach_shm): viewport destination = window size when the new buffer covers it; a window
  moved back to the 175% screen showed at 1317x731 of 2194x1202 until it repainted (all awarenesses, incl. per-monitor).
- `NEUTRON_WORKAREA_GUARD` (winewayland follow_output): record a maximized height only for the output the configure belongs
  to; a move stored the 175% screen's 1202 for the 100% screen in `<prefix>/.neutron-maximized-inset`.
MEASURED (nested desk, 175% + 100%): `sysmax.c` + `/var/tmp/neutron-pen-lab/run-sysmax.sh` (KWin's presented buffer vs frame,
saved heights), system/pm/unaware x start on either screen: 140 FAIL 6/6 (unaware and system started on the second screen never
appear), patched PASS 6/6; each kill switch alone brings back its failure. Regression: menupm 98% (= 140), mousepos 27 moves 0
mismatched, createpos 6/6 both screens. Lightroom with `AppCompatFlags\Layers lightroom.exe="~ HIGHDPIAWARE"` (lab prefix):
right size opened on either screen, after both moves, Export dialog on the 100% screen whole; GetDpiForMonitor(100% screen) = 168.
Lab runtime `/var/tmp/neutron-i19/rt142lab`. SHIPPED in 11.10-141 (package: sysmax 6/6, menupm 98%, mousepos 0 mismatched, createpos 6/6 both screens, imapi2 PASS; Lightroom via CLI beta.21 right on both screens). The Lightroom Layers value: CLI beta.21 (his call, C83).

### `neutron-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-winewayland-pen-side-buttons.mypatch` (2026-10-21) — the pen's side buttons (GitHub neutron-wine#2)
Reporter (Cintiq 16, KDE 6.8 Wayland): *"Pen buttons currently do not work as intended on a Cintiq 16. No context menu is displayed
for quick switching when pressing the buttons on the pro pen."* `wayland_wintab_button` was an empty stub; KWin delivers the side
buttons as tablet tool buttons (measured on his Cintiq 22HD: BTN_STYLUS 0x14b lower, BTN_STYLUS2 0x14c upper, hovering and in
contact) and they were dropped. Now as a Wacom driver on Windows: pkButtons bit (tip 0, lower 1, upper 2, third 3) + the system
button map's click at the pen (lower = right click, upper = middle click), serial-tagged; BTN_LEFT/RIGHT/MIDDLE tool buttons (a
compositor remap) pass through. Tip sets/clears bit 0 only; its LEFTDOWN/UP no longer overwrites a pending side click. Pen/eraser
declare 4 buttons, SYSBTNMAP L/R/M. `NEUTRON_PEN_BUTTONS=0`.
MEASURED (his Cintiq, `penbtn.c`): 141 FAIL (no side message, pkButtons 0/1); patched: lower -> WM_RBUTTONDOWN/UP + WM_CONTEXTMENU,
0x2 (0x3 in contact); upper -> WM_MBUTTONDOWN/UP, 0x4 (0x5). Photoshop 2026 (lab pfx): his *"it worked. Brush picker appeared"*.
Second report item (pen lost after the mouse visits another screen) NOT reproduced: test window and Photoshop kept the pen with
pressure (his check). Lab runtime `/var/tmp/neutron-i19/rt142pen`. SHIPPED in 11.10-142 (package winewayland.so = the lab-tested build: same build ID).
