# Immutable versions; override source directories for local multi-repository work.
set(FORGE_RUNTIME_REVISION "ddeba40e8c57b4e8bc46f273c519a4c497fe2a56")
set(FORGE_STDLIB_REVISION "8b896aadfba9da98c6349b3083f6dddfee9de70a")
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
