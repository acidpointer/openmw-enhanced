if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "OPENMW_LUAJIT_GOOD_GC requires a native Linux toolchain")
endif()

find_program(OPENMW_LUAJIT_MAKE NAMES gmake make)
find_program(OPENMW_LUAJIT_PATCH NAMES patch)
if(NOT OPENMW_LUAJIT_MAKE OR NOT OPENMW_LUAJIT_PATCH)
    message(FATAL_ERROR "OPENMW_LUAJIT_GOOD_GC requires GNU Make and patch")
endif()

include(FetchContent)
set(luajit_revision 63c8f1a9b479ba5518cf6a170ff7729178bc3cfd)
FetchContent_Declare(luajit
    URL "https://codeload.github.com/LuaJIT/LuaJIT/tar.gz/${luajit_revision}"
    URL_HASH SHA256=8cad76f444b76001b7123dc626c5d0bfc24f1f790333737443345f7e6b41bb5f
)
FetchContent_MakeAvailable(luajit)

# Apply the patch to a private copy so the downloaded or makepkg source stays
# pristine. LuaJIT's own Makefile generates the public luajit.h and VM code.
set(luajit_build_dir "${CMAKE_CURRENT_BINARY_DIR}/luajit-good-gc/${luajit_revision}")
set(luajit_patch "${CMAKE_CURRENT_LIST_DIR}/../enhanced/patches/luajit/good-gc.patch")
set(LUA_INCLUDE_DIR "${luajit_build_dir}/src")
set(luajit_archive "${LUA_INCLUDE_DIR}/libluajit.a")
set(luajit_header "${LUA_INCLUDE_DIR}/luajit.h")
file(MAKE_DIRECTORY "${LUA_INCLUDE_DIR}")

add_custom_command(
    OUTPUT "${luajit_archive}" "${luajit_header}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${luajit_SOURCE_DIR}" "${luajit_build_dir}"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${luajit_build_dir}"
        "${OPENMW_LUAJIT_PATCH}" --batch --forward --fuzz=0 -p1 --input "${luajit_patch}"
    COMMAND "${OPENMW_LUAJIT_MAKE}" -C "${LUA_INCLUDE_DIR}" BUILDMODE=static
        "CC=\"${CMAKE_C_COMPILER}\"" "PREFIX=${CMAKE_INSTALL_PREFIX}" TARGET_CFLAGS=-fPIC libluajit.a
    DEPENDS "${CMAKE_CURRENT_LIST_FILE}" "${luajit_patch}" "${luajit_SOURCE_DIR}/src/lj_gc.c"
    COMMENT "Building LuaJIT with OwnlyMe's Good GC patch"
    VERBATIM
)
add_custom_target(openmw-build-luajit DEPENDS "${luajit_archive}" "${luajit_header}")

add_library(openmw-luajit STATIC IMPORTED)
set_target_properties(openmw-luajit PROPERTIES IMPORTED_LOCATION "${luajit_archive}")
add_dependencies(openmw-luajit openmw-build-luajit)
target_include_directories(openmw-luajit SYSTEM INTERFACE "${LUA_INCLUDE_DIR}")
target_compile_definitions(openmw-luajit INTERFACE OPENMW_LUAJIT_GOOD_GC)
target_link_libraries(openmw-luajit INTERFACE m ${CMAKE_DL_LIBS})
set(LUA_LIBRARIES openmw-luajit)

install(FILES "${luajit_SOURCE_DIR}/COPYRIGHT"
    "${CMAKE_CURRENT_LIST_DIR}/../enhanced/patches/catalog/luajit-good-gc.toml"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/openmw/luajit")
