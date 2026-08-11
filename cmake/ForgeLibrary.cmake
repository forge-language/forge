# Reusable helpers for compiling Forge (.fg) sources with CMake.
#
# Used two ways:
#  - In-tree: included directly by this repo's root CMakeLists.txt. Defaults
#    below match the historical hardcoded behavior (forge_root = repo root,
#    lib_dir = this build's lib/, forge binary = the in-tree `forge` target).
#  - Out-of-tree: included by ForgeConfig.cmake after `find_package(Forge)`,
#    which sets FORGE_EXECUTABLE/FORGE_ROOT_DIR/FORGE_LIB_DIR/etc to point at
#    an installed toolchain before including this file.
if(NOT DEFINED FORGE_EXECUTABLE)
    set(FORGE_EXECUTABLE forge)
endif()
if(NOT DEFINED FORGE_ROOT_DIR)
    set(FORGE_ROOT_DIR "${CMAKE_SOURCE_DIR}")
endif()
if(NOT DEFINED FORGE_LIB_DIR)
    set(FORGE_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
endif()
if(NOT DEFINED FORGE_RUNTIME_TARGET)
    set(FORGE_RUNTIME_TARGET forge_runtime)
endif()
if(NOT DEFINED FORGE_BUILD_DEPS)
    set(FORGE_BUILD_DEPS forge forge_runtime)
endif()
if(NOT DEFINED FORGE_CC_ARGS)
    set(FORGE_CC_ARGS "")
endif()

function(forge_add_library NAME SOURCE)
    # Optional: EXTRA_ARGS <forge CLI flags...> (e.g. -I dir -l other_lib)
    #           EXTRA_DEPENDS <extra build-order dependencies...>
    cmake_parse_arguments(FAL "" "" "EXTRA_ARGS;EXTRA_DEPENDS" ${ARGN})

    set(gen_a "${CMAKE_BINARY_DIR}/lib/libforge_${NAME}.a")
    set(gen_h "${CMAKE_BINARY_DIR}/generated/libs/${NAME}.h")

    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/libs")
    # In-tree, lib/ already exists by the time this runs (created by the
    # forge_std/forge_runtime ARCHIVE targets this custom command depends
    # on). Out-of-tree, nothing else creates it, so `ar` fails with
    # "No such file or directory" unless we make it ourselves.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    add_custom_command(
        OUTPUT "${gen_a}" "${gen_h}"
        COMMAND ${FORGE_EXECUTABLE} --lib "${SOURCE}" -o "${gen_a}" --header "${gen_h}"
            ${FORGE_CC_ARGS}
            --forge-root "${FORGE_ROOT_DIR}" --lib-dir "${FORGE_LIB_DIR}"
            ${FAL_EXTRA_ARGS}
        DEPENDS ${FORGE_BUILD_DEPS} "${SOURCE}" ${FAL_EXTRA_DEPENDS}
        COMMENT "Compiling Forge library ${NAME}"
        VERBATIM
    )

    add_custom_target(forge_lib_${NAME} DEPENDS "${gen_a}" "${gen_h}")

    add_library(${NAME} STATIC IMPORTED GLOBAL)
    add_dependencies(${NAME} forge_lib_${NAME})
    set_target_properties(${NAME} PROPERTIES
        IMPORTED_LOCATION "${gen_a}"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/generated/libs"
        INTERFACE_LINK_LIBRARIES "${FORGE_RUNTIME_TARGET}"
    )
endfunction()

function(forge_add_executable NAME SOURCE)
    # Optional: EXTRA_ARGS <forge CLI flags...> (e.g. -I dir -l other_lib)
    #           EXTRA_DEPENDS <extra build-order dependencies...>
    cmake_parse_arguments(FAE "" "" "EXTRA_ARGS;EXTRA_DEPENDS" ${ARGN})

    set(gen_bin "${CMAKE_BINARY_DIR}/bin/${NAME}")

    # See forge_add_library's comment: in-tree bin/ already exists (created
    # by the `forge` executable target this custom command depends on);
    # out-of-tree nothing creates it, so the linker fails without this.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    add_custom_command(
        OUTPUT "${gen_bin}"
        COMMAND ${FORGE_EXECUTABLE} "${SOURCE}" -o "${gen_bin}"
            ${FORGE_CC_ARGS}
            --forge-root "${FORGE_ROOT_DIR}" --lib-dir "${FORGE_LIB_DIR}"
            # Always search this project's own lib/ output (where
            # forge_add_library places locally-built archives) in addition
            # to --lib-dir (the runtime libs, which live elsewhere when
            # FORGE_LIB_DIR is an installed prefix out-of-tree).
            -L "${CMAKE_BINARY_DIR}/lib"
            ${FAE_EXTRA_ARGS}
        DEPENDS ${FORGE_BUILD_DEPS} "${SOURCE}" ${FAE_EXTRA_DEPENDS}
        COMMENT "Compiling ${NAME}.fg to native binary"
        VERBATIM
    )

    # ALL: out-of-tree consumers expect `cmake --build .` to build the
    # executable by default, same as add_executable(). In-tree, this repo's
    # root CMakeLists.txt separately aggregates example targets under its own
    # `examples` ALL target — declaring ALL here too is redundant there but
    # harmless (a target can be a dependency of multiple ALL declarations).
    add_custom_target(${NAME} ALL DEPENDS "${gen_bin}")
endfunction()
