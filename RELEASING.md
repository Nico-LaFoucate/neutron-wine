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
# 1. Build the patched Wine (fetches wine-tkg, applies patches/, builds → ./_work)
build/build.sh

# 2. Package the built tree into dist/ (tarball + sha256 + manifest.json)
build/package.sh

# 3. Tag and publish. Attach ALL THREE dist/ files to the release.
git tag v$(cat build/VERSION)
git push origin v$(cat build/VERSION)
gh release create v$(cat build/VERSION) dist/* \
    --title "neutron-wine $(cat build/VERSION)" \
    --notes "Wine base: $(cat build/WINE_BASE). See patches/MANIFEST.md for the patch set."
```

`build/package.sh` produces, for `dist/`:

| File | What |
| --- | --- |
| `neutron-wine-<ver>.tar.xz` | the compiled Wine fork (unpacks to `neutron-wine-<ver>/`) |
| `neutron-wine-<ver>.tar.xz.sha256` | checksum (`sha256sum -c`-compatible) |
| `neutron-wine-<ver>.manifest.json` | machine-readable manifest the engine resolves |

## How the engine consumes a release

The Neutron CLI pins a runtime version and resolves it from the manifest — download URL +
`sha256` are both in the manifest, so the client can fetch and verify without guessing:

```
neutron runtime install            # fetch the pinned neutron-wine, verify sha256,
                                    # unpack to ~/.local/share/neutron/runtimes/<ver>/
neutron launch premiere            # launch against the resolved runtime
```

The tarball stamps `NEUTRON_WINE_VERSION` at its root so an unpacked runtime self-identifies.

> The companion DLLs (patched DXVK `dxgi`/`d3d11`, vkd3d-proton `d3d12core`, the nvcuda
> wrapper) are **not** part of this Wine tarball — they come from their own forks and are
> overlaid by the Neutron runtime bundle (`neutron runtime capture` on the engine side).
> Keep the two concerns separate.

## Licensing

Wine is **LGPL-2.1**. Distributing compiled Wine binaries obligates us to offer the
corresponding modifications — which this public repo *is* (the patch set in `patches/` +
the pinned upstream base in `build/WINE_BASE`). Every release's notes should point back at
the patch set and the upstream ref so the compiled artifact's source is discoverable.
