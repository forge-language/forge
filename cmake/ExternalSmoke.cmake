foreach(required FORGE_BUILD SOURCE_DIR TEST_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

function(run_step name)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
    if(NOT "${status}" STREQUAL "0")
        message(FATAL_ERROR "${name} failed (${status}):\n${out}${err}")
    endif()
endfunction()

set(prefix "${TEST_DIR}/install")
set(external_build "${TEST_DIR}/project")
run_step(install "${CMAKE_COMMAND}" --install "${FORGE_BUILD}" --prefix "${prefix}")
run_step(configure "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/examples/external-project"
    -B "${external_build}" "-DCMAKE_PREFIX_PATH=${prefix}")
run_step(build "${CMAKE_COMMAND}" --build "${external_build}")
execute_process(COMMAND "${external_build}/bin/hello"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 10)
if(NOT "${status}" STREQUAL "0" OR NOT out MATCHES "Hello from an external project, Forge")
    message(FATAL_ERROR "external program failed (${status}): ${out}${err}")
endif()
message(STATUS "Installed Forge built and ran an external library consumer")
