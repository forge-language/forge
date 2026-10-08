# Immutable versions; override source directories for local multi-repository work.
set(FORGE_RUNTIME_REVISION "39ab3daa90f15852cbbf4dd97f5d4c1502bd392d")
set(FORGE_STDLIB_REVISION "f0b92f8846f4797fd057b2c8b415f6bdba87a699")
include(FetchContent)
foreach(component IN ITEMS runtime stdlib)
    string(TOUPPER "${component}" upper)
    set(FORGE_${upper}_SOURCE_DIR "" CACHE PATH "Local forge-${component} checkout")
    if(FORGE_${upper}_SOURCE_DIR)
        FetchContent_Declare(forge_${component} SOURCE_DIR "${FORGE_${upper}_SOURCE_DIR}")
    else()
        FetchContent_Declare(forge_${component}
            GIT_REPOSITORY https://github.com/forge-language/forge-${component}.git
            GIT_TAG "${FORGE_${upper}_REVISION}")
    endif()
    FetchContent_MakeAvailable(forge_${component})
endforeach()
