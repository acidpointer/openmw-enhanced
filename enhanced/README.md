# OpenMW Enhanced Build Infrastructure

This directory contains fork-owned infrastructure. The goal is to keep upstream
OpenMW easy to update while still letting developers work with normal source
files and Git branches.

## Source Layout

This repository is the actual OpenMW fork. Upstream OpenMW source lives at the
repository root, and our fork-specific tooling lives in `enhanced/`.

```text
repo root = OpenMW source tree
enhanced/ = fork build, packaging, Rust, and patch metadata
```

This is not a submodule workflow. It is normal Git fork workflow:

- `upstream`: canonical OpenMW, default `https://gitlab.com/OpenMW/openmw.git`
- `origin`: our fork remote
- `enhanced/main`: fork integration branch

Configure the fork remote:

```sh
./enhanced/scripts/setup-openmw-fork.sh --fork-url <your-fork-url>
git push -u origin enhanced/main
```

Fetch upstream without changing the current branch:

```sh
./enhanced/scripts/sync-openmw-upstream.sh
```

Update the current fork branch from upstream:

```sh
./enhanced/scripts/sync-openmw-upstream.sh --merge
```

## Docker Builds

Build a development tree:

```sh
./enhanced/scripts/build-dev.sh
```

Build an AppImage:

```sh
./enhanced/scripts/build-appimage.sh
```

Build with Bullet threading disabled:

```sh
./enhanced/scripts/build-appimage.sh --bullet-threading off
```

Docker mounts persistent caches from `.build/` so repeated builds reuse CMake
outputs, ccache, Cargo downloads, and release artifacts.

## Manual verification after upstream synchronization

Build and runtime checks are run by the user. Agents may perform static source
checks, but must not run builds or packaging.

