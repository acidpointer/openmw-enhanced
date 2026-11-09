# OpenMW Enhanced Build Infrastructure

This directory contains fork-owned infrastructure. The goal is to keep upstream
OpenMW easy to update while still letting developers work with normal source
files and Git branches.

## Startup regression audit (2026-11-09)

The rendering changes remain applied. The audit found an unsafe new startup
path: `PostProcessor` queried `glGetInternalformativ` directly from its
constructor. That constructor does not acquire the graphics context, whereas
OSG's [realization code](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6/src/osgViewer/Viewer.cpp)
explicitly makes a context current for realization operations, releases it,
and configures rendering threads before returning. A cached context ID or
extension function pointer does not establish ownership of a current context.

The normal-format probe now runs as a realization operation. It publishes the
result on that specific context, keyed by the requested MSAA count. The
postprocessor reads only this CPU metadata, before constructing either frame's
textures/FBOs. Missing or mismatched metadata keeps the legacy RGB16F format.
The RGBA16F compatibility fallback and all rendering optimizations are retained.

The inability to terminate ordinary startup hangs has a separate established
mechanism: [SDL's SIGINT/SIGTERM handlers](https://github.com/libsdl-org/SDL/blob/SDL2/src/events/SDL_quit.c)
defer termination until event pumping. Bootstrap can block before the event
loop starts. During `prepareEngine`, a scoped guard restores default termination
for those two signals, retaining inherited ignores. On POSIX it preserves and
restores the complete original actions, including masks and flags. Normal SDL
event handling resumes after initialization. Fatal-signal crash-reporting
handlers and the separate exit-crash fixes are unchanged.

The available failed-run log ends during OpenAL initialization, before content
loading and postprocessor construction. Its final line and the later PulseAudio
stream timeout do not identify the blocked instruction. Consequently, the
context-ownership defect is confirmed in source, but attributing that particular
hang to it is not established. These fixes must not be represented as runtime
proof that the complete startup regression is resolved.

Regression sources cover normal-format/MSAA selection, context-local metadata
reads without GL operations, and startup signal restoration, inherited ignores,
exception paths, and preservation of fatal handlers/POSIX masks. Verification
here is C++ frontend/static checks only; no build, test executable, game, or GPU
run was performed, in accordance with `AGENTS.md`.

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
outputs, ccache, Cargo downloads, and release artifacts. `.dockerignore` keeps
these caches and the Git database out of the toolchain image's build context;
the source checkout, including Git metadata, is mounted at build time.

## GitHub release AppImages

The `AppImage release` workflow runs when a release is **published**, including
a prerelease published from a draft through GitHub's UI. Creating a tag or saving
a draft alone does not start this workflow.

| Tag | Release type |
| --- | --- |
| `v1.0.0` | Stable release |
| `v1.0.0-rc.1` | Prerelease |

Tags must have three numeric version components, with an optional `-rc.N`
suffix; numeric components cannot have leading zeros. Other tag formats are
skipped. Before building, the workflow sets the GitHub prerelease flag from the
tag and prevents release candidates from becoming the latest stable release,
including when the build fails. Regular push CI runs on branches and pull
requests; tag pushes use the release packaging workflows.

Push the workflow and build-script changes before preparing the release. In
GitHub's Releases UI, select or create the tag on a commit containing those
changes, prepare your title and notes, and publish it. The workflow builds that
tagged commit with the existing Docker script in Release mode, with Bullet
threading enabled. It attaches these files to the existing release:

- `OpenMW-Enhanced-v1.0.0-x86_64.AppImage`
- `OpenMW-Enhanced-v1.0.0-x86_64.AppImage.sha256`

Prerelease filenames include the full `-rc.N` suffix. The same files are also
available as a workflow artifact for 30 days. A failed build leaves the manual
release available without new assets. Re-run failed jobs to retry transient
failures; a source fix requires a new release tag pointing at the fixed commit.
Reruns keep the original checkout SHA. Before publishing, the workflow verifies
that the release is still published and its tag still resolves to that SHA,
including annotated tags.

Upload retries skip completed assets with matching SHA-256 checksums, upload
missing assets, and remove only failed-upload placeholders. Completed assets
with different content cause an error instead of being overwritten. Prefer
**Re-run failed jobs** after an upload failure to reuse the existing artifact:
rebuilding from scratch can produce different bytes, even for the same commit.
Workflow artifacts can be replaced on a full rerun; release assets remain
available after the workflow artifact expires.

This flow attaches assets after publication, so it requires mutable releases.
If repository release immutability is enabled, it fails before building: GitHub
requires assets to be attached to the draft before an immutable release is
published. No additional repository secret is needed. Only the metadata and
publishing jobs receive `contents: write` through the built-in `GITHUB_TOKEN`;
the Docker build has read access only.

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

## Visibility and post-processing dispatch audit

This pass targets unnecessary work on invisible objects and on screen regions
where the shadow contribution is zero. It does not require another diagnostic
replay to review, and its FPS impact has not been measured.

- The shadow raw pass calculates its existing distance fade before normal
  reconstruction and the 64-step trace. Beyond the authored 7600-unit cutoff,
  the result was already multiplied by zero; the trace is now omitted there.
- The speculative two-stage empty-neighborhood shadow filters were removed
  after a playtest reported 30–40 FPS with post-processing versus 120+ without
  it. Keeping all taps in local arrays across two loops increased live values
  and fetched masks before geometric rejection; no driver measurements had
  justified that tradeoff. The filters again use one loop and sample masks only
  after their original eligibility checks. This removes a regression candidate;
  it does not establish that those arrays caused the reported slowdown.
- Shadow ray samples reject zero depth-hit weights before world-position
  reconstruction and geometric hit tests. Samples that survive reuse the same
  normalized world-depth value for reconstruction and linearization. Filter
  centers also reuse depth for their projected light axis. Existing depth APIs
  retain their signatures; the FX header adds `omw_LinearizeDepth(float)` and a
  sampled-depth overload of `omw_GetWorldPosFromWorldDepthUV`. The input is the
  value returned by `omw_GetWorldDepth`, including its reverse-Z conversion.
  First-person overlay depth is sampled only for candidates that survive the
  ray's depth-hit test or the filters' layer test, rather than for every step.
  The trace still uses 64 steps and the filters retain their original taps,
  weights, target sizes, and formats.
- Cell occlusion skips masked scene children before reading bounds. Eligible
  large and small children receive an ordinary OSG frustum precheck before MOC
  projection, occluder-cache lookup, or static-candidate preparation. This avoids
  processing nodes that the following normal traversal would reject anyway.
  The precheck reads the parent's active planes without modifying current or
  result masks, and leaves disabled culling, invalid bounds, cameras, and absolute
  transforms to their existing traversal. Terrain is not rasterized into MOC
  when the camera traversal mask excludes terrain.
- For P post-processing passes in T active techniques, the dispatcher previously
  performed 2P+T full state applications: one before each technique, one before
  each draw, and one after each pass pop. It now performs P. Stack changes are
  still balanced, and state is applied before every actual draw. Viewports are
  explicitly tracked for scene intermediates, named targets, final presentation,
  and stereo resolve. Initial target clears force full color writes and suspend
  scissoring, so deferred state application cannot leave a partial initialization.
- HDR luminance uses an explicit luminance-sized viewport and a 1×1 resolve
  viewport. This removes its dependence on the inherited presentation viewport
  while preserving normalized sampling. Dirty notifications no longer recreate
  the luminance pyramid when its power-of-two dimensions have not changed.

The latest existing log also revealed an earlier patch error: a `//` comment in
the shadow technique metadata caused `unexpected token </>` and prevented the
shader from loading. GLSL body validation had not checked that metadata syntax.
The unsupported comment was removed; metadata now receives a separate static
lexical check. The activation log also checks validity and GLSL support instead
of reporting an invalid technique as active. This restores the requested shadow
effect; it must not be counted as a performance improvement from disabling it.
The later log confirms the shader is now active and requests normals. Thus the
broken-shader build and the repaired build do not represent identical workloads;
this fact alone does not explain the remaining post-processing on/off gap.

Static checks cover the changed C++ and the shadow's metadata and 48 GLSL body
variants. New occlusion regressions cover node-mask overrides, crossing bounds,
disabled culling, invalid bounds, cameras, absolute transforms, and sibling-plane
mask preservation. Test execution and GPU image/performance checks remain deferred.

## Static candidates for the remaining post-processing degradation

This is a source audit of the current renderer and the installed shader copies
referenced by the user's configured chain. It identifies executable work and
optimization candidates, not measured attribution of the reported 120+ to
30–40 FPS change. No new playtest, build, or runtime check was performed.

The key control-flow finding is that `PingPongCull::operator()` directs the scene
into the primary FBO regardless of the post-processing toggle. The opaque
color/depth blit, transparent depth replay, and first-person depth handling also
remain installed when the toggle is off. In steady state, a genuinely empty
effective chain with no normals/HDR/light requirements reaches the same
`PingPongCanvas::drawImplementation` presentation branch in both toggle states.
These existing scene copies are optimization targets, but their existence alone
does not explain the on/off difference. The optional list also excludes the
hidden internal distortion technique, and `loadChain` reinserts screen-space
shadows on enable when its Enhanced setting is true. The reported case with
shadows disabled must therefore be considered separately from the current
configured chain, whose log confirms `hdr=1 normals=1`.

Candidates, ordered by their relevance to the present configured workload:

| Candidate | Static evidence and scale at 2880×1800 | Scope and next implementation target |
| --- | --- | --- |
| Normals change the cost of scene rendering | `PostProcessor::updateBufferRequirements` enables normals from active techniques. `update` changes the global `disableNormals` shader define; `createObjectsForFrame` attaches a full-resolution RGB16F target to scene and first-person FBOs. Object, terrain, groundcover, and water shaders export normals. `Terrain::createPasses` also enables normals blending for terrain layers. | Affects geometry before effect dispatch and grows with shaded overdraw and terrain layers. Audit normals output and blended terrain passes separately from the ray shader. Format throughput/padding is hardware-dependent; the source does not prove a slow driver format path. |
| God-rays work survives zero ray strength | The installed `godrays.omwfx` calculates `strength` before its `for (i=1; i<=N)` loop, but applies that strength only after sampling and `exp`/`pow` inside the loop. There is a facing test, but no zero-strength test. Default N=20 permits 103.68 million iterations/frame. | Uniform zero strength can reject expensive ray integration before the loop. Preserve the later sun-disc composition; zero ray strength does not justify skipping the entire technique. This candidate applies when god rays are active, not to a chain with that effect removed. |
| Full-resolution work and repeated scene intermediates | The same god-rays shader declares five passes without sized render targets. Its first two passes discard much of a full-size destination rather than declare smaller targets. The configured above-water exterior chain has 13 effect draws: ten full-resolution and three half-resolution, before optional distortion and HDR metering. | The dispatcher uses scene-size scratch targets for unnamed outputs. Introduce explicit pass-size/coverage contracts and resource lifetime planning; changing a shader's packed UV layout implicitly would break compatibility. Pixel-position counts are not GPU timings. |
| Depth of field executes a blur on exactly sharp pixels | The installed `depth_of_field.omwfx` smartblur pass calculates `c=color.a*Rfixed`, then reads noise and all 12 neighbor samples even when c is exactly zero. All offsets are then zero. Up to 62.208 million neighbor fetch instructions/frame are submitted by this pass. | Return the center RGB with the original output alpha when c is exactly zero. A nonzero cutoff would change image quality and is not justified by this audit. Applies only while this effect is active. |
| Hidden distortion resolve is based on draw candidates, not surviving pixels | `DistortionCallback` marks content from leaf/state-graph/child-bin presence. The canvas runs `internal_distortion` fullscreen if that flag is true or unknown. The distortion geometry uses shader depth occlusion, so candidate presence does not establish a visible distortion contribution. | Can remain with an empty optional F2 chain. Conservative screen coverage could restrict the resolve, with displacement-aware bounds; a visibility query must remain asynchronous and must not introduce a GPU wait. |
| Scratch memory can cross a residency threshold | Three scene-size scratch textures per alternating canvas permit six RGBA8 images, approximately 118.65 MiB. Two logical RGB16F normal images add 59.33 MiB, the three half-resolution R16F shadow masks add 7.42 MiB, and the two HDR luminance pyramids add about 10.67 MiB. Driver alignment/padding is additional and formats are selected by the driver. | The full chain can use these allocations even though the empty-chain eager-allocation bug was fixed. Audit scratch sharing across alternating frames per graphics context and release inactive resources while preserving temporal effect targets. Residency pressure is plausible on a large mod list; it is not established by source inspection. |
| Shadow rays perform work with guaranteed zero contribution | The raw loop calculates `distanceWeight=1-smoothstep(0.05,0.92,stepPosition)` after tracing. For i=59 through 63 of 64, stepPosition is always above 0.92, so these five traces contribute zero to both accumulators. All integrated contributions are nonnegative, and the final result saturates once `(integrated/64)*8.5*1.8` reaches one. | Calculate zero weights before tracing and terminate once the existing final result is guaranteed saturated. Keep the original 64-step sequence and normalization. These are specific shadow optimizations, not an explanation of the reported slowdown with shadows disabled. |

The normal attachment's additional bandwidth and render-target footprint are
consistent with the general guidance in Intel's
[Xe-LP optimization guide](https://www.intel.com/content/www/us/en/developer/articles/guide/lp-api-developer-optimization-guide.html)
to minimize render-target count and unnecessary precision/channels. NVIDIA's
[shader guidance](https://developer.nvidia.com/blog/advanced-api-performance-shaders/)
also identifies register pressure and expensive transcendental math as costs;
its [memory guidance](https://developer.nvidia.com/blog/advanced-api-performance-memory-and-resources/)
describes memory budgets, reuse, and render-target residency. These vendor
guidelines support the mechanisms, not a diagnosis of this particular capture;
API-specific D3D/Vulkan advice is not assumed to describe OpenGL behavior.

Lower-priority explanations: changing normal/light requirements stops viewer
threading, regenerates shader sources globally, and rebuilds FBOs, so transitions
can hitch. The guarded code does not do this every steady-state frame. HDR
metering generates a mip pyramid and performs tiny GPU blits, but has no CPU
pixel readback here. The occlusion code does not branch on post-processing, and
the fullscreen effect canvas has depth testing disabled: object occlusion can
reduce scene work, but does not suppress expensive effect pixels. No evidence
in this audit establishes an engine-requested full scene rerender or a loss of
object occlusion solely from switching the toggle on.

## Rendering optimization implementation plan

Status: the implementation below is now present, with terrain batching gated
pending image validation. Static validation passes; FPS and GPU image results
remain unmeasured. Packaging, release automation, crash handling, and the
separate exit-crash work remain outside this change.

Preserve effect selection, user configuration/data precedence, effect settings,
sample sequences, normal-map detail, resolution, and default quality. A faster
result obtained by silently disabling an effect is not acceptance. No further
manual diagnostic replay is a prerequisite for implementation.

1. **Establish one effective execution plan and protect its invariants.**
   Extract chain/environment filtering from the canvas and resource-requirement
   calculation into a shared CPU-side plan. Represent internal distortion and
   automatically inserted shadows explicitly. Distortion eligibility is refined
   at draw time, when its current-frame bin contents are known. Resource needs
   are derived from passes that can execute, not every loaded technique. Reuse
   the immutable plan until chain, environment, or target descriptors change.
   Add regressions for true-empty chains, hidden passes, disabled environments,
   invalid techniques, final output selection, and last-shader/last-pass routing.
   The true-empty case must request no normals, lights, HDR metering, or scratch
   images and must use the existing presentation path. A one-time plan summary
   on changes can expose pass sizes and requirements without per-frame logging.
   Files: `components/fx/technique.hpp`, new FX plan helper, `postprocessor.*`,
   `pingpongcanvas.*`, and component tests.

2. **Remove exact zero-contribution shader work.**
   In the owned shadow shader, calculate the existing distance weight before
   tracing, omit zero-weight steps, and return the already-saturated result when
   the original accumulation expression reaches one. Preserve the 64-step
   sequence, denominator, and remaining sample order. Keep the single-loop
   denoisers and recently added sampled-depth reuse. In the recognized god-rays
   revision, reject zero ray strength before integration while preserving the
   separate sun-disc composition. In recognized DOF, return center RGB and
   alpha=1 before noise and neighbor sampling when radius is exactly zero.
   Do not introduce near-zero approximations. Test shadow bound/accumulation
   math and compare original/optimized shader outputs in automated offscreen
   fixtures when execution becomes available.

   These two mod shaders are not repository-owned active resources. Deliver
   their changes through an isolated, source-fingerprinted compatibility helper
   invoked after normal VFS resolution and before FX parsing. Match reviewed
   source revisions, not filenames alone; preserve technique/uniform names,
   saved values, source provenance, and hot reload. Unknown or edited revisions
   retain the original source. A failed compatibility transformation falls back
   to that original source with a clear diagnostic. No user/mod file is edited
   and no bundled directory is promoted above the user's data order. Keep the
   adapter small and versioned, with idempotence, unknown-source passthrough,
   metadata, and shader-compilation regressions. Record upstream provenance and
   check licenses before including third-party source fixtures. The adapter
   implements reviewed semantic changes; it is not a general GLSL text optimizer.
   Files: owned shader, `components/fx/technique.cpp`, a dedicated compatibility
   helper, and FX tests.

3. **Share transient scene scratch per rendering context.**
   Replace independent scratch sets on alternating canvases with a pool owned
   by the postprocessor and indexed by GL context and compatible descriptors.
   Keep cull/update metadata double-buffered. Share only render-thread scratch
   whose lifetime ends after the consuming draw; do not share scene inputs,
   named persistent targets, temporal history, or HDR adaptation state. Plan
   last-shader and last-pass lifetimes so a pass cannot overwrite a live input.
   For serialized mono rendering, target three scene scratch images instead of
   six, saving approximately 59.33 MiB at this resolution. Contexts, simultaneous
   views, and incompatible sizes/formats receive distinct resources or retain
   the existing allocation path. Refresh on descriptor changes; avoid recreating
   the pool for a mere chain reorder. Initialize allocations deterministically
   and use OSG-managed GL lifetimes on release. Retain bounded inactive reuse to
   avoid allocation churn; never purge temporal named targets speculatively.
   Tests cover overlapping lifetimes, feedback avoidance, two alternating
   canvases, resize, stereo/multiview, and context release/recreation.
   Files: new render-target pool, `pingpongcanvas.*`, `postprocessor.*`.

4. **Scope normals output to cameras that consume it.**
   Replace the runtime global `disableNormals` rewrite with cached shader
   variants selected by an imported, camera-scoped normals-output define.
   Enable writes for scene/first-person targets only when the effective plan
   needs them; disable them for reflection and other color-only cameras. Use
   the existing OSG define mechanism already used for `FORCE_OPAQUE` and
   `DISTORTION`, with explicit inheritance/override rules. Preserve specialized
   sky, effect, preview, and unsupported-hardware behavior. Changing the plan
   should not regenerate every shader source globally. Cache the two output
   variants; first-use compilation remains a possible transition cost.
   Keep the existing RGB16F precision and blended-normal encoding. Check format,
   blending, and MSAA support through cached capability information; RGBA16F is
   a same-precision compatibility fallback, not an assumed performance upgrade.
   Do not switch to packed or reconstructed normals without separate evidence
   that the resulting image meets the existing quality contract. This scopes
   unnecessary exports and transition work; it does not remove the main scene's
   required normals bandwidth.
   Files: `postprocessor.*`, scene shader fragments, camera setup, shader tests.

5. **Reduce terrain layer draws and normals blend traffic.**
   Add a bounded terrain shader variant that batches adjacent compatible layers
   up to the available sampler/instruction budget. Start with small batches and
   retain the one-layer path for unsupported combinations. Evaluate each
   layer's own normal mapping, material lighting, specular, parallax, shadows,
   and fog before reproducing the existing weighted sum. Preserve blendmap UVs,
   depth semantics, vertex colors, and alpha accumulation: the existing
   SRC_ALPHA blending also multiplies source alpha, so alpha cannot simply be
   set to the sum of coverage weights. Write color and normals once per batch.
   Do not hoist nonlinear lighting/clamping across layers without proving
   equivalence. Respect reserved global texture units and keep composite baking
   on its established path initially.

   Per-layer RGBA8 and RGB16F framebuffer rounding means this is not bit-exact
   merely because the algebra matches. Define bounded numerical tolerances and
   automate comparisons for layered slopes, normal/specular/parallax maps,
   fog, water edges, and screen-space shadow boundaries. Enable batching by
   default only after those comparisons pass. Expected structural result is
   ceil(L/K) layer draws for L compatible layers and batch size K, with fewer
   blended MRT writes; no FPS multiplier is promised.
   Files: `components/terrain/material.cpp`, terrain shaders, terrain tests.

6. **Convert recognized packed god-ray masks to explicit targets.**
   After the compatibility helper and resource planning are established, give
   the first two god-ray mask passes explicit R8 targets instead of full-size
   RGBA scratch outputs. Preserve the original stored alpha precision by moving
   that mask to the red channel. The shader currently keeps a 0.55-sized packed
   region, including a guard band around its nominal half-size data. Preserve
   that coverage, pixel centers, sampling footprint, clamp behavior, and the
   ray pass's full-resolution output. Rewrite affected sampler/UV mappings
   together; simply setting width_ratio=0.5 is incorrect. Test odd dimensions,
   screen-edge/offscreen sun positions, and aspect ratios against the original.
   The first two mask destinations should fall from roughly 39.55 MiB to about
   2.99 MiB at 2880×1800, before alignment, while preserving their useful sample
   density. This estimate describes those targets, not guaranteed net pool
   savings; later passes may still require three full-size scratch images.
   Existing explicit render-target declarations should be used first. Broader
   pass-size metadata is justified only by additional consumers.

7. **Restrict expensive distortion sampling to conservative coverage.**
   Accumulate current-frame screen coverage from distortion draw candidates,
   separately per context and eye. Unknown/deformed bounds, near-plane crossing,
   or unsupported child bins conservatively cover the full screen. Expand for
   sampling footprints and any displacement needed by the selected algorithm.
   The internal fullscreen resolve should return the ordinary scene sample
   immediately outside coverage. It must still write every output pixel: a
   scissor alone would leave stale pixels in a scratch destination. Evaluate
   copy-plus-scissor only as a later alternative with an explicit input copy;
   never introduce framebuffer feedback to avoid that copy. Retain the proven
   empty-bin bypass. A previous-frame visibility query cannot justify skipping
   a newly visible current-frame effect; no synchronous GPU query is planned.
   Tests cover clear-after-content, resize, unknown bounds, fully occluded
   candidates, and both stereo eyes.
   Files: `distortion.*`, `pingpongcanvas.*`, internal distortion shader.

8. **Finish render-target and visibility correctness around the new paths.**
   Validate declared sizes as finite/positive and keep derived dimensions at
   least one pixel. Preserve legacy size rounding for valid existing targets.
   Explicitly distinguish transient scratch from persistent named resources;
   a target read before its first write may intentionally contain history.
   Recheck viewport, color masks, scissor, read/draw FBO restoration, and mipmap
   generation for all new target transitions. Keep the current luminance-sized
   and 1×1 HDR viewports and pyramid reuse. Avoid another HDR algorithm rewrite
   until a demonstrated cost justifies it. Keep conservative mask/frustum/MOC
   checks and camera ownership: never use CPU object occlusion as proof that
   neighboring screen-space effect pixels are unnecessary. Broader scene-copy
   elimination or depth-prepass changes need a separate consumer/lifetime audit
   because soft particles and water use these buffers even with effects off.

Implementation sequence: 1 → 2 → 3 → 4; then 5, 6, and 7 as independent
reviewable packages on the relevant foundations, with 8 applied throughout.
No parallel agent work is assumed. Keep unrelated working-tree edits intact.

Verification uses actual FX parsing plus generated GLSL headers, both normal
mode (static parameters emitted as constants) and debug mode (uniforms), GLSL
120/330, normals and reverse-Z combinations, and supported stereo paths.
Meaningful CPU regressions cover planning, aliasing, ownership, and fallbacks;
offscreen reference comparisons cover shader and terrain image semantics.
Structural acceptance includes zero scratch for true-empty chains, no live-input
overwrite, reduced transient allocation, preserved history/settings, no new GPU
waits, and explicit fallbacks. Build/test execution and automated GPU comparison
must run in CI or by a maintainer under the repository's verification rules;
agents perform allowed static checks only. Manual playtests remain deferred.
FPS improvement and the remaining regression's dominant cause remain unmeasured
until runtime evidence is available; this does not block the source work above.

The memory-pool design follows the reuse/lifetime guidance cited in the audit.
Separate read/write targets remain the default: the
[OpenGL texture-barrier specification](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_barrier.txt)
places specific restrictions on framebuffer feedback, and barriers are not a
general license to run neighborhood filters in place.

## Implemented rendering changes and verification

The renderer and canvas now use a shared `Fx::ExecutionPlan` implementation.
Dispatch nodes capture immutable requirements at construction; filtered lists
and requirements are rebuilt on chain/environment/toggle changes rather than
allocating and consulting mutable technique requirements on every draw.
Current-frame distortion eligibility remains a draw-time refinement. Effective
candidate counts and resource requirements are logged when the plan changes.

`PostprocessScratch` shares up to three transient scene images per GL context
between alternating canvases, rather than keeping independent sets of three.
`ScratchRouting` explicitly retains last-shader and last-pass inputs while
selecting a different output slot. Descriptor changes invalidate scratch;
chain reordering alone does not. Named and temporal targets remain separate.
Allocated images are initialized before use, and context resize/release hooks
are forwarded to the pool. The pool retains its bounded allocation for reuse.

Scene shaders select normals output through the camera-scoped
`OMW_WRITE_NORMALS` define. Ordinary RTT cameras default to disabling it, with
an explicit subclass opt-in for a normal target. Normal-requirement changes no
longer regenerate every scene shader globally. RGB16F stays the default; cached
context-realization capability checks can select RGBA16F when RGB16F lacks required
rendering/blending/MSAA support and RGBA16F provides it. This is a precision-
preserving compatibility fallback, not an assumed speed advantage. Query
semantics follow the
[ARB_internalformat_query2 specification](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_internalformat_query2.txt).

The owned shadow shader omits zero-weight ray steps and exits once the existing
result is guaranteed saturated, retaining the original sequence and denominator.
The known-source compatibility helper applies the god-rays and DOF changes
described in the plan. It also initializes the DOF circle of confusion that was
otherwise read uninitialized when out-of-focus blur was disabled. Revisions,
source fingerprints, and normalization are recorded in
`enhanced/patches/catalog/shader-compatibility.toml`. Unknown/edited sources are
untouched; a metadata/parse failure falls back to the original source. Runtime
driver compilation and image equivalence have not been verified. The existing
saved god-rays setting is N=50, not its default N=20; it is preserved, and can
submit up to 259.2 million ray iterations/frame at 2880×1800 before rejection.

God-rays compact masks use the same packed pixel grid and 0.55 guard band in R8,
and keep the ray and sun-disc stages at the original size. Tiny surfaces,
non-UBO contexts, and multiview keep the old mask layout. Resize across the tiny-
surface boundary recompiles the layout. The two mask images occupy about
2.99 MiB at 2880×1800. These explicit images are additional to the shared scene
pool, so their 36.56 MiB reduction relative to two old destinations must not be
added to the pool's 59.33 MiB saving as an independent allocation saving.

Distortion coverage uses conservative projected drawable bounds with padding.
Dynamic/uncertain bounds, child bins, unsorted state graphs, and stereo use the
full-screen fallback. Outside known coverage, the internal effect writes the
original scene sample without further distortion fetches. Every output pixel
is still written; there is no scissor-only stale-output path or visibility wait.

Terrain has an experimental two-layer batch path for adjacent ordinary diffuse
layers. Normal-mapped, specular, parallax, ESM4, composite, and sampler-budget
incompatible cases retain their original passes. The shader preserves weighted
color/normal sums and squared-alpha accumulation. It stays disabled by default
because per-layer framebuffer rounding needs automated GPU image comparisons.
The explicit experimental setting is `[Terrain] batch texture layers = true` in
`openmw-enhanced.cfg`; no user setting is changed automatically.

Static scene validation also exposed and fixed a duplicate `viewNormal`
declaration in the unlit BS shader with soft particles and normals both enabled.
Derived render-target sizes are now at least one pixel, invalid/overflowing
sizes are rejected, and explicit targets keep non-power-of-two dimensions.

Verification performed:

- C++20 syntax checks pass for all changed C++ implementations and new test
  sources. Formatting and whitespace checks pass for the changed code.
- The reusable `python3 enhanced/scripts/check-rendering-shaders.py` checks the
  owned effect shaders and representative scene-fragment feature combinations.
  Supply `--godrays <original-godrays.omwfx> --depth-of-field <original-depth_of_field.omwfx>`
  to also validate the reviewed compatibility revisions. That mode uses C++20
  constant evaluation of the actual compatibility implementation to verify the
  generated source fingerprints before validating GLSL. It requires GCC and
  `glslangValidator`, creates no executable, and does not launch the engine.
- With the reviewed mod sources, 276 effect-stage variants and 52 scene-fragment
  variants pass static GLSL checks, covering constants/uniforms, reverse-Z,
  normals, supported effect multiview syntax, and the gated terrain batches.
  The legacy EXT_gpu_shader4 feature bodies are additionally checked as GLSL
  330 core-language features because this validator lacks the extension.
- New component regressions cover environment filtering, empty/disabled plans,
  live scratch input exclusion, context isolation, descriptor invalidation,
  source passthrough, real FX parsing of an R8 target, dimension edge cases,
  terrain eligibility, and projected coverage fallbacks. Test binaries were not
  built or executed, as required by `AGENTS.md`.

Engine builds, automated GPU image comparisons, MSAA/stereo driver checks, and
performance measurements remain pending. No new user replay was requested.

## Occlusion traversal audit and changes

The November audit found missing rejection paths, independently of the remaining
post-processing slowdown. The latest available log confirms the earlier FX
compatibility and execution-plan changes were loaded; their presence does not
establish an FPS benefit. No new timing capture was requested.

- The adaptive policy previously disabled both static occluder construction and
  object visibility queries for 120 frames after two low-benefit samples. With
  no terrain indoors, this could remove virtually all useful occlusion. It now
  reduces the static preparation/raster budget while keeping fresh occluders and
  visibility tests active. Explicit user enable/disable settings still apply.
- Large cell objects were traversed before static depth was complete and were
  exempt from the second visibility pass. All eligible object sizes now wait
  until the cell's occluders are rasterized, then test before descending into
  lighting callbacks, skeleton traversal, skinning, and render submission.
- A visible paged chunk previously admitted all its drawables, even those hidden
  by buildings. Paged models and ordinary static model instances now receive
  drawable rejection after geometry merging. Merged batches remain intact;
  their bounds, rather than every constituent triangle, determine visibility.
  Callback-bearing drawable copies own their user data and retain the immutable
  source arrays' lifetime without modifying shared resource templates.
- Groundcover chunks now test their conservative animated/instanced bounds
  before building per-object light lists or submitting grass.
- Main-camera cell traversal now keeps the terrain/paging phase first, then
  visits cells near to far. Update traversal and other cameras keep graph order.
- Camera-containing and near-plane-crossing model bounds were rejected during
  occluder selection. Opaque wall triangles remain useful from inside a room.
  Candidate projection now permits those bounds with conservative importance;
  rasterization still clips the original triangle subset against all MOC clip
  planes. Visibility queries retain their near-plane fail-open behavior. The
  former inside-threshold setting is retained as a disabled legacy launcher
  field. Existing distance, radius, screen-size, time, and triangle limits still
  constrain occluder selection.

The drawable callback uses OSG's boolean `Drawable::CullCallback` contract. A
generic node callback on a drawable cannot suppress the render leaf: OSG calls
it and then continues submission, as shown in
[CullVisitor::apply(Drawable&)](https://github.com/openscenegraph/OpenSceneGraph/blob/master/src/osgUtil/CullVisitor.cpp).
Clipping and conservative depth semantics follow the vendored
[MaskedOcclusionCulling API](https://github.com/GameTechDev/MaskedOcclusionCulling/blob/master/MaskedOcclusionCulling.h).
Uncertain bounds, explicit exclusions, disabled culling, absolute transforms,
and unrelated cameras remain visible. Hidden light origins are not removed
from update-time light collection: they can illuminate visible surfaces.
Gameplay, AI, and animation-event simulation continue independently of rendering.

New regression sources inspect actual OSG render leaves and nested callback
execution, including hidden versus visible drawables, transformed bounds,
frustum misses, callback cloning, shared-template ownership, camera isolation,
cell order, adaptive budgets, self-occlusion, and camera-containing occluders.
Renderer implementations and these test sources pass C++20 syntax checks.
Tests and the engine have not been built or executed under the repository's
agent rules; FPS improvement and visual correctness remain unverified.

## Drawable-count regression: research and implementation plan

The latest user comparison is the same view with post-processing toggled:
approximately 1,400 fast drawables at 100–110 FPS with it off, versus 6,000 at
56 FPS with it on. This is roughly 4.3 times the counted geometry entries and
an increase from 9.1–10.0 ms to 17.9 ms per frame. It is the primary regression
to explain. It does not by itself establish which thread or GPU stage dominates.

OSG's [RenderBin statistics implementation](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6/src/osgUtil/RenderBin.cpp)
counts geometry render leaves; it does not report unique world objects or exact
OpenGL draw calls. [RenderStage statistics](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6/src/osgUtil/RenderStage.cpp)
also include dependent pre/post camera stages. Conversely, manually replaying a
render leaf inside a draw callback does not create another leaf for this count.
Both counted geometry and extra draw-time replays therefore need attribution.

### Findings and confidence before implementation

| Finding | Source | Assessment |
| --- | --- | --- |
| Main and terrain-only software depth are cleared every frame; candidate collection and rasterization share wall-clock budgets. Adaptive throttling still reduces that budget. | `OcclusionCuller::beginFrame`, budget predicates, `SceneOcclusionCallback` | Confirmed timing-dependent coverage; a candidate for the toggle regression, not a proven explanation. |
| Paged chunks rasterize their own occluders and immediately traverse their drawables. Later chunks/cells have not contributed their walls yet. Near-to-far ordering of cells does not fix ordering inside the paging phase. | `PagedOccluderCallback`, `CellOcclusionCallback`, `QuadTreeWorld::accept` | Confirmed incomplete coverage at submission time. |
| The paging analyser calls `traverse(child)` for selected Switch/LOD/Sequence children, bypassing dispatch on the child itself. Direct geometry children can be missed. It also fails to restore inherited material state across siblings and ignores a geometry's own StateSet when counting materials. | `ObjectPaging::AnalyzeVisitor` | Confirmed inaccurate batching estimates; independent of the PP toggle. |
| The compositor has one world-root child and two alternating fullscreen canvases, one active per frame. Ordinary FX passes draw through the canvas rather than attaching another world scene. | `PostProcessor` construction/update/traverse, `PingPongCanvas::drawImplementation` | No direct world-duplication mechanism identified here. Preserve and test this invariant. |
| The transparent depth replay is configured independently of runtime PP enablement, and reflection cameras use separate views without main-view MOC. | `TransparentDepthBinCallback`, `Reflection`, `SceneOcclusionCallback` | Potential additional cost/coverage gaps; neither alone proves the reported toggle-dependent count increase. |

The earlier culling fixes remain useful foundations. They do not establish that
the regression is fixed, and a larger architectural change must not be described
as the measured root cause without a reproducing regression test.

### Primary change: prepare visibility before submitting world geometry

1. **Publish immutable ready occluders.** Maintain placement records for eligible
   nonpaged objects and paged chunks, containing stable instance identity,
   immutable mesh handles, conservative bounds, transform, masks, and revision.
   Register/unregister on insertion, replacement, disable/enable, movement,
   cell changes, and paging revision changes. Resolve worker completion outside
   the inner per-object cull loop. Avoid repeated model-path construction,
   ready-cache locking, and mesh lookups for unchanged placements. Missing or
   invalid data stays visible. Do not retain live scene instances in build jobs.

2. **Separate view preparation from drawing.** Split `QuadTreeWorld::accept`'s
   selected-entry preparation from its `entry.mRenderingNode->accept` loop.
   Use the existing `ViewData` selection and world-update revision to expose
   only the chunks selected for this view, including current masks, LOD, active
   grid, disabled references, and paging replacements. Combine their ready
   occluders with eligible cell objects. Rasterize terrain and a globally ranked
   static set before accepting the first world drawable. This is CPU software
   depth preparation; it must not issue another GL world draw or run a second
   cull traversal that performs skinning, lighting, or leaf submission. Keep
   intersection visitors and water-height selection semantics intact.

3. **Make the completed depth read-only during world traversal.** Paged and cell
   callbacks become visibility consumers rather than incremental producers of
   static depth. Retain the existing early node and drawable rejection, including
   groundcover bounds. Test hidden subtrees before nested lighting callbacks or
   skinning. Preserve doors/exclusions, conservative original-triangle occluders,
   near-plane visibility fallbacks, and gameplay/update-time light collection.
   This follows MOC's [front-to-back and conservative-coverage guidance](https://github.com/GameTechDev/MaskedOcclusionCulling).

4. **Stabilize selection and reuse valid software depth.** Use a latched, bounded
   occluder plan with deterministic priorities and tie-breaking. A later timer
   sample must not discard an already valid plan for the same unchanged view.
   Candidate preparation and optional enrichment can be deferred; preserve the
   user's radius/distance/triangle limits and the raster time target, including
   the existing possibility that one bounded operation finishes over target.
   Do not set the budget to unlimited or introduce unbounded mandatory work.
   Reuse completed software depth only when the camera/view/projection, clip
   planes, viewport, near clip, software-buffer dimensions, worldspace, settings,
   selected LOD/chunks, placements, and occluder-data revisions match exactly.
   HDR/normals/compositor enablement and alternating framebuffer identity are
   not visibility inputs and must not invalidate this cache. Invalidation must
   precede querying moved, disabled, removed, or replaced occluders. NPC movement
   still produces fresh occludee queries. Uncertain invalidation means rebuild
   or fail open, never stale depth that can hide visible geometry.

View ownership must use camera/view identity and matrices, rather than a camera
name plus frame number. Keep main, reflection, shadow, and stereo-eye ownership
separate. Additional views require their own valid depth or a conservative
fallback; never reuse main-camera depth to cull reflected geometry. Bound cache
retention and keep ownership free of scene/callback cycles. Preserve the separate
exit-crash work and normal configuration/save locations.

### Post-processing contract and automated acceptance

Add a small render-stage inventory to the regression infrastructure. Attribute
world leaves, distinct instances, repeated submissions, material/primitive sets,
fullscreen geometry, and explicit transparent replays to their camera and pass.
Record culling bypass reasons in optional statistics. These checks accompany
the structural fixes; they are not a request for another manual debug capture.

For fixed camera, scene, selected LOD, and warmed ready meshes, main-world leaf
identities and multiplicities must remain equal across PP off, enabled-empty,
identity effect, HDR, normals-requiring effect, and repeated off/on transitions.
Hold visibility inputs constant and exclude intentional sky-sunglare changes
from the world comparison. Check both alternating framebuffer slots, MSAA
on/off, nested cameras, and supported stereo modes. Fullscreen effects may add
their own draws, but must not attach additional world traversal or silently
change culling masks, frustum, near/far settings, LOD, or batching.

Use the inventory to distinguish two failure modes. If world leaves increase,
fix the visibility/selection or traversal ownership that admitted them. If only
dependent-stage totals increase, fix the stage lifecycle or unintended scene
attachment responsible. If counts are unchanged but draw calls increase, audit
the explicit replay paths. Do not remove a legitimate reflection or change HUD
accounting merely to make the reported number smaller. The source audit has not
yet identified which of these explains the user's complete 4.3x increase.

Keep scene traversal, visibility ownership, and framebuffer routing distinct in
the frame plan. Effect requirements may select attachments and shader variants;
they must not alter the world graph. Exercise actual cull visitors/render leaves
in component fixtures. Add an offscreen integration fixture for the real
compositor and render callbacks, because tests of flags alone cannot establish
the traversal contract. Maintainer/CI execution is required by `AGENTS.md`.

### Separate drawable-reduction and replay changes

Correct the paging analyser using scoped inherited state, geometry-local state,
and `accept` on selected children. Match the paging copy rules for masks, LOD,
Switch, Sequence, and supported static source geometry. Test sibling-state
restoration and direct geometry under each selector. Then evaluate actual
merge-compatible opaque batches within bounded spatial chunks. Preserve
transparent sorting, dynamic controllers, refnum picking, primitive attributes,
template ownership, and conservative bounds. Avoid merging a whole area into
one enormous drawable: that defeats the occlusion work above. This package
reduces baseline submission work; it is not a substitute for fixing the toggle
invariant.

Audit transparent depth replay and scene-color/depth copies against their real
consumers, then gate only passes with no consumer in the effective frame plan.
Water, distortion, first-person depth, and soft particles still have consumers
with PP disabled, so blanket removal is unsafe. Account for water straddlers
and other draw-time repeats separately from OSG's leaf statistics. Leave further
shader tuning and experimental terrain batching behind these structural tasks.

### Delivery order and completion criteria

Prepare the stage/visibility regression fixtures first, then deliver the ready
registry, complete CPU occluder phase, and stable-plan/cache invalidation as one
coherent change. Pair that with any compositor/view-ownership repair revealed
by the invariance fixtures. Deliver the analyser/batching correction separately,
followed by consumer-proven replay elimination. Keep each package reviewable;
do not ship another counters-only patch as the performance fix.

Acceptance requires stable world submissions under PP toggles; no second world
submission from the preparation phase; correct removal/movement/LOD/cache
invalidation; preserved visible images, particles, water and first-person depth;
bounded CPU work and cache memory; and no added GPU readback or synchronization.
The same hidden-geometry scene must cull correctly with occluders in later
chunks/cells and with different traversal insertion orders. Test reused and
rebuilt software depth against equivalent fresh-depth visibility results.

Agents perform static review and allowed C++/GLSL frontend checks only. CI or a
maintainer must execute component fixtures, offscreen image comparisons, and
fixed-scene performance comparisons on the target GPU classes. Manual replay
collection remains deferred. Counts and source invariants can establish the
structural repair; restored FPS remains a separate runtime acceptance result.

### Implemented structural repair

The renderer now prepares selected terrain/paging entries and eligible cell
occluders before submitting world geometry. This uses a metadata visitor sharing
the real camera's `ViewData`, masks, and distance LOD selection. It is not another
cull traversal: it does not invoke skinning or light-list callbacks and cannot
submit render leaves. Cell and page cull callbacks consume the resulting depth;
hidden paged roots can be rejected before their nested lighting callbacks.
Whole-cell rejection retains terrain-only depth and explicit object exclusions.

Cell sources use a placement registry with weak scene references and shared
immutable ready meshes. Membership checks inspect the object's parent list,
avoiding a scan of the whole cell for each source. Ready placements avoid repeated
model-path construction and storage lookups. Removal, disabled masks, movement,
fading, missing/replaced visual roots, and changes to the selected source set
invalidate coverage before visibility queries. Page proximity queries use a
conservative BVH and distance to mesh bounds, retaining large nearby walls whose
centers lie outside the distance limit.

A globally ranked plan replaces traversal-order rasterization. An exact view,
terrain revision, settings, and placement-set match retains valid software depth.
Incomplete plans accumulate coverage on subsequent unchanged frames instead of
discarding it because a later timer sample ran out. The triangle cap remains
hard; the time target permits one bounded mesh operation to make progress and
defers subsequent operations when exhausted. Cache retention is limited to the
last owned view, and ownership guards prevent another camera from querying it.
Framebuffer slot, HDR, and normal-output format are not visibility inputs.

Effect-chain reload now refreshes effect targets without resizing the main
camera or rebuilding its projection. Actual window resize still performs those
operations. Current canvas passes determine the frame's depth consumers. The
separate world-depth copy is omitted when unused; transparent replay is omitted
only when effects, later geometry, and unknown cameras have no consumer. The
first-person depth replay remains intact. Optional rendering statistics separate
world submissions, dependent-camera geometry, and explicit depth/water replays.

The paging analyser now dispatches selected children with `accept`, restores
inherited material state across siblings, and includes geometry-local state.
This repairs estimates used by existing spatially bounded opaque merging.
Occluder construction also excludes update-only hidden/collision geometry, which
must never close visible openings. The `mesh-v4` cache key automatically prevents
reuse of older proxies; ready geometry is regenerated without manual deletion.

Regression sources exercise the production cell/page callbacks and real OSG
render leaves, including a later cell's wall hiding an earlier actor, traversal
order, zero preparation submissions, placement invalidation, progressive depth,
view/settings changes, nested stages, and output-format/normal-define changes.
They also cover selector/material analysis, nearby large walls, hidden geometry,
and incompatible cached proxies. These fixtures are CPU/source coverage; they
do not instantiate the full compositor, validate GPU images, or establish the
entire PP toggle contract for MSAA/stereo.

Verification performed here is limited to C++ frontend checks and static diff
checks, as required by `AGENTS.md`. No build, component-test binary, engine, or
GPU run was executed. Component-fixture execution, real-compositor offscreen
comparisons, and target-GPU performance acceptance remain pending. These source
repairs do not establish that the reported 4.3x drawable increase or FPS loss is
fully resolved. Manual debug replay collection remains deferred.

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
