# Example of building a Forge (.fg) library from a project that lives
# completely outside the forge repository, against an installed Forge
# toolchain (`cmake --install` from the forge repo).
#
# Try it:
#   cmake -S <forge-repo> -B build
#   cmake --build build
#   cmake --install build --prefix /opt/forge
#
#   cmake -S examples/external-project -B external-build \
#       -DCMAKE_PREFIX_PATH=/opt/forge
#   cmake --build external-build
#   ./external-build/bin/hello
cmake_minimum_required(VERSION 3.16)
project(forge_external_example LANGUAGES C)

find_package(Forge REQUIRED)

forge_add_library(greeting_ext "${CMAKE_CURRENT_SOURCE_DIR}/libs/greeting_ext/greeting_ext.fg")

forge_add_executable(hello "${CMAKE_CURRENT_SOURCE_DIR}/main.fg"
    EXTRA_ARGS -I "${CMAKE_BINARY_DIR}/generated/libs" -l forge_greeting_ext
    EXTRA_DEPENDS forge_lib_greeting_ext
)

# Each consumer links the component that owns its API. The runtime is independent;
# the standard library declares its runtime dependency through its exported target.
add_executable(sdk_runtime sdk_runtime_consumer.c)
target_link_libraries(sdk_runtime PRIVATE Forge::forge_runtime)
add_executable(sdk_std sdk_consumer.c)
target_link_libraries(sdk_std PRIVATE Forge::forge_std)
