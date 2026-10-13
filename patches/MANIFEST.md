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
