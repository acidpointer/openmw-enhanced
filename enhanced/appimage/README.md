# OpenMW Enhanced AppImage

Build from the repository root:

```sh
./enhanced/scripts/fetch-upstream.sh
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
.build/cmake
.build/ccache
.build/cargo
.build/appdir
.build/dist
```

The default AppImage entry point launches `openmw-launcher`. To run the engine
directly through the wrapper:

```sh
.build/dist/OpenMW-Enhanced-x86_64.AppImage --openmw --version
```
