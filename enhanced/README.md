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
wrapper puts an alias to that directory, followed by `usr/lib`, first on
`OSG_LIBRARY_PATH` and `LD_LIBRARY_PATH`. Versioned and bare plugin names must
resolve to the same physical file. Bundled libraries also retain unversioned
aliases such as `libosg.so` for OSG's lazy node-kit loading. These paths take
precedence over host plugins with the same OSG version but a different ABI.
AppRun supplies `OPENMW_ENHANCED_OSG_LIBRARY_PATH`; the engine and launcher use
it to replace OSG's complete library search list before loading any plugins.
Setting `OSG_LIBRARY_PATH` alone only prepends paths: OSG still appends its
compiled-in host directories. Native installs retain their normal search list.
With `LD_DEBUG=libs`, plugin initialization paths should be inside the AppImage,
not the host's `/usr/lib/osgPlugins-VERSION/` directory.

### Exit crash investigation

The captured Play-button session loaded a save, reported duplicate `null` and
`zlib` compressor registrations, and crashed after `Quitting peacefully.` The
staged AppDir contained separate copies of the same OSG plugins in `usr/lib`
and `usr/lib/osgPlugins-3.6.5`. It lacked `osgdb_deprecated_osg.so` and
unversioned node-kit aliases including `libosg.so` and `libosgDB.so`. The bundled
OSG ABI was 162; the host legacy reader required ABI 161. This left lazy scene
loading able to mix incompatible OSG libraries. The compressor registrations
belong to `libosgDB`, so removing duplicate serializer files alone would not
close that route. Initially, host debuggers also failed to start because they
inherited bundled OpenSSL libraries.

After those changes, the exit crash persisted. The completed crash reports
identified another missing reader: `/usr/lib/osgPlugins-3.6.5/osgdb_glsl.so`
was loaded from the host alongside bundled plugins. The same loaded-library
list contained both bundled `libosgDB.so.162` and host `libosgDB.so.161`,
`libosg.so.161`, and `libosgUtil.so.161`. The crashing thread showed almost
1,000 repeated frames inside bundled `libosgDB`. Disassembly of the staged
library maps the fault and repeated return address to offsets `0xd983b` and
`0xd9840`, the recursive erase helper for a `std::map<std::string, std::string>`.
This establishes a stack overflow during OSG map destruction and continued ABI
mixing, rather than proving which map was first corrupted. The GLSL reader is
now required by CMake/startup checks and bundled by the packaging script. The
AppImage search-list replacement closes the compiled-in host-directory fallback
that survived the earlier path-priority fix.

Packaging now keeps one canonical plugin copy, includes current and legacy
serialization wrappers from the build's OSG installation, and preserves the
matching node-kit aliases. It uses linuxdeploy's `--deploy-deps-only` for these
staged files instead of flattening plugin copies with `--library`. Deployment
checks reject OSG dependency ABI mismatches, missing targets, duplicate plugin
copies, and broken aliases before publishing the new AppImage. Existing cached
AppDirs are repaired without cleaning the build cache.

The engine restores its previous OSG notification handler before application
logging ends and retires the GUI log listener before stream teardown. AppRun
also records the original host environment. Only the separate crash helper
restores that snapshot before starting host debuggers or dialogs, avoiding the
OpenSSL and `zenity`/GLib loader failures without changing the running game's
library paths. Environment restoration has regression test coverage for native
installs, incomplete snapshots, and the distinction between empty and unset
variables. These changes have only been checked statically; build and runtime
validation remain user-run. Additional resolution tests cover a missing bundled
plugin with an existing fake host plugin, versioned/bare aliases, native
installs, and incomplete AppImage environment metadata. These tests only resolve
file paths and do not load the fake plugin files.

