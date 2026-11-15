# OpenMW Enhanced AppImage

Build from the repository root using the Arch Linux Docker toolchain:

```sh
./enhanced/scripts/build-appimage.sh
```

Build with Bullet threading disabled:

```sh
./enhanced/scripts/build-appimage.sh --bullet-threading off
```

The AppImage is written to:

```text
.build/dist/OpenMW-Enhanced-x86_64.AppImage
```

Persistent build caches live under `.build/` and are mounted into Docker:

```text
.build/archlinux/cmake
.build/archlinux/ccache
.build/archlinux/downloads
.build/dist
```

The default AppImage entry point launches `openmw-launcher`. To run the engine
directly through the wrapper:

```sh
.build/dist/OpenMW-Enhanced-x86_64.AppImage --openmw --version
```

The default Release/LTO build uses Arch's threaded, double-precision `bullet-dp`.
The explicit threading-off build compiles a private static Bullet, using a
checksummed source archive; it uses a separate CMake cache. Set
`OPENMW_BUILD_JOBS=4` to limit parallelism, or `CMAKE_BUILD_TYPE=RelWithDebInfo`
for debug symbols. Native Arch packages use [archlinux/](../../archlinux/README.md).

The AppImage bundles Arch userspace dependencies and Qt/OSG plugins. Host glibc
and GPU drivers must be compatible with that Arch toolchain; older distributions
are not automatically supported by using the AppImage format. Existing OpenMW
configuration, saves and mods continue to use the user's normal paths.

After packaging, check extraction without FUSE:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 .build/dist/OpenMW-Enhanced-x86_64.AppImage --openmw --version
```