After building, check the packaged version:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 .build/dist/OpenMW-Enhanced-x86_64.AppImage --openmw --version
```

Launch the AppImage and confirm the launcher retains the existing mod/content
selection. In game, check exterior and interior transitions, doors and distant
objects with occlusion enabled, water/refraction, first/third-person screen-space
shadows, and the shadow settings (resolution, soft shadows, temporal reuse).
Screen-space shadows require post-processing to be enabled. Check the log for
bundled `usr/bin/openmw.cfg` and `defaults.bin` together with user
`~/.config/openmw/openmw.cfg` and `settings.cfg`.

Check both launching through the launcher's Play button and starting the engine
directly with `--openmw`. The AppImage launcher hides while the engine runs and
exits when the engine finishes; it must keep the AppImage mounted so texture
reader plugins and other resources remain available. Repeat the Play-button
check with `APPIMAGE_EXTRACT_AND_RUN=1` to cover extraction without FUSE.

OSG plugins must retain their `usr/lib/osgPlugins-VERSION/` directory. The
wrapper puts `usr/lib` first on `OSG_LIBRARY_PATH` so bundled plugins take
precedence over host plugins with the same OSG version but a different ABI.
With `LD_DEBUG=libs`, plugin initialization paths should be inside the AppImage,
not the host's `/usr/lib/osgPlugins-VERSION/` directory.

## GPU selection and frame pacing diagnostics

In the launcher, open Graphics → Rendering GPU, choose a GPU, and press Play.
Automatic preserves the inherited system selection. Explicit Linux choices use
stable PCI addresses for Mesa and PRIME offload for a single proprietary NVIDIA
GPU. The choice is saved in the normal `launcher.cfg`; no mod or game settings
are changed. Missing or unsupported saved devices fall back to Automatic. Multiple
proprietary NVIDIA GPUs are not individually selectable until provider mapping
is implemented. Other platforms currently offer Automatic only.

“Last game renderer” shows the renderer from the previous `openmw.log`. After
launching, confirm the new log's `OpenGL Renderer:` line matches the intended GPU.
GPU selection applies to Play; direct `--openmw` starts retain their command-line
environment. The launcher process itself keeps its existing GPU.

For a short diagnostic session, start the launcher from the repository root:

```sh
OPENMW_FRAME_TRACE=/tmp/openmw-frame-trace.json \
OPENMW_GPU_TIMINGS=1 \
OPENMW_OSG_STATS_LIST='times;resource' \
OPENMW_OSG_STATS_FILE=/tmp/openmw-frame.stats \
.build/dist/OpenMW-Enhanced-x86_64.AppImage
```

Choose the GPU, load the same Balmora save, hold the affected view for about
30 seconds, then move/turn through the area for another 30 seconds and quit
normally. Keep resolution, mods, post-processing chain, and effect settings the
same between GPU comparisons. Diagnostic recording has overhead; compare normal
play as well. No performance improvement is claimed until these runs are compared.

The CPU trace is Chrome Trace JSON, finalized on normal exit. Its bounded writer
queue drops events rather than making producers wait for file output; the JSON
reports the drop count. It records frame/traversal waits, Lua synchronization,
cache maintenance and locks, worker work, post-processing draw dispatch, and
buffer swaps. Summarize it without starting the game:

```sh
python3 enhanced/scripts/analyze-frame-trace.py /tmp/openmw-frame-trace.json --skip-seconds 20
```

Optional GPU timestamps measure elapsed GPU time around each dispatched
post-processing pass, including its mipmap generation. They do not modify effects,
their order, samples, or resolution. Results are polled without waiting, with at
most 256 pending pass timings per canvas/context. Supported drivers add
`GPU postprocess EFFECT/PASS time taken` values (seconds) to viewer statistics.
Statistics output is delayed 16 frames in this mode; unavailable/late results can
be absent. GPU elapsed time is not a utilization measurement. CPU waits and GPU
work can overlap; a high FPS peak does not rule out stalls in slower frames.

## Occlusion and frame-pacing audit

Culling now wraps the world subtree in one scoped callback. The owning cull
visitor, camera, projection, frame, and world-root path must match before a child
callback may use the depth buffer. Nested/concurrent traversals fall back to normal
rendering, and exception unwinding releases ownership. Shadow and water cameras
are not culled by this path. Water reflection/refraction clip planes are not yet
represented by MOC; the existing water-occlusion setting currently falls back to
normal rendering. Orthographic cameras also bypass MOC.

A dedicated terrain subtree precedes cell objects, including after worldspace
changes. Paging managers are connected when created and when cache storage is
assigned. Every paged chunk gets the terrain visibility callback, including
chunks without eligible occluder triangles. It runs before the chunk's light-list
callback, avoiding light-list preparation for terrain-hidden chunks. Bounds
include parent scale/rotation. The active worldspace and cell size now update the
terrain occluder instead of leaving it on the initial worldspace.

The previous vertex clustering and whole-model centroid shrink could bridge
windows or move concave/disconnected surfaces into empty space. Proxies now keep
at most `2 * resolution^2` original opaque triangles, preferring large faces.
Optional shrinking only insets each triangle within its own plane; values above
one are clamped. Original winding is retained, and the rasterizer rejects
clockwise backfaces. Transparent/alpha-tested paths, including alpha state moved
by shader conversion, and dynamic/camera-dependent branches (including LODs)
do not occlude.
No render meshes, effect shaders, mods, or visual-quality settings are modified.
This conservative proxy can lose coverage on highly tessellated surfaces; that
tradeoff requires visual and timing checks rather than assuming a net FPS win.

Eligible unpaged static records are prepared from retained immutable templates
by one background worker. Culling checks ready results without disk access or
mesh generation. A full 128-request strong-reference queue now defers additional
requests through a deduplicated FIFO backlog of at most 4096 weak references,
rather than permanently losing ordinary dense-load requests. Expired sources are
skipped; overflow/failure always leaves objects visible. Shutdown stops chaining,
cancels queued work, and joins active work before closing SQLite. Ready-hit,
queued, deferred, and rejected counts appear in occlusion debug messages.
Persistent keys use the `mesh-v3` algorithm namespace and the winning VFS asset's
content hash, invalidating older unsafe proxies and changed mod meshes.

SIMD is already part of Intel's [MOC implementation](https://github.com/GameTechDev/MaskedOcclusionCulling).
The engine now uses its SSE/NEON helper for AABB projection and its runtime-selected
rasterizer to transform retained local meshes directly, avoiding per-instance
index copies and CPU world-vertex arrays. Projection is reused for screen-size
and visibility decisions. Near-plane crossings and invalid projections remain
visible; query rectangles receive a one-software-pixel safety margin, and the
rasterizer uses the configured camera near plane. CPU feature detection now
queries supported CPUID leaves directly and retains SSE4.1 when AVX is unavailable
to the OS. The SSE2 dispatcher/helper no longer compiles with a translation-unit
SSE4.1 baseline; stronger instructions are scoped to their implementation, with
separate AVX objects and interprocedural optimization disabled for this library.
There is no global `-march=native` or unconditional AVX requirement. See
[GCC target pragmas](https://gcc.gnu.org/onlinedocs/gcc/Function-Specific-Option-Pragmas.html)
and [Clang target attributes](https://clang.llvm.org/docs/AttributeReference.html#target).

Terrain is rasterized once and its SIMD depth tiles are merged into the full
buffer, replacing the second transform/clip/raster pass. Different SIMD backends
fall back to separate rasterization because their tile layouts differ.
Terrain geometry is borrowed without per-frame vector copies. A camera-cell
cache hit performs no mesh rebuild, including when terrain is empty. On a
one-cell move with radius `r`, overlapping cells reuse their meshes and only the
new border needs storage reads: for example, radius 1 needs 3 new cells instead
of rebuilding all 9. Worldspace/LOD changes invalidate the cache; cell entries
outside the current window are evicted. Cold/uncached border construction still
runs during culling and is a remaining candidate for asynchronous preparation.

The static time budget includes candidate gathering, selection, and raster work.
Candidate selection reserves half the configured budget for rasterization;
one operation may finish beyond the limit. Zero retains unlimited behavior.
The configured adaptive benefit threshold is honored. During rendering-statistics
captures, camera statistics include `Occlusion objects tested`, `Occlusion objects
skipped`, `Occlusion cells skipped`, `Occlusion chunks skipped`, `Occlusion static
triangles`, and occlusion overhead/candidate/raster times in seconds. These show
actual culling decisions and CPU cost; skipped objects are not a draw-call count.

Resource cache expiry keeps its existing cadence and lifetime rules but scans
copyable keys in batches of at most 128 under the mutex, using key checkpoints
that survive concurrent insertion/removal. It no longer holds one lock for the
entire cache sweep. Replacement, removal, clear, and destruction retire values
outside the cache mutex, allowing resource destructors to reenter safely.
Graphics threads stop before world/resource teardown. These remove identifiable
stall/race hazards; their contribution to the reported Balmora spikes remains
unmeasured.

Regression sources cover solid-wall rejection, visible openings, near-plane and
front/backface behavior, local mesh transforms, SIMD dispatch fallback, camera/
frame/projection/nested-world ownership, proxy topology, shader-converted alpha,
queue overflow, content invalidation, invalid indices, terrain reuse/invalidation,
and cache retirement/batching. GPU environment/config tests remain included.
Agents have performed static checks only and must not build or execute tests.
The user can run the component-test filters `Occlusion*`, `OccluderMeshTest.*`,
`TerrainOccluderTest.*`, `ResourceGenericObjectCacheTest.*`, `RenderGpuTest.*`, and
`LauncherSettingsTest.*` using their normal component-test build.

No FPS improvement or complete visual correctness is certified without those
tests and gameplay captures. Before playtesting, a user-run cached build and
component tests provide the next useful evidence. No game or window is launched
by the focused test selection below. From the host, enter the existing build
container, then compile the engine/launcher and run the focused tests:

```sh
./enhanced/scripts/shell.sh
# The following commands run inside that shell; they are for the user to execute.
cmake -S /src -B /build/appimage/build -DBUILD_COMPONENTS_TESTS=ON -DOPENMW_USE_SYSTEM_GOOGLETEST=OFF
cmake --build /build/appimage/build --target openmw openmw-launcher components-tests --parallel 4
/build/appimage/build/components-tests --gtest_filter='Occlusion*:OccluderMeshTest.*:TerrainOccluderTest.*:ResourceGenericObjectCacheTest.*:RenderGpuTest.*:LauncherSettingsTest.*'
# Restore the normal packaging build's test option afterward.
cmake -S /src -B /build/appimage/build -DBUILD_COMPONENTS_TESTS=OFF
```

The first test configuration may download the repository's pinned GoogleTest and
test-data dependencies. It reuses the existing CMake tree and caches rather than
cleaning them. Agents have not run any of these commands. After packaging, check the
version command above. Later, compare the same save/view/mods/effects on each GPU,
check interiors/doors/worldspace transitions and shutdown, and record the CPU/GPU
and occlusion statistics described above. Further batching, shadow submission,
lighting, and driver/swap work should follow the timings. Mod effects remain
outside the optimization scope.
