# OpenMW Vulkan renderer implementation plan

Implement a second game renderer with Vulkan and retain the existing OpenGL
renderer. Share gameplay, content, mods, configuration, and saves. Renderer
selection will apply at the next game launch.

Implementation proceeds directly on the renderer. Efficiency comes from
persistent GPU resources, suballocation, instancing, batched uploads, cached
state, conservative visibility, and precise synchronization. No benchmark
framework or benchmark prerequisite is part of this work.

## Current implementation

The implementation contains a native Vulkan module in
`apps/openmw/mwrender/vulkan/`, API-independent contracts and module ownership
in `components/render/`, and an OpenGL frame lifecycle adapter in
`apps/openmw/mwrender/opengl/` used by the engine.

The Vulkan module implements device and presentation setup, indexed textured
rasterization, instanced batches, depth testing, alpha testing, alpha/additive
blending, face culling, mipmap generation, memory suballocation, resident mesh
and texture handles, asynchronous uploads, descriptor reuse, and deferred
resource destruction. The native material pipeline consumes normals, all six
vertex-color material modes, ambient/diffuse/emissive/specular properties,
shininess, the sun, and the CPU-selected point lights. Vertex lighting preserves
classic/radius attenuation and negative lights while avoiding a per-fragment
light loop on low-end hardware. Material controllers remain live through their
uniforms; texture color is combined with specular contributions separately.

The backend-independent `openmw-render-core` target now prepares persistent
model instances, shares uploaded meshes and image/sampler resources, applies
changed transforms without reuploading meshes, culls bounds separately for
each view, groups opaque draws, and sorts transparency by camera depth. Resource
ownership unwinds partial uploads and rejects stale instance handles.

`ResourceSystem::getRenderModelManager()` loads CPU models before GL shader
rewriting. Its image manager bypasses GL capability checks. The static importer
preserves hierarchy transforms, primary UV transforms, sampler state, vertex
colors and floating-point alpha, authored DDS mip chains, and material alpha
comparisons. Vulkan accepts BC1 through BC5 data where supported; DXT fallback
decoding occurs only when a device cannot use the source compression. The
importer rejects animated/view-dependent nodes, additional texture stages,
and other unsupported material operations explicitly. These remain part of
the subsequent animation and material work.

The launcher now persists `[Video] renderer = opengl|vulkan` in the existing
user settings. Vulkan selection creates a native SDL Vulkan game window and
installs `VulkanFrameDriver`. The adapter consumes the existing CPU cull result
and submits cached visible geometry directly to Vulkan. Skinning, morphing,
terrain selection, and gameplay transforms still run through existing CPU OSG
services. Unchanged geometry remains resident; changed vertices update resident
buffers without reuploading unchanged indices. Live inherited bindings are
borrowed from the cull graph, and material parameters are decoded once per
state graph per prepared frame. UV animation stays in instance data.

MyGUI emits its existing clipped and ordered CPU batches to the native renderer.
The viewer's rendering entry point is preserved, so loading screens, videos and
modal loops use the same Vulkan path. Font/video row order is preserved and
ordinary RGBA uploads copy rows directly. The game window title identifies
Vulkan; startup logs report the actual Vulkan device. The existing launcher GPU
selection is matched by PCI address when requested. Resize, high-DPI input and
VSync changes route to the selected backend.

This is an experimental visible integration, not feature parity. A hidden
16-by-16 GL compatibility context initializes capability-dependent legacy scene
services. It neither presents nor draws the visible game scene, but Vulkan
startup still requires that context until those services are migrated. Native
offscreen color cameras now submit pre-render and post-render stages in dependency
order. Inventory portraits and local maps use persistent native target images;
target contents survive camera inactivity until their source texture expires.
Target rows preserve GL texture sampling orientation while the displayed window
uses a negative-height viewport. Clear/load passes preserve incremental target
contents, and sampled-target transitions and deferred ownership cover multiple
frames in flight. MSAA resolve semantics, full shadowing/material and OMWFX
parity, and stereo remain incomplete.

Terrain uses diffuse layers with independent mask/tiling UV transforms, authored blend
weights and equal-depth tests. Distant chunks retain the terrain composite cache:
the native driver bakes the authored albedo/weight quads into a Vulkan target
before any dependent scene camera, then reuses one terrain layer per chunk.
Composite readiness is committed only after successful frame submission; CPU
bake drawables are released afterward. The GL composite compile queue is bypassed
for native chunks. Nearby terrain still uses its authored material layers.
GL layer batching remains disabled for Vulkan. Animated water interprets its normal texture as a normal
field and samples its native reflection target with projected coordinates, wave distortion and Fresnel
weighting. Reflection clipping consumes the positioned source clip plane without
requiring optional hardware clip-distance features. Opaque color/depth snapshots,
full refraction and ripple simulation remain outstanding. OMWFX underwater
pixel effects now have native depth and 3D water-normal inputs.
Linear distance fog is handled natively. Source GL programs and custom GL draw
callbacks are not executed by the Vulkan adapter. Unsupported non-triangle diagnostic geometry is reported
once and omitted. These limitations must remain explicit while the remaining
work packages are implemented.

Native screenshot readback serves saved-game thumbnails and the screenshot key.
It avoids waiting for GL-only draw-completion callbacks. GPU frame fences and
image acquisition use bounded waits; an acquired image is retained across a
retry so its binary semaphore is never reused while signalled. Culling/import
is skipped while the next GPU frame is busy. Loaded mesh/image assets remain
cached within device-scaled residency budgets; dead sources and cold assets
under memory pressure retire incrementally. Temporary UI/state caches retain
their short interval. Empty allocation blocks are trimmed after completed frames. Legacy GL shadow and clustered passes remain disabled. Native color-target
reflection cameras participate in frame preparation and submit Vulkan passes.
The Vulkan front-face choice follows its signed polygon-area rule for each
viewport orientation. The subsequent playtest exposed a separate depth-ordering
error: OpenMW intentionally keeps a standard CPU culling projection and supplies
the actual shader projection through the effective `projectionMatrix` uniform.
The native bridge had incorrectly treated the culling projection as reversed
zero-to-one depth whenever reverse Z was enabled, making distant surfaces win a
forward less-depth test. Native drawing now uses the effective shader projection
and converts it to forward zero-to-one depth; the culling-only fallback always
uses the standard minus-one-to-one conversion. Regression fixtures cover depth
ordering across the near/far interval, matching normal/reversed projections,
and orthographic map views. Visual acceptance and FPS remain user-verified.

