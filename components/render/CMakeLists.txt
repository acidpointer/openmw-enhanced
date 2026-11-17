# Backend-independent rendering data and scene preparation. Keep this target
# free of OSG, SDL, Vulkan and shared game singletons.
add_library(openmw-render-core STATIC
    scene.cpp scene.hpp
    texture.cpp texture.hpp
    backend.hpp environment.hpp presentation.hpp resourcepool.hpp allocationranges.hpp math.hpp raster.hpp reserve.hpp uploadpages.hpp cachesweep.hpp residency.hpp pipelinecache.hpp
    ../renderassets/model.hpp
)
target_include_directories(openmw-render-core PUBLIC "${CMAKE_SOURCE_DIR}")
set_target_properties(openmw-render-core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
