# Pinned dependency, following the other FetchContent declarations in extern/.
# Upstream v3.6.1: 56dc5eb80145f9a3ba7ca23d47c6a248b9d8586c.
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE ON CACHE BOOL "" FORCE)
set(MI_OPT_ARCH OFF CACHE STRING "" FORCE)
set(MI_OPT_SIMD OFF CACHE BOOL "" FORCE)
set(MI_USE_CXX OFF CACHE STRING "" FORCE)
if(OPENMW_MIMALLOC_DIAGNOSTICS)
    set(MI_STATS ON CACHE STRING "" FORCE)
    set(MI_PROFILE ON CACHE STRING "" FORCE)
else()
    set(MI_STATS OFF CACHE STRING "" FORCE)
    set(MI_PROFILE OFF CACHE STRING "" FORCE)
endif()
set(MI_SKIP_COLLECT_ON_EXIT ON CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(mimalloc
    URL https://github.com/microsoft/mimalloc/archive/refs/tags/v3.6.1.tar.gz
    URL_HASH SHA256=2672e6a99d4579fb4bb32d9de0d63aa424ce600d31fbfaf06eb74d940d2e07a6
    SOURCE_DIR fetched/mimalloc
)
FetchContent_MakeAvailable(mimalloc)

# Small, checked default-table adaptation; no allocator algorithm is forked.
# Apply after population as well as with FETCHCONTENT_SOURCE_DIR_MIMALLOC (the
# native package uses makepkg's verified/extracted dependency archive).
function(openmw_mimalloc_workload_defaults directory)
    set(options_file "${directory}/src/util/options.c")
    file(READ "${options_file}" options)
    foreach(pair IN ITEMS
            "1000, MI_OPTION_LEGACY(purge_delay,reset_delay)|OPENMW_MI_PURGE_DELAY, MI_OPTION_LEGACY(purge_delay,reset_delay)"
            "1, MI_OPTION_LEGACY(purge_decommits,reset_decommits)|OPENMW_MI_PURGE_DECOMMITS, MI_OPTION_LEGACY(purge_decommits,reset_decommits)"
            "4, MI_OPTION(arena_purge_mult)|OPENMW_MI_ARENA_PURGE_MULT, MI_OPTION(arena_purge_mult)")
        string(REPLACE "|" ";" parts "${pair}")
        list(GET parts 0 original)
        list(GET parts 1 replacement)
        string(FIND "${options}" "${original}" found)
        if(found EQUAL -1)
            string(FIND "${options}" "${replacement}" patched)
            if(patched EQUAL -1)
                message(FATAL_ERROR "mimalloc option table changed; review the OpenMW defaults: ${original}")
            endif()
        else()
            string(REPLACE "${original}" "${replacement}" options "${options}")
        endif()
    endforeach()
    file(READ "${options_file}" original_options)
    if(NOT options STREQUAL original_options)
        file(WRITE "${options_file}" "${options}")
    endif()
endfunction()
openmw_mimalloc_workload_defaults("${mimalloc_SOURCE_DIR}")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(arena_reserve_kib 262144)
else()
    set(arena_reserve_kib 131072)
endif()
target_compile_definitions(mimalloc-static PRIVATE
    MI_DEFAULT_ARENA_RESERVE=${arena_reserve_kib}
    OPENMW_MI_PURGE_DELAY=2000
    OPENMW_MI_PURGE_DECOMMITS=0
    OPENMW_MI_ARENA_PURGE_MULT=1
)
# The unity translation unit still optimizes across the allocator itself.
# Keep its public interposition symbols out of executable LTO internalization,
# including when makepkg supplies -flto in the environment's CFLAGS.
target_compile_options(mimalloc-static PRIVATE -fno-lto -fvisibility=default)
set_target_properties(mimalloc-static PROPERTIES
    C_VISIBILITY_PRESET default
    INTERPROCEDURAL_OPTIMIZATION OFF
    INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF
)
# Skip the allocator's standalone installation, headers and CMake exports.
set_property(DIRECTORY "${mimalloc_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
set(OPENMW_MIMALLOC_SOURCE_DIR "${mimalloc_SOURCE_DIR}" PARENT_SCOPE)
