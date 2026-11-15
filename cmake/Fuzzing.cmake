option(FORGE_BUILD_FUZZERS "Build bounded Clang/libFuzzer input harnesses" OFF)
if(FORGE_BUILD_FUZZERS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR FORGE_SANITIZER STREQUAL "thread")
        message(FATAL_ERROR "The fork fuzz harness requires Linux Clang and must be separate from ThreadSanitizer")
    endif()
    add_executable(forge_parser_fuzz tests/fuzz/parser_fuzz.c
        compiler/lexer.c compiler/parser.c compiler/ast.c compiler/source.c
        compiler/semantic.c compiler/ownership.c compiler/resource_contracts.c
        compiler/typed_ast.c compiler/typed_lookup.c compiler/optimize.c compiler/mod_registry.c
        compiler/safety.c compiler/safety_expr.c compiler/safety_calls.c compiler/safety_flow.c compiler/safety_loop.c)
    target_include_directories(forge_parser_fuzz PRIVATE compiler)
    target_compile_options(forge_parser_fuzz PRIVATE -D_GNU_SOURCE -g -O1 -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(forge_parser_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -Wl,--wrap=exit)
    target_link_libraries(forge_parser_fuzz PRIVATE m)
endif()
