# Releasing neutron-wine

`neutron-wine` ships the **Proton-GE way**: this repo is the *recipe* (patch set + pinned
config), and each release is a *prebuilt, self-contained Wine tree* attached to a GitHub
Release. Users never build Wine — they download a versioned tarball that Neutron's engine
resolves and runs.

```
 patches/ + build/  ──build.sh──▶  a complete Wine tree  ──package.sh──▶  dist/*.tar.xz + manifest
   (in git)                          (in ./_work, gitignored)              (GitHub Release assets)
```

## Versioning

The release version lives in [`build/VERSION`](build/VERSION), e.g. `11.10-1`:

- **`11.10`** tracks the upstream Wine base (see [`build/WINE_BASE`](build/WINE_BASE)).
- **`-1`** is the Neutron release revision — bump it when the *patch set or config* changes
  but the Wine base doesn't (`-1` → `-2`). Bump the base half when you move to a new Wine.

The git tag is `v<VERSION>` (`v11.10-1`); the artifact is `neutron-wine-<VERSION>.tar.xz`.

## Cut a release

```sh
# 1. Build the patched Wine from a NEUTRAL work dir. Wine compiles its install prefix into its
#    binaries, so a build under your home directory would ship your home path; package.sh
#    refuses such a tree. wine-tkg is pinned to build/WINE_TKG_COMMIT.
export NEUTRON_WINE_WORK=/var/tmp/neutron-wine
build/build.sh

# 2. Package the built tree into dist/. This also builds DXVK, vkd3d-proton, the NVIDIA wrappers
#    and the ucrtbase shim from external/ (in /var/tmp/neutron-external, reused while external/
#    is unchanged), adds licenses/ and SOURCE, and writes the source archive.
build/package.sh

# 3. Tag and publish ONLY this version's four files.
V=$(cat build/VERSION)
git tag "v$V" && git push origin "v$V"
gh release create "v$V" "dist/neutron-wine-$V.tar.xz" "dist/neutron-wine-$V.tar.xz.sha256" \
    "dist/neutron-wine-$V.manifest.json" "dist/neutron-wine-$V-source.tar.xz" --prerelease \
    --title "neutron-wine $V" \
    --notes "Wine base: $(cat build/WINE_BASE). See patches/MANIFEST.md for the patch set."

# 4. After the clean-room test passes, promote it so `neutron setup` installs it by default:
gh release edit "v$V" --prerelease=false --latest
```

Releases start as **pre-releases**: testers can install them by version, but `neutron setup` only
picks the release marked **Latest**. Promote a build only after the clean-room test passes.

`build/package.sh` produces, for `dist/`:

| File | What |
| --- | --- |
| `neutron-wine-<ver>.tar.xz` | the compiled Wine fork (unpacks to `neutron-wine-<ver>/`) |
| `neutron-wine-<ver>.tar.xz.sha256` | checksum (`sha256sum -c`-compatible) |
| `neutron-wine-<ver>.manifest.json` | machine-readable manifest the engine resolves |
| `neutron-wine-<ver>-source.tar.xz` | complete source of the release: this repo, Wine, wine-staging, wine-tkg, and every external project as compiled |

## How the engine consumes a release

The Neutron CLI pins a runtime version and resolves it from the manifest — download URL +
`sha256` are both in the manifest, so the client can fetch and verify without guessing:

```
neutron setup                      # fetch the Latest neutron-wine, verify sha256,
                                    # unpack to ~/.local/share/neutron/runtimes/<ver>/
neutron launch premiere            # launch against the resolved runtime
```

The tarball stamps `NEUTRON_WINE_VERSION` at its root so an unpacked runtime self-identifies.

## The companion DLLs (external/)

DXVK, vkd3d-proton, dxvk-nvapi, the NVIDIA wrappers (nvcuda, nvenc, wine-nvoptix) and our ucrtbase
shim ship **inside** the runtime tarball, under `lib/wine/<project>/`. Wine never searches those
subdirectories, so they don't shadow its own builtins; `neutron prefix provision` copies them into
each prefix and writes their DLL overrides, as listed in `share/neutron/natives.json`.

- `external/sources.conf` pins each upstream repository to an exact commit.
- `external/patches/<project>/` holds our changes (DXVK `d3d11`/`dxgi`, vkd3d-proton `d3d12core`,
  nvcuda). Everything else is built unmodified.
- `external/build-external.sh <wine-tree>` clones, patches, builds and checks them. The build fails
  if a patched DLL lacks its Neutron markers, an unmodified one contains any, a file has the wrong
  architecture, or any file contains the builder's home path.

Microsoft's files (VC++ runtime, UCRT, d3dcompiler_47, GDI+, core fonts) are **not** in the
tarball: `neutron setup` downloads them from Microsoft on the user's machine.

## Licensing

Wine, vkd3d-proton, nvcuda, nvenc and wine-nvoptix are **LGPL-2.1**; DXVK is zlib and dxvk-nvapi
is MIT. Every release carries its own complete source as `neutron-wine-<ver>-source.tar.xz`, next
to the binaries, and `licenses/` inside the tarball holds every component's license text.
