# Immutable versions; override source directories for local multi-repository work.
set(FORGE_RUNTIME_REVISION "dcbb1929b17c2f1d11fd367ecf80c7c87c58a274")
set(FORGE_STDLIB_REVISION "3e86c4c9c2f6646711b894942c92b927ea8578bf")
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
