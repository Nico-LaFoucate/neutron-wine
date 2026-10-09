# d2d1 command-list playback — UNVERIFIED, not built

**The `EndDraw` half shipped as `patches/neutron-zzz9-d2d1-cmdlist-enddraw.mypatch` (11.10-20).
The playback half below is correct-looking code that After Effects never executes; it stays here,
unbuilt — this directory is not read by `build.sh`.**

## What the change is

Wine's d2d1 could *record* an `ID2D1CommandList` (all 22 op types) and `Stream()` it to a
caller-supplied `ID2D1CommandSink`, but had no sink of its own, so nothing could replay a list
into a device context. Two ends of one gap:

| where | was | now |
|---|---|---|
| `device.c` `d2d_device_context_EndDraw` | `FIXME` + `return E_NOTIMPL` for a command-list target | returns `context->error.code`; recording just ends, nothing to present |
| `device.c` `d2d_device_context_DrawImage` | only understood `ID2D1Bitmap`; a command list fell to `FIXME("Unhandled image")` and drew nothing | QIs `IID_ID2D1CommandList` → `d2d_command_list_playback()` |
| `command_list.c` | — | `d2d_playback_sink`: an `ID2D1CommandSink` forwarding all 24 methods to an `ID2D1DeviceContext` (see `d2d1-command-list-playback.c.part`) |
| `d2d1_private.h` | — | declares `d2d_command_list_playback()` |

Transform composition is the only subtle part: ops are recorded in the LIST's coordinate
space, so effective = `recorded * translate(target_offset) * caller_transform`. The sink holds
that as `base` and composes it on every `SetTransform`. Context drawing state (transform,
antialias modes, blend, unit mode, tags, text params) is saved and restored around a replay.

## THE MEASURED RESULT — read this before continuing the work

**The `EndDraw` fix is real and load-bearing.** After Effects 2025's composition viewer died on
its very first draw with `0x80004001` — surfaced by the app as *"direct2d drawbot error.
HRESULT: -2147467263"*. With it fixed:
* `Composition > New Composition` **succeeds** (user-confirmed);
* command-list use exploded from **2 → 301 `Close` calls**, i.e. AE exercises the path hard;
* the Composition panel changed from **white to black** — AE's paint pipeline now runs to
  completion instead of aborting.

**The playback sink is, so far, DEAD CODE for After Effects.** Instrumented and measured over a
full session (`NEUTRON_D2D_OPTRACE=1`, counters at `neutron-cl:`):

* **48+ lists closed, every one in state `OPEN`** (healthy recording — note the enum order is
  `INITIAL=0, ERROR=1, OPEN=2, CLOSED=3`, so a logged `state=2` means OPEN, **not** closed;
  misreading that sends you the wrong way), 1440–5120 bytes each, up to 132 referenced objects.
* **Zero `Stream()` calls.** AE never plays them back itself.
* **Zero `DrawImage` calls with a command list** — and zero `"Unhandled image"`, so DrawImage
  isn't being handed anything unusual at all.
* **Zero `"Only image brushes with bitmaps are supported"`** ⇒ the image-brush route is ruled
  out too.

⇒ **AE records healthy command lists and never consumes them by any d2d path.** Whatever it
does with them, it is not drawing them. (Its blank composition had another cause, fixed
2026-08-03: see `patches/MANIFEST.md`.)

Keep the code anyway: it is a genuine Wine gap (upstream-quality, and the `EndDraw` half is
required), but do NOT claim it fixes AE.

## Deliberate limitations
* No `ID2D1CommandSink1/4` (`SetPrimitiveBlend1/2`). `Stream()` already falls back to
  `SOURCE_OVER` for those, which is the pre-existing behavior, so declining the QI loses nothing.
* A `Clear` inside a list with no `image_rect` clips to the whole target rather than the list's
  extent — D2D command lists have infinite extent, so there is nothing to clip to.

## Why this is not a .mypatch yet
The patch-built baseline tree (`~/neutron-wine/_work/.../wine-git`) **did not exist** when this was
written (2026-07-31), and the rule earned at real cost is: *generate a patch against the
PATCH-BUILT baseline, never by filtering a dev-tree diff* — the dev tree holds every neutron change as one blob, so a keyword
filter emits neighboring patches' lines and duplicates code (cost: 3 builds). `git diff` of the
three touched files here is **3947 lines**, almost all of it other people's work, which is
exactly that trap.

To productionize: rebuild the baseline via `build.sh`, apply the block in
`d2d1-command-list-playback.c.part` plus the three small edits above, then generate the
`.mypatch` against that baseline and prove it by re-apply + `cmp`.

## How it was tested
Built in the dev tree and staged as a single dll with `~/stage-d2d1-cmdlist.sh` (backs up the
runtime's `d2d1.dll`, refuses while any Adobe app is live, `--revert` restores). Verified before
staging that all five 11.10-19 d2d1 fix markers matched the shipped dll and the only string
difference was the removed `E_NOTIMPL` one — strong evidence, not proof, that the staged dll was
11.10-19 + this change.
