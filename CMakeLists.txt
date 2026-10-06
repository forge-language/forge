cmake_minimum_required(VERSION 3.16)
project(forge VERSION 0.3.0 LANGUAGES C)
include(GNUInstallDirs)
include(CTest)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
option(FORGE_ENABLE_LTO "Enable link-time optimization" OFF)
if(FORGE_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT FORGE_IPO_SUPPORTED OUTPUT FORGE_IPO_ERROR)
    if(NOT FORGE_IPO_SUPPORTED)
        message(FATAL_ERROR "LTO unsupported: ${FORGE_IPO_ERROR}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
include(cmake/Dependencies.cmake)

# Assemble the build SDK from the two repositories' exported headers. Compiler
# invocations and installed consumers use the same bin/include/lib layout.
set(FORGE_ROOT_DIR "${CMAKE_BINARY_DIR}")
set(FORGE_LIB_DIR "${CMAKE_BINARY_DIR}/lib")
file(MAKE_DIRECTORY "${FORGE_ROOT_DIR}/include" "${CMAKE_BINARY_DIR}/generated")
foreach(component IN ITEMS runtime stdlib)
    set(header_root "${forge_${component}_SOURCE_DIR}/include")
    file(GLOB_RECURSE public_headers CONFIGURE_DEPENDS "${header_root}/*.h")
    foreach(header IN LISTS public_headers)
        file(RELATIVE_PATH relative_header "${header_root}" "${header}")
        # configure_file registers the source header as a configure dependency,
        # so local component edits refresh the SDK before generated C is built.
        configure_file("${header}" "${FORGE_ROOT_DIR}/include/${relative_header}" COPYONLY)
    endforeach()
endforeach()
set(FORGE_CC_ARGS --cc "${CMAKE_C_COMPILER}")
add_executable(forge compiler/main.c compiler/lexer.c compiler/parser.c
    compiler/ast.c compiler/codegen.c compiler/codegen_js.c compiler/mod_registry.c
    compiler/module_loader.c compiler/symbols.c compiler/optimize.c compiler/driver.c)
target_include_directories(forge PRIVATE compiler "${FORGE_ROOT_DIR}/include")
target_link_libraries(forge PRIVATE forge_runtime)
install(TARGETS forge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

include(cmake/ForgeLibrary.cmake)
forge_add_library(greeting "${CMAKE_CURRENT_SOURCE_DIR}/libs/greeting/greeting.fg")
forge_add_library(mathutil "${CMAKE_CURRENT_SOURCE_DIR}/libs/mathutil/mathutil.fg")
file(GLOB FORGE_EXAMPLES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.fg")
foreach(example IN LISTS FORGE_EXAMPLES)
    get_filename_component(name "${example}" NAME_WE)
    if(name STREQUAL "use_library")
        forge_add_executable(${name} "${example}"
            EXTRA_ARGS -I "${CMAKE_BINARY_DIR}/generated/libs" -l forge_greeting -l forge_mathutil
            EXTRA_DEPENDS forge_lib_greeting forge_lib_mathutil)
    else()
        forge_add_executable(${name} "${example}")
    endif()
    list(APPEND FORGE_EXAMPLE_TARGETS ${name})
endforeach()
add_custom_target(examples DEPENDS ${FORGE_EXAMPLE_TARGETS})

set(FORGE_BOOTSTRAP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/bootstrap/compiler.fg")
set(FORGE_STAGE1_BIN "${CMAKE_BINARY_DIR}/bin/forge-stage1")
set(FORGE_STAGE2_C "${CMAKE_BINARY_DIR}/generated/forge-stage2.c")
set(FORGE_STAGE2_BIN "${CMAKE_BINARY_DIR}/bin/forge-stage2")
set(FORGE_BOOTSTRAP_LINK -lforge_std -lforge_runtime -lm)
if(WIN32)
    list(APPEND FORGE_BOOTSTRAP_LINK -lws2_32)
else()
    list(APPEND FORGE_BOOTSTRAP_LINK -pthread)
endif()
add_custom_command(OUTPUT "${FORGE_STAGE1_BIN}"
    COMMAND forge "${FORGE_BOOTSTRAP_SRC}" -o "${FORGE_STAGE1_BIN}"
        ${FORGE_CC_ARGS} --forge-root "${FORGE_ROOT_DIR}" --lib-dir "${FORGE_LIB_DIR}"
    DEPENDS forge forge_runtime forge_std "${FORGE_BOOTSTRAP_SRC}" VERBATIM)
add_custom_target(forge-stage1 DEPENDS "${FORGE_STAGE1_BIN}")
add_custom_command(OUTPUT "${FORGE_STAGE2_BIN}"
    COMMAND "${FORGE_STAGE1_BIN}" "${FORGE_BOOTSTRAP_SRC}" -o "${FORGE_STAGE2_C}"
    COMMAND ${CMAKE_C_COMPILER} -std=c11 -O3 "-I${FORGE_ROOT_DIR}/include"
        "${FORGE_STAGE2_C}" -o "${FORGE_STAGE2_BIN}" "-L${FORGE_LIB_DIR}" ${FORGE_BOOTSTRAP_LINK}
    DEPENDS forge-stage1 forge_std forge_runtime "${FORGE_BOOTSTRAP_SRC}" VERBATIM)
add_custom_target(forge-selfhost ALL DEPENDS "${FORGE_STAGE2_BIN}")
foreach(name IN ITEMS match control_flow)
    set(output "${CMAKE_BINARY_DIR}/generated/selfhost_${name}.c")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${FORGE_STAGE2_BIN}" "${CMAKE_CURRENT_SOURCE_DIR}/examples/${name}.fg" -o "${output}"
        DEPENDS forge-selfhost "${CMAKE_CURRENT_SOURCE_DIR}/examples/${name}.fg" VERBATIM)
    if(name STREQUAL "match")
        add_custom_target(forge-selfhost-test DEPENDS "${output}")
    else()
        add_custom_target(forge-selfhost-control DEPENDS "${output}")
    endif()
endforeach()
set(FORGE_STAGE3_C "${CMAKE_BINARY_DIR}/generated/forge-stage3.c")
set(FORGE_STAGE3_BIN "${CMAKE_BINARY_DIR}/bin/forge-stage3")
set(FORGE_STAGE4_C "${CMAKE_BINARY_DIR}/generated/forge-stage4.c")
add_custom_command(OUTPUT "${FORGE_STAGE4_C}"
    COMMAND "${FORGE_STAGE2_BIN}" "${FORGE_BOOTSTRAP_SRC}" -o "${FORGE_STAGE3_C}"
    COMMAND ${CMAKE_C_COMPILER} -std=c11 -O3 "-I${FORGE_ROOT_DIR}/include"
        "${FORGE_STAGE3_C}" -o "${FORGE_STAGE3_BIN}" "-L${FORGE_LIB_DIR}" ${FORGE_BOOTSTRAP_LINK}
    COMMAND "${FORGE_STAGE3_BIN}" "${FORGE_BOOTSTRAP_SRC}" -o "${FORGE_STAGE4_C}"
    COMMAND ${CMAKE_COMMAND} -E compare_files "${FORGE_STAGE3_C}" "${FORGE_STAGE4_C}"
    DEPENDS forge-selfhost forge_std forge_runtime "${FORGE_BOOTSTRAP_SRC}" VERBATIM)
add_custom_target(forge-selfhost-verify DEPENDS "${FORGE_STAGE4_C}")

install(FILES cmake/ForgeLibrary.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Forge)
include(CMakePackageConfigHelpers)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/ForgeConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
configure_package_config_file(cmake/ForgeConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ForgeConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Forge
    PATH_VARS CMAKE_INSTALL_BINDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_INCLUDEDIR)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ForgeConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ForgeConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Forge)
install(PROGRAMS "${FORGE_STAGE2_BIN}" DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME forge-fg OPTIONAL)
install(FILES bootstrap/compiler.fg DESTINATION ${CMAKE_INSTALL_DATADIR}/forge/bootstrap)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/forge)

if(BUILD_TESTING)
    add_executable(optimizer_test tests/optimizer_test.c compiler/ast.c compiler/optimize.c)
    add_executable(parser_test tests/parser_test.c compiler/parser.c compiler/lexer.c compiler/ast.c)
    foreach(name IN ITEMS optimizer parser)
        target_include_directories(${name}_test PRIVATE compiler)
        if(MSVC)
            target_compile_options(${name}_test PRIVATE /UNDEBUG)
        else()
            target_compile_options(${name}_test PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ${name}_regressions COMMAND ${name}_test)
    endforeach()
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME module_merge_regressions COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/module_merge_test.py" "$<TARGET_FILE:forge>")
        foreach(name IN ITEMS compiler driver selfhost org_integration)
            if(name STREQUAL "org_integration" AND NOT UNIX)
                continue()
            endif()
            if(name STREQUAL "selfhost")
                set(test_compiler "${FORGE_STAGE2_BIN}")
                set(extra_args --source-root "${CMAKE_CURRENT_SOURCE_DIR}")
            else()
                set(test_compiler "$<TARGET_FILE:forge>")
                set(extra_args "")
            endif()
            add_test(NAME ${name}_regressions COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}_test.py"
                --forge "${test_compiler}" --root "${FORGE_ROOT_DIR}"
                --lib-dir "${FORGE_LIB_DIR}" --cc "${CMAKE_C_COMPILER}" ${extra_args})
            set_tests_properties(${name}_regressions PROPERTIES TIMEOUT 120)
        endforeach()
        find_program(FORGE_NODE node)
        if(FORGE_NODE)
            add_test(NAME javascript_regressions COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/js_test.py" "$<TARGET_FILE:forge>")
        endif()
    endif()
endif()
