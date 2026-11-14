find_package(Vulkan 1.1 REQUIRED)
find_program(OPENMW_GLSLANG_VALIDATOR NAMES glslangValidator)
if(NOT OPENMW_GLSLANG_VALIDATOR)
    message(FATAL_ERROR "The Vulkan renderer requires glslangValidator to compile its shaders")
endif()

# Load this module explicitly; linking it to openmw-lib would make the Vulkan
# loader mandatory even when the user chooses OpenGL.
add_library(openmw-renderer-vulkan MODULE
    backend.cpp backend.hpp
    check.hpp
    memory.cpp memory.hpp
    module.cpp
)
target_include_directories(openmw-renderer-vulkan PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(openmw-renderer-vulkan PRIVATE openmw-render-core Vulkan::Vulkan SDL2::SDL2)
set_target_properties(openmw-renderer-vulkan PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

set(vulkan_shader_outputs)
if(APPLE)
    set(vulkan_resource_root "${APP_BUNDLE_DIR}/Contents/Resources")
else()
    set(vulkan_resource_root "${OPENMW_RESOURCES_ROOT}")
endif()
foreach(shader basic.vert basic.frag)
    set(source "${CMAKE_SOURCE_DIR}/files/shaders/vulkan/${shader}")
    set(output "${vulkan_resource_root}/resources/shaders/vulkan/${shader}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${vulkan_resource_root}/resources/shaders/vulkan"
        COMMAND "${OPENMW_GLSLANG_VALIDATOR}" --target-env vulkan1.1 -V "${source}" -o "${output}"
        DEPENDS "${source}"
        VERBATIM
    )
    list(APPEND vulkan_shader_outputs "${output}")
endforeach()
add_custom_target(openmw-vulkan-shaders DEPENDS ${vulkan_shader_outputs})
add_dependencies(openmw-renderer-vulkan openmw-vulkan-shaders)

# SDL's module loader uses an exact executable-relative path. Put the module
# beside the executable for both local installs and installed AppImages.
install(TARGETS openmw-renderer-vulkan
    LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