Repeated references to a source render stage submit that camera only once per
native frame, matching OSG's stage-drawn guard. This preserves independent
cameras and legitimate terrain material passes; it does not deduplicate models
that occur at different placements or replace the existing visible-scene cull.

Native particle submission consumes the CPU particle state and retains the
existing NIF freeze-when-culled frame bookkeeping. Each view gets its own billboard
orientation, material/atlas colors and alpha, and requested depth sorting.
Mesh capacity and index topology are retained as the live particle count changes.
Configured alignment axes, scale reference frames, LOD and visibility distance
are retained. Line and point particles use triangle-based approximations; weather-specific
custom drawables and groundcover instancing remain separate follow-up work.

The user confirmed the initial module build and normal OpenGL gameplay. The
asset/scene and visible integration slices have C++ frontend checks and fixture sources
for shared uploads, failure cleanup, independent camera visibility,
transparency order, texture/mip validation, static import, image decoding,
Vulkan signed-area facing, depth ordering/projection selection, degenerate
normals, material color modes and particles.
Stage sorting and dependency ordering were checked against the upstream
[OSG RenderBin](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6.5/src/osgUtil/RenderBin.cpp)
and [RenderStage](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6.5/src/osgUtil/RenderStage.cpp)
implementations. Particle sizing, UVs, alignment and timing were checked against
[OSG ParticleSystem](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6.5/src/osgParticle/ParticleSystem.cpp).
Shader interfaces and GPU resource transitions have received static review;
shader compilation, fixture execution and visual acceptance remain unverified.
Build and fixture execution for this slice remain user-operated under
`AGENTS.md`.

The native upload path now packs meshes, indices and texture mip chains into
persistently mapped upload pages. Each slice is aligned for the supported copy
formats, and pending uploads and in-flight frame ownership prevent page reuse
while the GPU can still read it. Completed work retains each page once per frame;
idle oversized pages are released. Changed meshes promote to a CPU stream that
retains source-vertex/primitive-set mappings and index topology, so pose changes
update vertex data without rebuilding triangle lists or hash maps. Static meshes
do not retain the extra stream data. Frame descriptor sets are rewritten only
when storage buffers grow, and light-buffer capacity grows geometrically.

Swapchain framebuffer/image recreation preserves compatible graphics pipelines
and render passes. Window pipelines are invalidated only when their attachment
format changes; offscreen pipelines remain independent. A `VK_SUBOPTIMAL_KHR`
result schedules a throttled surface-capability/format refresh instead of forcing
recreation every frame. Out-of-date images, changed extents/transforms and explicit
presentation changes still recreate the swapchain. This follows the distinction
between usable suboptimal presentation and invalid presentation in the
[Khronos swapchain documentation](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain.html).
Small buffers/images remain suballocated when dedicated memory is merely
preferred. Required dedicated allocations are always honored, and preferred
allocations are used for resources at least as large as their normal pool block,
consistent with the optional preference described by
[Khronos dedicated requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryDedicatedRequirements.html).

The ordinary game log reports `Vulkan slow frame:` at most once every five seconds
when a native frame or the interval outside native rendering exceeds 250 ms, or
submissions stop progressing while the window is not minimized. It records
readiness, culling, preparation, submission, retirement, upload counts and the
previous submitted draw count. These are CPU phase timings and progress warnings;
they do not measure GPU execution time. The latest playtest log established that
retirement consumes 1.7–3.4 seconds during repeated hangs, while culling takes
2–4 ms and submission takes about 1 ms. Source inspection identified real OSG
StateSet snapshots registering thousands of parents on shared uniforms and
attributes, making their removal quadratic. Native snapshots now retain the
bindings directly without registering OSG parents; mutable uniform values remain
live and override/protected rules are preserved. Retirement rotates through one
cache per frame with a 1 ms budget and a stable key cursor. Slow-frame warnings
also identify the cache and number of retired entries. A single resource
destructor can still exceed the budget; the user must confirm the improvement.

The active PingPongCanvas now supplies its parsed OMWFX dispatch chain to the
native driver. Supported pixel passes redirect scene color to an offscreen
target, execute in the existing scratch-routing order, and resolve to the window
before MyGUI. Technique environment flags, live settings/Lua uniforms, named
target dimensions, clear colors and history are retained. Native inputs include
color, sampled forward depth, view-space normals, distortion, custom 2D/3D textures and floating-point HDR
targets. Engine projection uniforms are converted back to the ordinary GL
projection expected by OMWFX depth reconstruction. HDR exposure uses a small
GPU log-luminance reduction and two persistent exposure targets with the
configured adaptation speed. This samples a 64-by-64 grid rather than reproducing
the GL full-resolution mip reduction exactly. Effects use the rendering
projection captured before model/view transforms, including authored overrides.

Generated OMWFX API headers are translated to Vulkan GLSL, linked to SPIR-V with
the bundled glslangValidator, and reflected for descriptor/uniform offsets.
Compilation is cached by source and runs outside the rendering thread; a single
compiler process at a time avoids a CPU surge. Pending effects wait without
blocking scene rendering. Native descriptors and small uniform uploads reuse
per-frame pools/pages. Feedback against an active attachment is rejected, and
GPU ownership lasts through frame fences. Shader compilation errors and
unsupported features are logged once instead of being silently ignored.

This is partial OMWFX compatibility: mipmapped effect targets,
compute/geometry passes, 1D/array/cube samplers, arbitrary pass blending,
multiview and a Windows compiler launcher remain outstanding. Such techniques
are skipped with explicit diagnostics. Packaged Linux builds carry the compiler
and its dependencies. Runtime compilation and effect output require user
verification; agents have not run shaders, builds, tests or the game.

