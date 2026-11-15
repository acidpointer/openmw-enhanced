# Arch packaging provenance

`PKGBUILD` is adapted from the official Arch Linux `openmw` package, rather
than an AUR recipe:

- Repository: <https://gitlab.archlinux.org/archlinux/packaging/packages/openmw>
- Baseline commit: `c4afbec072e6b46b1cb095cae4c20ad780f9e7ba` (`0.51.0-3`).
- Unmodified baseline: [upstream/openmw.PKGBUILD](upstream/openmw.PKGBUILD).
- Baseline SHA-256: `d4b1b911a8df0756783f0a18dab53dc00730d7977b7de1e4accc7d5e7a4514ec`.
- Original maintainer/contributor credit is retained in the forked recipe.

Fork changes: use `openmw-enhanced-git` and the fork's `enhanced/main` branch;
derive versions from the engine version and Git revision; provide `openmw` and
conflict with official/alternative packages providing it; preserve `/etc/openmw`
configuration with pacman backup entries; enable Vulkan, system Recast and the
shared fork build configuration. Shader compilation requires `glslang` at build
and runtime. Vulkan GPU drivers remain user-selected, not bundled.
`collada-dom` is a required runtime dependency: OpenMW loads the OSG DAE reader
at startup, and Arch's OSG package treats that reader's COLLADA library as an
optional dependency. Merely installing it in the Docker image is insufficient
for native package users.
The recipe also verifies the pinned mimalloc 3.6.1 archive for static compilation
and includes its MIT license; it does not depend on a shared mimalloc package.

The official Arch `bullet-dp` package already builds Bullet with
`USE_DOUBLE_PRECISION=ON` and `BULLET2_MULTITHREADING=ON`. There is no reason to
fork and replace that system package just to enable threading. Audited recipe:
<https://gitlab.archlinux.org/archlinux/packaging/packages/bullet/-/blob/b173110d97f40ff84d798068b569c1f4850acb70/PKGBUILD>.
The AppImage's optional unthreaded comparison build uses the same 3.25 archive
and SHA-512 checksum from that recipe, privately linked into that build only.

To refresh the baseline, review a newer official commit, record its identity
and checksum here, replace the baseline file, and apply relevant changes to
our recipe. Do not overwrite the fork's source, conflict, or Vulkan settings.
