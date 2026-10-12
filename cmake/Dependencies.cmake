# Immutable versions; override source directories for local multi-repository work.
set(FORGE_RUNTIME_REVISION "0b1e7d4e6ae0996422553666e349fc40f11e3ac6")
set(FORGE_STDLIB_REVISION "a16cf1bf1fe81c92f64aa6df675a353674715c98")
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