References: [OSG library naming and loading](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6/src/osgDB/Registry.cpp),
[OSG lazy wrapper loading](https://github.com/openscenegraph/OpenSceneGraph/blob/master/src/osgDB/ObjectWrapper.cpp),
[linuxdeploy dependency-only deployment](https://github.com/linuxdeploy/linuxdeploy/blob/master/src/main.cpp).

### Dependency boundary audit

The Docker build mounts the source and build/download caches, not the host's
`/usr/lib` or system OpenMW installation. Packaged dependencies are collected
inside the Ubuntu container. AppDir staging is recreated for each build while
the compilation caches remain available.

A static audit of the exit capture and exact staged dependencies covered 236
ELF files, including the launcher's dependency closure. It found no missing
`DT_NEEDED` providers, required symbol versions, or strong dynamic symbols, and
no absolute or empty RPATH/RUNPATH entries in bundled ELF files. The only
multiple-version library families in that capture were OSG 161/162. This is
also why symbol checks alone cannot certify C++ ABI compatibility or predict
every future `dlopen` path.

Host glibc, graphics drivers, ALSA, and PipeWire are intentional system
dependencies; they should not be replaced wholesale with container libraries.
The existing `qt.conf` redirects Qt's built-in plugin path into the AppImage.
AppRun additionally limits `QT_PLUGIN_PATH` to bundled directories and removes
`QT_QPA_PLATFORM_PLUGIN_PATH`, preventing inherited Qt installation paths from
bypassing that configuration. The separate crash helper restores both original
Qt overrides for host tools. Explicit inherited `LD_LIBRARY_PATH` driver
overrides remain supported; AppRun no longer adds an empty entry when the
variable is unset or empty, which would otherwise search the working directory.
User configuration, content selection, saves, graphics-driver selection, and
mod effects are unaffected by these packaging changes.

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
The presentation-only path reports `GPU postprocess presentation time taken`;
HDR exposure calculation, including luminance mipmap generation, reports
`GPU postprocess luminance time taken`. These stages were previously missing
from the per-pass measurements. Resource statistics also report
`Postprocess intermediates`, the number of allocated scene intermediates for
the current canvas (0–3, excluding effect-owned render targets and stereo resolve).
Statistics output is delayed 16 frames in this mode; unavailable/late results can
be absent. GPU elapsed time is not a utilization measurement. CPU waits and GPU
work can overlap; a high FPS peak does not rule out stalls in slower frames.

## Measured post-processing slowdown in interiors

The November 8 A500 capture loaded Adanumuran, an interior. With post-processing
off, the stable 13–15 second window reported a median GPU frame time of 4.25 ms.
With it on, the 17–19 second window reported 14.19 ms. The dispatched passes were
screen-space shadows, HDR, and FXAA; the underwater and depth-of-field techniques
in the configured chain were not dispatched in that view.

| GPU stage | Median time in the enabled window |
| --- | ---: |
| Shadow raw mask | 0.48 ms |
| Shadow directional filter | 3.99 ms |
| Shadow cross filter | 2.86 ms |
| Shadow composite | 0.74 ms |
| HDR luminance | 0.33 ms |
| HDR tone mapping | 0.26 ms |
| FXAA | 0.81 ms |

One confirmed avoidable cost in this capture was filtering a deliberately empty indoor shadow
mask. `screenSpaceShadowLoss` already returned zero in interiors, but the technique
lacked `disable_interiors`, so both filters and the composite still ran. This
spent about 7.6 ms filtering/compositing zero, plus the raw pass and the normals
render target. The previous intermediate-allocation optimization did not resolve
this bottleneck. These timings establish the captured A500 case; Intel Xe has not
yet been profiled, and diagnostic timestamps themselves can affect performance.

The shader now declares `disable_interiors`, retaining all four passes, 64 trace
steps, normal precision, filtering, and resolution for exteriors and interiors
that behave like exteriors. Buffer requirements are recomputed from the passes
active in the current environment, using the same environment mask as dispatch.
An inactive technique therefore cannot require normals output, point-light
collection, or HDR exposure calculation. Global post-processing disable also
clears these requirements. This matters on interior/exterior and underwater
transitions, not just when editing the chain.

A component regression test loads the actual shipped shader, checks that it is
masked in both indoor water states and remains active in both exterior states,
and verifies that its four passes and normals requirement are retained. Agents
performed syntax and formatting checks only; the regression test and GPU checks
must be run by the user.

After the cached build, repeat the Adanumuran capture using new output filenames
to preserve the baseline. There should be no `GPU postprocess screen_space_shadows/`
entries indoors. HDR and FXAA remain enabled and have their own GPU costs. On
exiting the interior, verify that screen-space shadows and their normals output
return, including after disabling and re-enabling post-processing. The captured
baseline is saved locally as `/tmp/openmw-postprocess-before.stats`; no post-fix
FPS improvement has been measured yet.

## Further rendering optimizations

The following changes remove work from existing rendering paths while retaining
the selected light limit, shadow trace steps, filter taps, target dimensions, and
normal precision. They have not yet been benchmarked or visually verified on the
A500 or Xe; playtests are postponed. The interior capture above establishes that
particular workload, not a diagnosis for every exterior or mod configuration.

| Path | Avoided work | Behavior retained |
| --- | --- | --- |
| Water transparency sorting | Check for a water drawable before transforming every transparent object's eight bounding-box corners or building water-side lists. | The existing depth order, water-side classification, and clipping when water is present. |
| Transparent depth replay | Find the first eligible leaf instead of counting the entire list before traversing it again. Exclude the water drawable during that search. | The same eligible geometry, alpha threshold, and depth writes. |
| Depth-only render state | Explicitly mask all color writes rather than relying on a fragment shader with no assigned color output. | The opaque color attachment used by water refraction is protected during depth replay. |
| Idle distortion | Reuse a buffer known to be empty instead of binding, clearing, and restoring it every frame. | The texture remains black for any effect that samples it; the first empty frame after distortion clears it again. |
| Per-object lighting | Select and sort only the closest retained lights with `partial_sort`, reducing selection from O(N log N) to O(N log K) for N candidates and a K-light limit. Avoid object-bound transforms when the view has no lights. | Radius-weighted proximity, the configured limit, ignored lights, and the sorted retained prefix. Equal-priority light ordering remains unspecified. |
| Screen-space shadow filters | Reuse sampled world depth for first-person exclusion; compare normals in view space to avoid per-tap world transforms. Return the center sample immediately at zero softness. | The same filter kernel and weights; rigid camera rotations preserve normal dot products. |

The distortion clear cache is separate for each graphics context and alternating
frame buffer. Replacing an FBO or releasing its GL handle invalidates reuse. A
nonempty or uncertain bin also invalidates it. Clears temporarily disable the
scissor test and restore its previous tracked value, so a partial clear cannot
be mistaken for an empty texture. Depth replay's color mask is scoped through
the existing OSG StateSet insertion/removal rather than left as an untracked GL
state change. Color-write masking is defined by the
[OpenGL specification](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf).

Static C++ syntax and formatting checks pass for the changed rendering sources
and test sources. Static GLSL validation passed for 48 shadow fragment variants:
GLSL 120/330, normals on/off, reverse-Z on/off, and supported mono/multiview
combinations. This checks shader syntax and types, not driver code generation or
image equivalence. Component regressions cover capped light selection, ignored
lights, empty-view bound avoidance, and removal of cached lights. The shipped
shadow regression also protects its half-resolution R16F targets. Test execution,
engine builds, and runtime checks remain deferred to the user.

When playtests resume, use the same save and view for comparisons. Check water
refraction with blended objects on both sides of the surface, foliage/particles,
distortion entering/leaving the view and resize, dense local lighting, and
exterior shadow edges in first and third person. Compare normal play separately
from diagnostic captures. These source-level reductions do not establish an FPS
gain until those comparisons have been made.

## Lighting spatial index and render-target reuse

The next audit targets the per-object lighting loop used by large mod lists.
Previously, every lit object tested its bounds against every light in its camera
cache. The renderer now builds a balanced bounding-volume hierarchy lazily for
per-object lists of at least 32 lights. Hierarchical bounding boxes allow whole
groups of distant lights to be rejected together; the general method is described
in [PBRT's BVH chapter](https://pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies).
Clustered lighting keeps its existing GPU selection path.

The hierarchy is a conservative broad phase. Every candidate still passes the
original OSG sphere-intersection predicate and ignored-light test. Candidate
indices are restored to the original light-list order before ranking and cache
lookup, including when the count is below the configured limit. The index does
not reduce light radii, change the light limit, or cull scene objects. Double
bounding boxes include a float-rounding margin around tangencies. Non-finite,
invalid, and extreme bounds retain the linear predicate's behavior through an
unpruned fallback. These safeguards are necessary for unusual mod data as well
as normal scene bounds.

The tree is built once for a requested cached camera/frame and has leaves of at
most eight lights. Queries that exceed half the light list or 64 candidates
report budget exhaustion and use the original scan. This limits extra candidate
work and scratch storage in dense overlapping-light scenes; it does not promise
sublinear queries in every scene. Camera storage is retained across updates,
while light positions and the index are invalidated every update. Expired camera
entries are removed. The existing culling threading model is unchanged.

Lighting StateSet lookup also reuses its key-vector storage. `try_emplace` receives
an lvalue key, so cache hits avoid allocating a temporary vector and new entries
receive independent key copies. This targets repeated lighting combinations on
many modded objects without changing the cached uniform data.

Post-processing buffer reattachment now preserves scene/depth/normal/distortion
texture storage when dimensions, source type, and formats are unchanged. Resizes
and format changes still invalidate it; filtering and wrap parameters are still
set explicitly. The existing FBO and intermediate-buffer lifecycle remains in
place. Initial graphics-context dimensions are now assigned before constructing
the HUD, framebuffers, and internal technique; those paths previously read
uninitialized width/height members. This is a separate startup correctness fix;
its performance contribution has not been measured.

Additional regression cases compare indexed selection against a brute-force OSG
sphere reference, covering random distributions, tangencies, malformed/extreme
bounds, dense-query fallback, moving lights between frames, separate cameras,
candidate ordering, and independent StateSet cache keys. Only static compilation
and formatting checks have been performed. Unit-test execution, GPU validation,
and performance comparisons remain deferred alongside playtests. Neither this
index nor the earlier indoor fix establishes the cause of every reported FPS
drop without those comparisons.

## Additional post-processing empty-chain overhead

An empty F2 effect list still includes the hidden `internal_distortion` technique.
Previously, entering that path allocated and cleared three full-resolution scene
textures on each of the two canvases, even though its only pass resolved directly
to the output. At 2880×1800 with RGBA8 storage, those six unused textures account
for 124,416,000 bytes (about 119 MiB), before driver overhead. They remained
resident between frames; the allocations and clears were initialization work,
not six clears every frame. The hidden resolve also sampled scene color and
distortion every frame even when the distortion buffer was empty.

Scene intermediates are now allocated only when a pass writes to them. The scene
draw records whether its distortion bin has content, per graphics context and
frame, accumulating across stereo eyes. The hidden resolve is omitted only after
an empty bin has been observed for that frame. Unknown state, child bins, and
unsorted state graphs retain the resolve. The distortion buffer is still cleared
for other effects that may sample it. With no active effects or distortion, the
canvas uses the same presentation draw as disabled post-processing and allocates
no scene intermediates. Effect mipmap generation follows the declared `mipmaps`
flag, independently of the texture's allocated level count.

These are source-confirmed costs, not a measured explanation of the entire GPU
slowdown. Extra render-target memory can worsen memory pressure; upstream also
tracks [post-processing VRAM overhead](https://gitlab.com/OpenMW/openmw/-/issues/7601).
Neither memory exhaustion nor a driver defect has been established for the
reported A500/Xe case. Normal scene rendering, opaque resolves, and transparent
depth replay also run with post-processing disabled, so their existence alone
does not explain an enabled-versus-disabled difference.

Build and runtime verification must be run by the user. Use the cached build:

```sh
./enhanced/scripts/build-appimage.sh
APPIMAGE_EXTRACT_AND_RUN=1 .build/dist/OpenMW-Enhanced-x86_64.AppImage --openmw --version
```

On each affected GPU, compare the same stationary view in separate launches with
post-processing disabled, enabled with an empty F2 chain, and enabled with the
normal chain. Keep resolution, MSAA, mods, and camera position constant. First
compare FPS without recording, then use the diagnostic command above for any
remaining slowdown. The empty-chain run should report zero intermediates and
presentation timing in a view without distortion geometry. Check a visible
distortion effect too: it must still resolve when it enters the view and disappear
cleanly when it leaves. Verify underwater transitions, multipass effects, resize,
and toggling effects; stereo users should also check each eye. Report GPU renderer,
driver, FPS, and timings before attributing the remaining cost to a driver or
claiming a measured performance improvement.

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
