string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" MOC_PROCESSOR)
if(MOC_PROCESSOR MATCHES "^(x86_64|amd64|x64|x86|i[3-6]86)$")
    set(MOC_SOURCES
        MaskedOcclusionCulling.cpp
        MaskedOcclusionCullingAVX2.cpp
        MaskedOcclusionCullingAVX512.cpp
    )
else()
    # ARM/other: base file only (SSE2/SSE4.1 via sse2neon on ARM)
    set(MOC_SOURCES
        MaskedOcclusionCulling.cpp
    )
endif()

add_library(maskedoc STATIC ${MOC_SOURCES})
target_include_directories(maskedoc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Keep separately targeted ISA implementations isolated during link optimization.
# Release-specific IPO takes precedence over the generic target property, and
# makepkg can independently inject -flto through CXXFLAGS.
set_target_properties(maskedoc PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION FALSE
    INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE
    INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO FALSE
    INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL FALSE
    INTERPROCEDURAL_OPTIMIZATION_DEBUG FALSE
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    # The SIMD lane accessors reinterpret vector storage through scalar arrays.
    # Preserve that aliasing contract without weakening optimizations elsewhere.
    target_compile_options(maskedoc PRIVATE -fno-lto -fno-strict-aliasing)
endif()

if(MOC_PROCESSOR MATCHES "^(x86_64|amd64|x64|x86|i[3-6]86)$")
    if(MSVC)
        # Main file must be compiled at SSE2 baseline (no AVX)
        set_source_files_properties(MaskedOcclusionCulling.cpp PROPERTIES COMPILE_FLAGS "/arch:SSE2")
        set_source_files_properties(MaskedOcclusionCullingAVX2.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX2")
        set_source_files_properties(MaskedOcclusionCullingAVX512.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX512")
    else()
        set_source_files_properties(MaskedOcclusionCulling.cpp PROPERTIES COMPILE_FLAGS "-msse2 -mxsave")
        set_source_files_properties(MaskedOcclusionCullingAVX2.cpp PROPERTIES COMPILE_FLAGS "-mavx2 -mfma")
        set_source_files_properties(MaskedOcclusionCullingAVX512.cpp PROPERTIES COMPILE_FLAGS "-mavx512f -mavx512bw -mavx512dq -mfma")
    endif()
endif()
