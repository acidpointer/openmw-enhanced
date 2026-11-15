# A single static allocator lives in each executable. Never link another copy
# into components, plugins or the dynamically loaded Vulkan renderer.
set(_openmw_mimalloc_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
    set(_openmw_mimalloc_default ON)
endif()
option(OPENMW_USE_MIMALLOC "Use the pinned static mimalloc process allocator on Linux" ${_openmw_mimalloc_default})
option(OPENMW_MIMALLOC_DIAGNOSTICS "Enable mimalloc allocation statistics and sampling support" OFF)

if(OPENMW_USE_MIMALLOC)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID)
        message(FATAL_ERROR "The static process allocator integration requires Linux ELF. Set OPENMW_USE_MIMALLOC=OFF on other platforms.")
    endif()
    if(CMAKE_VERSION VERSION_LESS 3.18)
        message(FATAL_ERROR "mimalloc 3.6.1 requires CMake 3.18 or newer")
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang)$")
        message(FATAL_ERROR "Static mimalloc interposition requires GCC or Clang on Linux")
    endif()
    set(_openmw_allocator_flags "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_EXE_LINKER_FLAGS}")
    foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        string(APPEND _openmw_allocator_flags " ${CMAKE_C_FLAGS_${config}} ${CMAKE_CXX_FLAGS_${config}} ${CMAKE_EXE_LINKER_FLAGS_${config}}")
    endforeach()
    if(_openmw_allocator_flags MATCHES "-fsanitize=")
        message(FATAL_ERROR "For sanitizer builds, configure with OPENMW_USE_MIMALLOC=OFF so the sanitizer owns allocation interception")
    endif()
endif()

function(openmw_apply_process_allocator directory)
    if(NOT OPENMW_USE_MIMALLOC)
        return()
    endif()
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type "${target}" TYPE)
        if(type STREQUAL "EXECUTABLE")
            # Whole-archive prevents GC/as-needed from silently discarding the
            # C/C++ overrides. Executable definitions preempt shared libraries.
            if(CMAKE_VERSION VERSION_LESS 3.24)
                get_whole_archive_options(allocator_link mimalloc-static)
            else()
                set(allocator_link "$<LINK_LIBRARY:WHOLE_ARCHIVE,mimalloc-static>")
            endif()
            # The project has both plain and keyword link signatures. Setting
            # the property avoids mixing them, and puts the allocator first.
            get_target_property(libraries "${target}" LINK_LIBRARIES)
            if(NOT libraries)
                set(libraries "")
            endif()
            set_property(TARGET "${target}" PROPERTY LINK_LIBRARIES "${allocator_link};${libraries}")
            target_compile_definitions("${target}" PRIVATE OPENMW_USE_MIMALLOC)
            # Export only the allocator API, not all game internals. This also
            # keeps override entry points alive through executable LTO/GC.
            foreach(symbol IN ITEMS malloc calloc realloc reallocarray free cfree aligned_alloc posix_memalign
                    free_sized free_aligned_sized
                    memalign valloc pvalloc malloc_usable_size malloc_size malloc_good_size
                    strdup strndup realpath __libc_* mi_* _Zn* _Zd*)
                target_link_options("${target}" PRIVATE "LINKER:--export-dynamic-symbol=${symbol}")
            endforeach()
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        # Dependency test/example programs are not OpenMW processes.
        if(NOT child MATCHES "/extern(/|$)")
            openmw_apply_process_allocator("${child}")
        endif()
    endforeach()
endfunction()
