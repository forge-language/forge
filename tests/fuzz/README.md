# Bounded parser, typed graph and checked-resource fuzzing

Use Linux Clang with libFuzzer. The harness forks per input because ordinary CLI parse
and semantic rejection exits with status 1. Status 0/1 is accepted; signals,
timeouts and sanitizer exit codes fail and preserve the minimized input.
Each child has a one-second CPU and two-second wall limit, no core dumps and a
64 KiB maximum input. ASan shadow mappings preclude a small RLIMIT_AS; input
size, existing nesting limits and the checked analysis's work budget bound this
smoke run. It never executes emitted unsafe C or opens source imports.
The linker wraps compiler exit calls so rejected inputs do not invoke the
inherited libFuzzer exit hook. Child coverage counters are copied through shared
memory back to the parent; otherwise fork copy-on-write would hide parser paths.

```sh
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DFORGE_BUILD_FUZZERS=ON \
  -DFORGE_RUNTIME_SOURCE_DIR=/absolute/path/to/forge-runtime \
  -DFORGE_STDLIB_SOURCE_DIR=/absolute/path/to/forge-stdlib
cmake --build build-fuzz --target forge_parser_fuzz -j2
ASAN_OPTIONS=detect_leaks=0:exitcode=86 UBSAN_OPTIONS=halt_on_error=1:exitcode=87 \
  build-fuzz/bin/forge_parser_fuzz tests/fuzz/corpus \
  -runs=10000 -max_total_time=60 -max_len=65536 -seed=20261022
```

Leak checking of valid programs belongs to the separate full ASan/UBSan lane;
fatal rejected-input cleanup is process teardown and is excluded in this fork
harness. Use a disposable corpus copy to retain the repository's fixed seeds.
The fixed typed-AST suite contains deliberately malformed graph controls.
Source-module resolution is exercised by module_merge_test.py and a separately
recorded bounded CLI corpus, since this harness deliberately performs no I/O.
