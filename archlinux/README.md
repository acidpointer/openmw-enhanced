# OpenMW Enhanced for Arch Linux

The package is `openmw-enhanced-git`. It provides `openmw` and conflicts with the
official package (and other packages providing `openmw`). Pacman manages that
replacement; do not install with `--overwrite`. It does not automatically
replace another package during an unrelated system upgrade.

With `base-devel` and `git` installed, build the current checkout, including
uncommitted changes, as your normal user:

```sh
./enhanced/scripts/build-archlinux.sh
```

`makepkg --syncdeps` may request sudo to install missing build dependencies.
The helper installs the resulting package through makepkg/pacman after a
successful build and may request sudo. Package files are retained in
`.build/dist/`; makepkg/CMake, downloads and compiler caches remain under
`.build/archlinux/`. Use `--no-install` to build without installing, as CI does.
An already built package can be installed directly with
`sudo pacman -U /path/to/package.pkg.tar.zst` without rebuilding.

For a conventional VCS package build, run `makepkg -si` inside `archlinux/`.
That builds the published `enhanced/main` branch; use the helper above for local
edits. The VCS source intentionally has a `SKIP` checksum because its commit
changes; `pkgver()` records the revision actually built. `.SRCINFO` is metadata
for the checked-in recipe, not a promise of an immutable source snapshot.

The normal binaries, desktop entries, `/etc/openmw` defaults and
`/usr/share/games/openmw` resources retain upstream paths. Your
`~/.config/openmw`, saves and mods remain in place. Both OpenGL and Vulkan are
included. Arch's threaded double-precision `bullet-dp` is the default.
`collada-dom` is required for OpenMW's mandatory OSG DAE reader, even when a
particular game session does not load a DAE asset. The launcher supervises the
engine and restores its window on a failed launch instead of exiting silently.

mimalloc 3.6.1 is built statically into the executable targets; no system
mimalloc package is needed. Conventional makepkg builds verify its source
archive. The local helper's first CMake configuration may fetch that dependency
while continuing to compile your local OpenMW checkout. Workload defaults and
overrides are documented in [the allocator notes](../enhanced/docs/mimalloc.md).

See [UPSTREAM.md](UPSTREAM.md) for the official PKGBUILD baseline and changes.