Upload-page fixtures cover lifetime, cancellation,
alignment, retirement and overflow; animated-stream fixtures cover stable topology,
pose/color changes and independent mask UVs. New fixtures cover parentless state
snapshots, incremental cache sweep resumption, generated GLSL translation, and
SPIR-V matrix/array/volume descriptor layouts. Fixtures have received frontend
checks only under the repository's build restriction.

The subsequent playtest confirmed that camera control improved after the
parentless-state fix, with remaining hitches concentrated on rotation. Source
inspection found that the native mesh/texture caches still evicted loaded assets
after only 16 unseen frames. These caches now retain assets while under soft
residency budgets, scaled to the largest local-memory heap or the lower reported
VK_EXT_memory_budget allowance: geometry uses 1/16 (32–512 MiB), textures use
1/8 (64–2048 MiB). Dead sources are reclaimed
regardless of budget. Under pressure, only assets unseen for more than 32 frames
are eligible, preserving the active working set. Visible/in-flight resources
and render targets are not hard-capped by these cache limits. Payload bytes
include generated texture mip levels, and charges follow resource lifetime.
Repeated uses of a mesh or texture binding now share one revision/sampler lookup
per prepared frame, while material transforms and per-instance data stay live.

When VK_EXT_extended_dynamic_state is available and its feature is supported,
face/culling and depth test/write/compare state are dynamic. This reduces basic
material variants from 480 to five per attachment format without changing
blend modes, draw order, or depth/facing semantics. Adjacent draws are instanced
only when their complete raster state also matches. Older devices retain the
static Vulkan 1.1 path. This uses the capability described by
[Khronos extended dynamic state](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_extended_dynamic_state.html).
Pipeline caches now persist under the normal user cache directory, with header,
vendor/device and driver cache-UUID validation, bounded reads and atomic writes.
Cache errors fall back to an empty cache; writes happen during teardown.

