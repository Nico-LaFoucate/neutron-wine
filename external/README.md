# external/: the non-Wine half of the runtime

The neutron-wine runtime also carries the graphics and NVIDIA translation layers that the Adobe
apps run on. They are built from upstream source at pinned commits, with our patches where we
needed changes, and installed under `lib/wine/<project>/` in the runtime. `neutron prefix
provision` copies them into each prefix and writes their DLL overrides.

| Project | Upstream | Ships | Neutron patch | License |
|---|---|---|---|---|
| DXVK | [doitsujin/dxvk](https://github.com/doitsujin/dxvk) | x64 `d3d11`, `dxgi` | yes | zlib |
| DXVK (unmodified) | same | x64 `d3d8`, `d3d9`, `d3d10core`; all x86 | no | zlib |
| vkd3d-proton | [HansKristian-Work/vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) | x64 `d3d12`, `d3d12core` | yes | LGPL-2.1 |
| vkd3d-proton v3.0.1 | same | x86 `d3d12`, `d3d12core` | no | LGPL-2.1 |
| nvcuda | [SveSop/nvcuda](https://github.com/SveSop/nvcuda) | `nvcuda` | yes | LGPL-2.1 |
| nvenc | [SveSop/nvenc](https://github.com/SveSop/nvenc) | `nvcuvid`, `nvencodeapi64` | no | LGPL-2.1 |
| wine-nvoptix | [SveSop/wine-nvoptix](https://github.com/SveSop/wine-nvoptix) | `nvoptix` | no | LGPL-2.1 / MIT |
| dxvk-nvapi v0.9.2 | [jp7677/dxvk-nvapi](https://github.com/jp7677/dxvk-nvapi) | `nvapi64`, `nvofapi64`, x86 `nvapi` | no | MIT |
| ucrtbase shim | this repo (`ucrtshim/`) | x64 `ucrtbase` | ours | LGPL-2.1-or-later |

Exact commits are in [`sources.conf`](sources.conf).

## What the patches do

- **DXVK** (`patches/dxvk/`): presents the app's swapchain through GDI into the window
  (`NEUTRON_GDI_PRESENT`), which is what makes Premiere Pro's GPU-drawn interface reach the
  screen under Wayland. Also exports textures for CUDA interop and adds bounded-wait acquire.
- **vkd3d-proton** (`patches/vkd3d-proton/`): the blit-to-window present path
  (`libs/vkd3d/blit_to_window.h`) and `NEUTRON_DISABLE_BTW`, which apps that draw better without
  it (Photoshop, Lightroom Classic, After Effects) set in their launch profiles.
- **nvcuda** (`patches/nvcuda/`): `cuGraphicsD3D11RegisterResource` and the D3D11/CUDA interop
  that After Effects needs to render compositions.

The patches also keep a few diagnostics and tuning switches that do nothing unless their
environment variable is set: `NEUTRON_TRACE`, `NEUTRON_ROOTDUMP`, `NEUTRON_UXP_PROBE`,
`NEUTRON_SC_TRACE`, `NEUTRON_DIRTYRECTS`, `NEUTRON_ACQUIRE_TIMEOUT_NS`, `NEUTRON_VKD3D_RT_PROBE`,
`NEUTRON_CUDA_PLAYBACK_EXPERIMENT`.

The ucrtbase shim (`ucrtshim/`) forwards every export to Microsoft's UCRT, which `neutron setup`
downloads from Microsoft and installs as `ucrtbase_orig.dll`. On top of that it adds
`__CxxFrameHandler4` and lets `_wstat64` accept `\\?\` paths, which Premiere's hardware-export
muxer needs. `ucrtbase-10.0.10586.15.exports` is the list of export names it forwards.

## Building

```sh
external/build-external.sh <built-wine-tree>
```

`build/package.sh` runs this for every release. It clones each project at its pinned commit into
`/var/tmp/neutron-external`, applies the patches, builds with meson (release, stripped), checks the
results and installs them into the tree. It needs `meson`, `ninja`, `glslang`, `mingw-w64-gcc` and
`git`; Wine's own tools (`winegcc`, `widl`) come from the tree being packaged. The build fails if:

- a patched DLL lacks its Neutron markers, or an unmodified one contains any;
- a file has the wrong architecture;
- any file contains the builder's home directory.
