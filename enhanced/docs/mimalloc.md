# OpenMW process allocator

The maintained Linux build uses **mimalloc 3.6.1**, the latest stable upstream
release verified on 2026-11-15. It is a statically built source dependency, not
an Arch runtime package, preload wrapper or separately loaded allocator DSO.

## Dependency and linking

The project already uses CMake FetchContent with archive checksums for bundled
dependencies. [extern/mimalloc.cmake](../../extern/mimalloc.cmake) follows that
convention; no submodule is needed. The dependency is pinned to:

- Release: <https://github.com/microsoft/mimalloc/releases/tag/v3.6.1>
- Commit: `56dc5eb80145f9a3ba7ca23d47c6a248b9d8586c`
- Source: <https://github.com/microsoft/mimalloc/archive/refs/tags/v3.6.1.tar.gz>
- SHA-256: `2672e6a99d4579fb4bb32d9de0d63aa424ce600d31fbfaf06eb74d940d2e07a6`
- License: MIT; installed at `usr/share/licenses/openmw/mimalloc/LICENSE`.

The Arch PKGBUILD additionally lists the archive and its SHA-512 checksum so
conventional makepkg builds verify/extract it before configuration. The local
build helper still uses the working OpenMW tree, including edits; FetchContent
may download the allocator dependency on the first build. It does not fetch a
replacement OpenMW checkout. An offline/custom source can be supplied with
`FETCHCONTENT_SOURCE_DIR_MIMALLOC=/path/to/extracted/mimalloc-3.6.1`.

Every OpenMW executable, including launcher, editor, tools and optional tests,
links `mimalloc-static` first with whole-archive semantics. The allocator's C/C++
entry points and `mi_*` API are explicitly exported in the executable's dynamic
symbol table. This lets shared Qt/OSG libraries, readers and renderer modules
resolve their standard allocation calls to the same allocator instance, retaining
mimalloc's per-thread heaps and cross-thread frees. The allocator itself
is compiled without LTO and with default visibility; this avoids losing its
interposition symbols through whole-program optimization or upstream hidden
visibility defaults. Game code retains its normal LTO configuration.

The Vulkan renderer, components and plugins never embed their own allocator.
Replacing allocation macros in selected source files would not provide this
cross-module ownership guarantee. No `LD_PRELOAD` is introduced, so separate
host processes such as the crash debugger keep their normal allocators.
GPU/device memory remains managed by Vulkan and the existing GPU pools.
Private third-party allocators backed directly by mmap retain their own policy;
Lua's normal tracked realloc/free path uses the process override.

## Workload defaults

| Setting | OpenMW default | Reason |
| --- | --- | --- |
| Initial arena reservation | 256 MiB on 64-bit; 128 MiB on 32-bit | Smaller virtual-address chunks for launcher/tools and modest machines; this is neither a RAM preallocation nor a total memory cap. Upstream can grow subsequent arenas. |
| Purge delay | 2000 ms | Reuse short-lived frame and cell-loading allocations before purging their pages. |
| Arena purge multiplier | 1 | Avoid multiplying that delay across asset arenas. |
| Purge decommits | 0 | Linux MADV_FREE lets the kernel reclaim freed pages under pressure while avoiding immediate discard/refault cycles during reuse. Upstream falls back to MADV_DONTNEED where needed. |
| Statistics and sampled profiling | Off | Avoid per-allocation accounting/sampling overhead during play. |
| Forced collection on process exit | Skipped | Let the OS reclaim process storage; avoid another full heap collection after game teardown. Live allocations are not destroyed before late library destructors run. |
| Architecture-specific instructions | Off | Keep the allocator compatible with older x86-64 CPUs used with low-end GPUs. |

The three runtime defaults without upstream build-time switches are exposed by
a checked, idempotent adaptation of the pinned `src/util/options.c` table. The
adaptation fails configuration if the expected entries change. Allocation,
coalescing and concurrency algorithms are unchanged. Defaults therefore apply
from the first allocation, before shared-library constructors or worker startup;
they are not set too late by a per-frame or main-function callback.

Normal `MIMALLOC_*` environment overrides still take precedence, including
legacy option names recognized by upstream. Purging is opportunistic: 2000 ms
is an eligibility delay, not a background timer or a guaranteed RSS deadline.
Lazy-free pages can remain in reported RSS while already reclaimable by the
kernel; a high RSS alone does not establish a leak. To prefer prompt RSS drops:

```sh
MIMALLOC_PURGE_DECOMMITS=1 MIMALLOC_PURGE_DELAY=500 openmw
```

These defaults follow the observed engine workload; they are not a measured
optimum or a promise of a specific FPS gain. Cache copies, asset decoding,
scene preparation and GPU pass costs remain independent optimization targets.

## Build and verification

Linux defaults to `OPENMW_USE_MIMALLOC=ON`; the Arch/Docker helpers also request
it explicitly. `OPENMW_USE_MIMALLOC=OFF` selects the system allocator for
comparison or sanitizer builds. Other platforms keep the system allocator;
forcing this ELF integration on an unsupported platform fails configuration.
Build-time sanitizer interception must not compete with the global override.

`OPENMW_MIMALLOC_DIAGNOSTICS=ON` enables upstream statistics/profiling support
when needed. The default build still supports `MIMALLOC_VERBOSE=1` for option
reporting. Startup logs record the linked version and effective purge/arena
options, including user overrides.

Native package/AppImage staging checks inspect ELF symbols, require defined
C/C++ overrides in installed executables, reject a shared mimalloc dependency,
and reject modules containing their own `mi_*` definitions. Functional fixture
sources cover calloc, aligned allocation, new/delete, realloc preservation and
allocation/free across a separate module and a terminating worker thread.

Agent verification is limited to static source/frontend/metadata checks.
Builds, fixture execution and runtime checks remain user-operated under
`AGENTS.md`. Rebuild using the normal scripts, then check the AppImage:

```sh
./enhanced/scripts/build-appimage.sh
APPIMAGE_EXTRACT_AND_RUN=1 .build/dist/OpenMW-Enhanced-x86_64.AppImage --openmw --version
```

References: [upstream static override guidance](https://microsoft.github.io/mimalloc/overrides.html),
[runtime options](https://microsoft.github.io/mimalloc/group__options.html), and
the pinned release's `src/util/options.c`, `src/arena/arena.c` and
`src/prim/unix/prim.c` for defaults, arena growth and Linux reclamation behavior.