The latest ordinary game log also showed every effect compiler invocation
printing usage instead of compiling. glslang's variadic descriptor-mapping
option consumed the trailing shader filenames. Inputs now precede those
options, matching the parser in the
[glslang implementation](https://github.com/KhronosGroup/glslang/blob/14.0.0/StandAlone/StandAlone.cpp).
The next playtest exposed an uninitialized clip matrix in the final fullscreen
resolve: once an asynchronous effect became ready, the world was redirected
offscreen and its presentation triangle disappeared. Draws now default to an
identity clip transform, and the resolve assigns it explicitly. Scene redirection
and the corresponding window resolve are published together. Shader samplers
and the engine block now carry explicit GLSL set/binding layouts instead of
using CLI resource-name overrides; generated global blocks retain a separate
reserved binding. This prevents anonymous block names from producing conflicts
between stages. Legacy HDR `varying` and fragment-output spellings are translated
without editing effect files. Fixtures cover the default fullscreen transform,
legacy shader compatibility and matching descriptor layouts across stages.
Fixtures cover this argument boundary, residency pressure/accounting, dynamic
pipeline key reduction and foreign/truncated pipeline cache rejection. All new
fixtures receive static frontend checks only. The optimizations and effect
compilation/output still require ordinary user playtesting; no runtime
verification or benchmark framework has been introduced.

The next rotation-stutter pass shares native mesh storage across OSG hierarchy
clones with identical vertex/index sources and revisions. Instance transforms
remain separate, and adjacent matching draws can now use the same instanced
batch. Modified geometry detaches from canonical storage before uploading;
animated streams then update their own buffers. Source observers and revision
checks prevent dead objects or reused addresses from matching old meshes.
Canonical GPU storage stays cached within its residency allowance even when the
last scene clone disappears; keeping only a weak storage reference used to lose
these uploads before a later clone could reuse them. Stale source revisions
retire incrementally, and memory pressure can retire unused canonical storage.
Residency charges follow shared storage, and records retire within the existing
incremental cache sweep. These changes remove duplicate imports/uploads from
camera-dependent discovery; their effect on frame pacing is not yet measured.

F3 now selects a native MyGUI/Vulkan overlay instead of asking the OSG stats
handler to create an invisible GL slave camera. Its five states show off, FPS,
submission-interval history and CPU rendering phases, cache residency, and
engine phases. The history tracks successful submissions, including elapsed
time across skipped fence polls; it is not GPU timing or display scanout timing.
The graph keeps fixed topology and uploads only changed vertices. The subsequent
playtest found unreadable text and chart/text overlap. The overlay now has an
opaque background, wrapped read-only labels, and separate measured header,
chart and detail areas in the same GUI coordinate space. Chart width follows
the panel, with fixed sample count. Overlay widgets are destroyed before
WindowManager teardown.

The native OMWFX scene target now adds an encoded view-normal attachment only
when a compiled active pass samples normals. Sky normals are neutral. When a
depth-consuming effect is active in first person, the renderer saves world
depth before clearing depth for the weapon, resumes with color/normals retained,
and combines the two depth images for subsequent effects. First-person samples
are remapped into the world projection so reconstruction uses a consistent
depth convention; world-depth consumers retain the separate saved image. In
third person the existing scene depth is used directly without the extra copy
or composition pass.

Distortion-bin geometry is captured separately from scene color. A native field
pass uses scene depth for occlusion, preserves authored blending and strength,
and supplies the existing internal distortion effect. Premultiplied-alpha and
weighted-replace blend selection are independent again. Unlit, sky, water and
distortion draws no longer upload unused point-light sets. The same-build
backend ABI is now version 10 for normal/world-depth access, color-only effect
targets, optional device memory-budget information and retained-topology vertex-prefix updates.
These paths have static frontend checks only; shader compilation,
visual output and frame-pacing improvements await the user's normal build and
playtest. Remaining material normal-map detail and advanced effect features
still require implementation.

The user subsequently reported approximately 30–34 FPS, functioning depth of
field, remaining camera-rotation hitches, incomplete sunshaft output, and an exit
freeze. Shutdown now releases the frame driver before world/GUI/resource
destruction, so its cull snapshots cannot outlive those services. Effect workers
use stop tokens: shutdown cancels queued jobs and kills/reaps an active compiler
instead of joining a serial queue of compiler timeouts. Waiting compiler jobs
sleep on a cancellable condition variable rather than polling the compile lock.
This fixes concrete teardown lifetime/wait hazards; normal exit still needs
playtest confirmation.

Fullscreen effect intermediates now use color-only framebuffers, avoiding
unused full-resolution depth allocations and per-pass depth clears. Scene,
inventory and map targets retain depth. Scratch images initialize to the same
neutral gray/opaque value as PingPongCanvas, preserving authored assumptions in
partial/discard passes such as the installed godray chain. Current view, inverse
view, eye position/direction and resolution are supplied explicitly to native
effect uniforms in both scalar and UBO layouts. Previous-view data advances
after successful submission. Sunshaft visibility remains unverified; these
changes do not imply complete optical/effect compatibility.

Mesh import uses a dense source-index remap instead of one hash node per packed
vertex, with independent remaps for per-primitive-set channels. MultiDrawArrays
ranges now restart topology correctly and validate each range. Shader-driven
primitive instancing remains unsupported. Live material state is decoded once
per StateGraph per prepared frame, while geometry-specific transforms remain
per draw. Residency ceilings now scale further on larger adapters without
increasing the cache fractions or the allowance on small adapters. These are
steps toward long viewing distances; visibility/LOD, asynchronous asset
preparation, draw submission scaling, far-depth precision and non-rendering
update/loading stalls remain further work. The last ordinary log contained
both a 226 ms prepare interval with hundreds of uploads and a separate 705 ms
interval outside the renderer, so these must not be treated as one proven GPU
bottleneck. New fixtures and affected integration units have static frontend
checks only; agents have not built, compiled shaders, run tests, or launched the
game.

The subsequent playtest did not confirm a performance or exit improvement.
Its screenshots show 86–176 mesh updates and approximately 22 ms of CPU
preparation for roughly 2000 draw items. The next changes target the remaining
work directly: native scene/effect state now borrows the live cull-state chain
instead of comparing/copying inherited maps. Override/protected rules resolve
from the root down, and uniform lookup strings are interned once for OSG's
non-heterogeneous map. The borrowed chain is consumed synchronously before
the next cull; workers and GPU submissions do not retain it. Owned snapshots
remain available for immutable/import operations and fixtures.

Effect work records, uniform byte arrays, descriptor-write arrays and pass
metadata retain their allocations across frames. Pass publication reserves
before mutation and then inserts only non-throwing metadata. Backend flattening
borrows pointers to Draw objects instead of copying complete draw records and
their shared light owners. Animated float pose streams update positions/normals
directly in persistent packed storage; unchanged UVs, colors, masks and topology
are retained. Non-float or changed-channel data uses the checked general path.
The redundant second RenderStage sort is removed; the source
[SceneView cull implementation](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6.5/src/osgUtil/SceneView.cpp)
already sorts the graph. This removes identified CPU
allocations/copies; GPU uploads of changed animation data remain necessary and
the whole engine is not claimed to be allocation-free or zero-copy.

The blinking sun had a concrete native-path defect: color-write masks were
ignored, allowing colorless OpenGL occlusion-query geometry in a separate bin
to render after the world as textured quads. Such invisible proxies are now
omitted based on effective color state, independently of the sky-bin flag.
The water material was still an approximation with a fixed blue color and
large fixed alpha. It now derives wave UVs from world position, blends three
animated normal-map scales, computes the view-space basis only in the water
fragment branch, samples the reflected camera image, and uses Fresnel alpha to
mix reflection with the undistorted scene behind water. It also handles the
huge water grid's far-depth limit, retains HDR reflection formats, and prevents
an inherited terrain blend sampler from replacing the reflection input.
Depth-dependent absorption, displaced scene refraction, ripple simulation and
full water-material parity remain outstanding; this does not imply those work.

Shutdown now accepts Exit immediately after input processing and skips the
remaining world/cull/upload work. It stops the renderer and cancels compilers
as soon as the main loop ends. Once GPU submissions finish, descriptor pools
and suballocation blocks use bulk teardown, avoiding individual descriptor
frees and repeated free-range coalescing. Normal shutdown phase logs distinguish
Lua joining/saving, renderer release, world/GUI services, and viewer/resource
cleanup. The latest playtest confirms the exit freeze and sun blinking are
fixed. Its log shows native resource release taking about 95 ms, followed by
about 1.88 seconds releasing game/GUI services and another 1.25 seconds in
world/background cleanup. Exit now hides the stopped game window immediately;
saving and ordered service destruction still finish normally. This improves
visible close response without claiming process teardown is instantaneous.

The latest screenshots still show 28.6–32.9 FPS, 14–20 ms CPU preparation,
roughly 5–6 ms culling, and up to 130 ms submission-interval spikes. The target
is at least 65 FPS at maximum configured draw distance, a total frame budget of
about 15.4 ms. Similar mid/max-distance rates suggest substantial fixed work;
they do not establish unlimited GPU or visibility capacity. The following
additional changes have static checks only and require the normal user build
and playtest before their FPS impact can be assessed:

- Borrowed state views memoize native uniform, attribute, texture and mode
  bindings at shared ancestors. Rebinding each frame invalidates addresses;
  uniform values remain live. Both missing bindings and OVERRIDE/PROTECTED
  results are cached, with no inherited-state clones, ownership registration,
  per-query allocation or mod-dependent cache growth. A repeated state-map
  lookup per draw is also removed.
- CPU state, light and texture-binding records survive brief changes in camera
  direction within bounded cache thresholds. Idle upload pages have a 32 MiB
  reuse allowance and expire after eight idle maintenance sweeps, instead of
  being destroyed at the first sweep and recreated during the next turn.
  In-flight page ownership still prevents premature reuse or eviction.
- Decoded point-light sets reuse their buffers between submissions, and unlit,
  water and sky draws avoid decoding them. Native light-offset deduplication
  uses retained flat storage instead of allocating hash nodes each frame.
- A valid OMWFX engine UBO is copied as a block with native camera corrections;
  its members no longer trigger redundant ancestor searches for scalar values.
  Scalar-only effects keep the existing member packing path.
- Vulkan specialization constants separate world surfaces, water, sky,
  distortion and window overlays within the existing shader pair. This lets
  the driver remove unrelated branches and varyings without reducing effect
  quality or draw distance. Common variants are warmed when a compatible
  target appears on devices with extended dynamic raster state, and reuse the
  persistent pipeline cache. First target creation can incur compilation work;
  the Vulkan 1.1 fallback retains lazy pipeline creation. See the
  [Khronos specialization example](https://docs.vulkan.org/samples/latest/samples/performance/specialization_constants/README.html).

State-cache invalidation, borrowed values and bounded upload-page reuse have
regression fixture sources. No builds, shader compilation, test execution or
game launch has been performed by the agent. Post-processing GPU cost, remaining
rotation spikes and attainment of the 65 FPS target are not yet verified.

The following playtest reported preparation varying from about 10 ms to
70–80 ms or more. Preparation still contained cold asset work: per-mesh Vulkan
buffer creation, CPU texture copies, the first animated mesh's static-to-stream
conversion, and synchronous render-target/pipeline creation. Those are concrete
burst-cost paths; phase-level timings alone do not prove which dominates each
reported spike. The latest ordinary log also records a loading frame with 1,970
mesh uploads, 1,285 image uploads and 38 composite maps, demonstrating that
preparation can receive large batches of work at once.

Mesh vertex/index data now suballocates reusable Vulkan buffer objects, rather
than creating two buffer objects for each mesh. A move-only range owner stays
alive with the mesh through its final in-flight frame. Upload copies, ordinary
vertex/index bindings and fullscreen effect bindings all use the range's
buffer-relative offset. Pools keep bounded idle capacity; shutdown skips
unnecessary range coalescing after draining the GPU. Range allocation prefers
the unused tail for streaming and caches failed-fit size bounds, avoiding
repeated scans of full blocks and movement of live metadata during append.
This uses Vulkan's existing buffer-offset model, not a new GPU requirement;
see [Khronos resource creation](https://docs.vulkan.org/spec/latest/chapters/resources.html)
and [descriptor/buffer management](https://docs.vulkan.org/samples/latest/samples/performance/descriptor_management/README.html).

Tightly packed live RGBA8 and supported BC mip data now borrow the source OSG
image bytes directly through the synchronous upload call. Only the required
copy into GPU staging remains. Row order, authored mip levels, sRGB encoding
and source bounds checks are preserved. Padded rows, channel conversions and
unsupported device compression retain the checked conversion fallback.
Device format capabilities are cached at initialization instead of repeatedly
querying the driver for each texture decision and upload.

Rig/morph arrays marked with dynamic/streaming VBO usage enter the retained
native mesh stream on their first import. They no longer first populate the
canonical static cache and then reimport/fork on animation. Stream imports
also skip discarded CPU asset normal/bounds sidecars, reserve index capacity
before emission, and compute the normalization reciprocal once per vertex.
Every visible draw is still prepared; no visibility or quality reduction and
no arbitrary asset omission is used to hide preparation work.

F3 now retains the worst preparation attempt between text refreshes, including
attempts that could not yet present. Its scene/effects/overlay totals and nested
mesh/texture/target costs describe the same peak attempt, with build/update
counts. Normal slow-frame logging includes those subdivisions for preparation
above 32 ms, rate-limited to once per five seconds. These are existing renderer
diagnostics, not a separate benchmark harness. Buffer ownership, range reuse,
failed-fit invalidation and borrowed texture layouts have regression fixture
sources. C++ frontend checks are allowed; builds, test execution and runtime
verification remain user-operated. Remaining cold image/target creation and
shader compilation can still cost time; eliminating all spikes is unverified.

The residency policy also had a mismatch: its documented cache allowance was
compared against total asset bytes, including currently visible resources. A
large visible scene could keep both caches permanently over their thresholds,
evicting otherwise reusable assets behind the camera after 32 unseen frames.
Charges now mark visible bytes once per preparation attempt, including shared
meshes, multiple cameras and GUI resources. The warm allowance applies above
the current/recent visible working set, with a 256-attempt high-watermark grace
so briefly looking up or at a wall does not collapse it. Combined logical
geometry and texture allowances remain capped at device-scaled ceilings (up to
one quarter and one half of the initial local-memory budget respectively),
leaving room for render targets, staging and driver allocations. Visible assets
are not evicted to enforce those soft cache ceilings. Existing warm fractions
are retained; this corrects their accounting instead of simply raising every
fixed cache limit. Source expiry, multiple touches, active-size changes,
working-set decay and ceiling precedence have regression fixture coverage.

The next playtest still reports 10–20 ms average preparation and occasional
90 ms or longer spikes. Ordinary logs separate two cases: a preparation frame
at 93.85 ms with only 0.53 ms of mesh work and no texture uploads, and another
at 90.35 ms with 25.37 ms of mesh work and 50.09 ms of texture work. Effects
preparation is usually well below 2 ms in those frames; that is CPU time, not
proof of cheap GPU effects. Cold asset bursts and resident-scene overhead
therefore need separate treatment.

Native uniform lookups now use typed slots. Each state graph propagates a
fixed borrowed pointer table once per frame and applies its local bindings in
one visit, retaining OVERRIDE/PROTECTED behavior and live uniform values.
This removes per-draw string searches and repeated walks through parent maps;
it does not clone StateSets or register inherited OSG parents. Arbitrary effect
uniform names keep the general resolver. The uniform cache stores exactly the
43 native slots, reducing each retained state record's footprint.

Hot state, geometry, texture-binding, GUI and light lookups use hash tables.
Their incremental cleanup retains keys rather than iterators and handles
rehashing or external removal safely. Model/view/projection products and normal
matrices are shared within a frame using a flat, epoch-based cache. Its storage
is reused on reset and grows only with the largest simultaneously visible set,
independent of source pointer churn between frames. Cache references are used
only until the next leaf can insert/grow the table. Camera-specific projection
and source matrix identities remain part of the key, and every new frame
recomputes the mutable matrix values.

Particle conversion caches transformed centers, preserves source-index ordering
for equal depths, sorts without stable-sort scratch allocations, avoids zero-
angle quaternion work, packs colors once per particle and exits immediately
when OSG reports an entirely dead pool. Retained GPU capacity and index topology
remain unchanged, but subsequent uploads copy only the live vertex prefix.
Pending updates reuse unsubmitted staging bytes in place, preserving a longer
pending tail when a later update changes only its prefix. A larger replacement
updates both staging ownership and byte count. ABI 10 prevents an older module, which requires full-size
updates, from accepting that new host contract. Float vertex channels also use
one checked bulk read, and byte colors avoid conversion to floats and back.

F3 and normal slow-frame logs include particle conversion/lock-wait time and
native binding/unique-transform counts for the same peak preparation attempt.
Regression fixture sources cover native slot/name equivalence, mutable values,
override rules, frame-cache growth/reset/collisions, hash cleanup cursor
invalidation, byte color/alpha preservation and stable particle ordering.
C++ frontend and source checks are permitted; fixture execution, builds and
runtime performance verification remain user-operated. These changes do not
prove the remaining spikes are eliminated or the 65 FPS target is met.

The next GPU-focused pass addresses effect attachment traffic and scene shading.
The latest user-operated log included 38 effect composites across the enabled
post-processing techniques. That establishes a large
pass count, not individual GPU durations. Color-only effect outputs previously
always used LOAD, including passes replacing the entire viewport. The source
pass now records whether its vertex stage is the generated fullscreen stage;
compiled fragment SPIR-V is checked for kill, termination, demotion and sample
mask operations. Only proven fullscreen replacements use a compatible
DONT_CARE-load render pass. Custom vertex stages and discarding effects retain
LOAD, preserving partial-region padding, persistent named targets and scratch
history. Builtin depth merging and exposure passes use the same overwrite path.
Attachment stores and read/write synchronization remain intact. ABI 11 rejects
modules built before the new pass contract. This follows Khronos guidance on
[avoiding unnecessary attachment loads](https://docs.vulkan.org/samples/latest/samples/performance/render_passes/README.html).

Surfaces without alpha testing or a clip plane select a dedicated shader
specialization with every discard path eliminated. This allows early depth
rejection of hidden opaque fragments without changing source draw order,
terrain blending, depth comparisons or reflection clipping. Pipeline keys also
specialize normal output to the actual target attachments: color-only targets
and the window no longer compute fragment normals for an unused output. Water
still computes its Fresnel normal, and effects requesting scene normals retain
the existing precision and MRT behavior. Vertex lighting skips specular work
when the material or light has no specular contribution; finite-radius lights
are rejected before square-root and shading-data work. Classic attenuation
retains its unlimited range. Draw distance, effect resolution, sample counts,
formats and user settings are unchanged.

Regression fixture sources cover discard/demotion/termination/sample masking,
malformed coverage metadata, and generated versus custom vertex stages. Static
C++ frontend checks and source review are permitted; shader compilation, builds,
fixture execution and GPU performance verification remain user-operated. These
changes do not establish that post-processing overhead is halved or that the
maximum-distance 65 FPS target is achieved.

The following GPU pass removes the final scratch-image store/read and fullscreen
resolve when the last effect uses the generated vertex stage, covers the viewport,
and does not observe framebuffer coordinates, front-facing state or signed Y
derivatives. Such effects render directly into the window. Other effects retain
their offscreen resolve so changing viewport orientation cannot change their
behavior. Final scratch allocation and initialization are skipped on the direct
path. ABI 12 covers the effect-stage metadata and water projection contract.

Ordinary target writes now synchronize with fragment consumers rather than also
stalling vertices. Reflected vertex texture reads use a conservative dependency at
the consumer, including when they read persistent history from a previous frame.
Color-only passes avoid a duplicate global external dependency, and color/depth
dependencies are separated so unrelated color stores do not stall early depth
tests. An outgoing producer-subpass dependency also replaces the separate color
and normals barrier commands after every offscreen pass. This follows the Khronos
[pipeline-barrier guidance](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_barriers/README.html).
The encoded-normal attachment prefers RGB10A2 instead of RGBA16F, halving its
storage/bandwidth with ten-bit normal components; feature checks retain RGBA16F
when sampling, filtering or blending is unsupported. Effect resolutions, sample
counts, ordering and enabled settings are preserved.
The custom screen-space shadow effect and its launcher controls have been
removed. Its shader asset is no longer installed, and old saved-chain entries
are migrated out while preserving the user's other effects.

Native water previously blended only its Fresnel reflection, leaving transmission
unchanged at every depth. It now snapshots depth on the GPU once before the first
water draw in a view and reconstructs optical thickness along the viewing ray.
Continuous Beer-Lambert absorption uses a 2500-unit attenuation distance instead
of leaving the first 2500 units completely unattenuated. Ambient-lit body color
increases with depth; shallow shores remain clear and reflection distortion fades
near the shore. Straight-alpha
composition combines absorption and reflection with the existing scene color,
avoiding a color snapshot or another scene traversal. Water resumes with LOAD for
both depth and color; weapon-depth preservation retains its separate clear-depth
semantics. Views without water allocate no snapshot and window passes without
water retain their discard-depth store policy. Frontend checks and regression
fixture sources cover presentation-sensitive SPIR-V and long-distance depth
reconstruction. Shader builds, fixture execution and GPU performance/visual
verification remain user-operated; these source changes do not establish a
particular FPS or prove that all post-processing overhead has been removed.

## Backend architecture

Use the native Vulkan implementation behind API-independent rendering
contracts. Keep Vulkan types and calls inside its dynamically loaded module;
the game executable must remain able to start OpenGL without a Vulkan loader.
Creation and destruction use paired, versioned, same-build module entry points.
Shared game and MyGUI singletons remain owned by the executable and its shared
services rather than duplicated inside renderer modules.

VSG and the existing vsgopenmw fork remain references for specific algorithms
or reusable code. They are not required dependencies of the native module.
Review licenses and compatibility before reusing external implementations.

```text
Gameplay and Lua
       |
Shared world, animation, camera, asset, and effect services
       |
Persistent resources and batched scene updates
       |
       +-- OSG OpenGL renderer
       |
       +-- Native Vulkan renderer
```

The CPU owns authoritative game transforms, animation state, root motion,
text-key events, and collision inputs. Workers publish immutable asset/pose
results. Rendering consumes stable frame data and retains resources through
GPU completion. Backend handles never enter saves.

OSG math types can remain in game code during migration. OSG graphics objects,
GL names, and Vulkan handles must not cross the final shared rendering API.
Avoid rebuilding or converting the complete OSG scene into Vulkan each frame.

## Work packages

Each package includes source changes and functional acceptance. Small vertical
slices preserve working OpenGL behavior while Vulkan integration progresses.

| Package | Changes and principal locations | Completion condition |
| --- | --- | --- |
| P01 Renderer integration inventory | Audit dependencies in engine, world, animation, GUI, maps, Lua, assets, and tools; track the concrete migration callers | Every operational graphics dependency has an owner and migration path |
| P02 Shared contracts and module loading | `components/render/`, root/component/app CMake; typed handles, resource ownership, device capabilities, versioned module loading | OpenGL-only builds require no Vulkan SDK or loader; expired handles cannot name reused assets |
| P03 OpenGL adaptation | `engine.*`, `mwrender/opengl/`, `renderingmanager.*`, world services | Route frame lifecycle through the interface with existing simulation, Lua, focus-query, rendering, and shutdown order preserved |
| P04 Vulkan initialization and presentation | `mwrender/vulkan/`, SDL window/input services | Select the intended device; create/present/recreate the surface; handle high DPI, minimization, failure, and shutdown |
| P05 Shared assets and collision | `components/nif/`, `nifosg/`, `resource/`, `nifbullet/`, new shared asset descriptions | Preserve geometry, materials, controllers, bounds, image formats, and NIF/non-NIF collision without requiring GL rendering |
| P06 GPU resources and streaming | Vulkan memory, upload, residency, and frame services | Persistent resources, batched uploads, cancellation, bounded queues, and safe release under cell streaming |
| P07 Shaders and material pipelines | `files/shaders/vulkan/`, shared shader libraries and Vulkan pipeline services | Explicit resource layouts and supported material pipelines preserve existing material contributions and projection conventions |
| P08 World objects and cameras | `mwworld/refdata.*`, `worldimp.*`, `scene.*`, `cellpreloader.*`, `mwbase/world.hpp`, mwrender objects/cameras | Stable render handles preserve activation, move/rotate/scale, enable/delete, cell transfer, picking, and camera behavior |
| P09 Actors animation and particles | mwrender animation/actor/NPC/creature/weapon/ESM4 code; shared controllers/skeleton/rig/morph code | Correct pose, blending, root motion, text keys, equipment attachments, particles, and conservative animated bounds |
| P10 Terrain paging and visibility | `components/terrain/`, land/terrain storage, object paging, groundcover, occlusion services | Persistent terrain and instance data preserve LOD, seams, layer blending, density, and conservative per-view visibility |
| P11 Lighting shadows and atmosphere | shared light/shadow helpers, sky/fog/weather code and shaders | Preserve light selection/attenuation, negative lights, configured shadows, sky/weather, fog, and interior rules |
| P12 GUI video and offscreen views | `components/myguiplatform/`, `mwgui/`, `extern/osg-ffmpeg-videoplayer/`, maps/previews/screenshots | Working menus, text, loading screens, inventory, previews, video, maps, fog of war, save images, and screenshots |
| P13 Transparency and water | water/ripples/transparent passes/distortion/opaque-copy code and shaders | Correct blend ordering, water-separated geometry, reflection/refraction, depth/color snapshots, underwater effects, and distortion |
| P14 OMWFX and Lua compatibility | `components/fx/`, postprocessor/canvas/luminance/scratch code, OMWFX sources and Lua bindings | Preserve effect order, data conventions, uniforms, flags, history, HDR/exposure, controls, and error behavior |
| P15 Stereo and debug rendering | `components/stereo/`, debug drawers, reports and statistics | Independent per-eye visibility and effects; functional debug geometry, modes, and existing diagnostic services |
| P16 Format and service closure | Remaining OSG callers in world, classes, mechanics, GUI, Lua, resource adapters and tools | Preserve supported formats and public behavior; retain OpenCS and existing tools with their current backend |
| P17 Launcher settings and GPU routing | video settings/defaults, options/main, launcher graphics page, GPU selection helpers | Persist renderer choice, support a CLI override, select the actual GPU, report failures, and offer an explicit OpenGL retry |
| P18 Low end code optimization | Scene preparation, batching, memory, lighting, visibility, animation and synchronization | Reuse persistent state, avoid redundant work and bandwidth, and keep optional hardware features optional |
| P19 Dependencies and packaging | CMake, Dockerfiles, cached build scripts, package script, AppRun | Ship the module and shaders with host-driver discovery while preserving normal user config, mods, saves, and OpenGL startup |
| P20 Functional verification | Component/game fixtures, shader validation, image scenarios, CI and package checks | Verify material/scene/UI behavior, synchronization, failure cleanup, streaming, save/load, and shutdown |
| P21 Platform rollout and documentation | Platform dependency/install files, CI, settings/postprocessing/Lua docs, enhanced docs | Linux/AppImage first; advertise other Vulkan platforms only after their functional acceptance |

## Renderer implementation rules

- Keep meshes, textures, materials, and pipeline objects persistent. Apply
  changed instance data in batches rather than allocating objects per draw.
- Use generation-tagged handles and frame-owned references. An unloaded object
  or a canceled background request must not mutate a later resource in its slot.
- Suballocate GPU memory with correct alignment. Separate optimal images from
  linear buffers, honor dedicated allocation requirements, and reclaim ranges
  only after their GPU use has completed.
- Reuse command pools, staging space, descriptor pages, and instance buffers.
  Batch compatible adjacent draws and preserve required transparency order.
- Keep ordinary uploads inside frame submissions. Avoid per-resource queue
  waits and steady-state device-wide idle waits or synchronous GPU readbacks.
- Keep acquisition and frame synchronization separate from presentation
  synchronization. Presentation semaphores belong to swapchain images. See
  [Khronos semaphore lifetime guidance](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).
- Begin with compact forward rendering. Allocate normals, scene copies, shadow
  targets, and postprocessing intermediates only when a consumer needs them.
- Preserve conservative visibility and separate main, reflection, shadow,
  stereo, map, and preview view state. Reduce work before deformation and
  submission without suppressing required animation events or attachments.
- Pack and reuse light data. Any clustered-light capacity overflow must have
  defined behavior that preserves illumination and memory safety.
- Compile/cache bounded shader variants and cache material descriptors. Do not
  create pipelines or rewrite identical descriptors on the normal draw path.
- Keep a Vulkan 1.1 baseline. Descriptor indexing, ray tracing, mesh shaders,
  timeline semaphores, synchronization2, dynamic rendering, 16-bit arithmetic,
  and dedicated transfer queues remain optional paths.
- Keep quality and backend selection separate. Preserve materials, view
  distance, effects, resolution, and the user's graphics settings during the
  API migration.

## Compatibility details

Retain VFS precedence, mip levels and compressed textures, color-space and
orientation behavior, UV channels and animation, inherited NIF state,
alpha/culling flags, later-game material support, and supported non-NIF readers.
Existing CPU image decoders may remain import adapters; they must not require
an active OpenGL context for Vulkan resource creation.

Separate animation playback and pose evaluation from drawing. GPU skinning and
morphs must preserve bind poses, normal transforms, equipment replacement,
root motion, attack/cast timing, bone queries, and CPU visual picking semantics.
Do not depend on reading posed geometry back from the GPU for ordinary gameplay.

Preserve Lua camera projections and the documented OMWFX depth, normal, light,
scene-color, per-eye, and temporal inputs. Convert clip depth, viewport
orientation, winding, and image conventions at explicit backend boundaries.
Arbitrary OpenGL shader replacements require a supported translation path or
clear incompatibility diagnostics; filenames do not justify guessed rewrites.

Build an explicit pass planner for effect requirements, attachment lifetimes,
clears, load/store operations, image layouts, barriers, resolves, and history.
Keep effect order and required snapshots. Reuse scratch images only when
lifetimes do not overlap. Retain existing empty-chain and unused-target fixes.

Map and screenshot readbacks are asynchronous. Keep pending resources alive,
and flush map results needed by save serialization without a per-frame wait.
Preserve video decoding and audio scheduling while separating decoded-frame
access from the existing player's OSG texture output.

GPU selection uses stable device identity rather than enumeration position.
Keep Automatic's inherited routing, map the existing PCI choice where Vulkan
properties permit, and use UUIDs where appropriate. A failed explicit Vulkan
selection reports its error and offers a one-run OpenGL retry without silently
rewriting the saved choice.

## Delivery order

The active sequence is P02/P03/P04/P06/P07, followed by P05/P08/P09/P10/P12 to
connect the game world and interface. Complete lighting, water/transparency,
effects, stereo, and format/service compatibility before claiming parity.
P17 exposes Vulkan only when it can run the game. Packaging and functional
fixtures accompany the implementation throughout.

The native module, OpenGL frame adapter, resource contracts, raster pipelines,
resource ownership, static model loading, persistent render scene, module
loading, and packaging source changes are present. Remaining immediate work includes advanced multi-texture materials, groundcover
instancing/wind/stomping, weather-specific drawables, shadows, complete water and
depth-target paths, remaining OMWFX inputs/pass types, stereo/debug/service compatibility, and
removal of the hidden GL capability context. The current color-target, lighting,
sky and particle paths are implemented source changes, not verified parity.
The user confirmed the corrected image in playtesting; the persistent performance
collapse is now traced to cache retirement in the supplied game log. Source
changes remove OSG parent registration from native snapshots, spread retirement
across frames, and remove repeated driver allocations, pipeline rebuilds and CPU
topology conversion; acceptance still requires user playtesting. The visible adapter is transitional; it
must not become a second full-scene conversion or GL framebuffer-copy path.

## Build and runtime handoff

Follow `AGENTS.md`: agents perform static source checks and do not run builds,
Docker builds, packaging, test executables, or runtime checks. The user runs the
existing cached scripts and reports results. Keep `.build/` caches intact.

The development and AppImage scripts enable the experimental module; standalone
CMake builds can use `OPENMW_BUILD_VULKAN_RENDERER=ON`. The module requires
Vulkan development headers/libraries and glslangValidator. OpenGL-only CMake
builds leave the option off and do not require either dependency.

```bash
./enhanced/scripts/build-appimage.sh
APPIMAGE_EXTRACT_AND_RUN=1 .build/dist/OpenMW-Enhanced-x86_64.AppImage --openmw --version
```

The packaged module is `usr/bin/openmw-renderer-vulkan.so` on Linux. Its compiled
basic shaders are installed under `resources/shaders/vulkan/`. Module creation
uses `Render::BackendModule` with an SDL Vulkan window.

For the first visible check, select **Graphics → Renderer → Vulkan
(experimental)** in the launcher, save the settings, and launch the game. The
window title should identify Vulkan. Check the main menu, readable fonts,
loading screen, and a loaded save with camera movement and visible world
geometry. `openmw.log` should contain `Vulkan Renderer: <device name>`.
Appearance will differ while the material and lighting pipelines are incomplete.
Select OpenGL in the launcher to return to the existing renderer. Renderer
changes take effect at the next launch; user mod/configuration/save paths are
unchanged.

For configuration regressions, verify bundled `usr/bin/openmw.cfg` and
`usr/bin/defaults.bin` plus the user's `~/.config/openmw/openmw.cfg` and
`~/.config/openmw/settings.cfg` in startup logs. Keep Bullet threading enabled
by default. A successful version check establishes packaging startup only.
