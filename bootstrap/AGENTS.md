# Self-hosted Forge compiler

`compiler.fg` is an independent scanner/checker/C emitter for the subset needed
to compile itself. It is not a generated copy of `compiler/`.

## Navigation

| Functions/region | Purpose |
| --- | --- |
| `skip_ws`, `matching_delimiter`, `find_stmt_end` | Shared string/comment-aware scans |
| `map_ident`, `compile_expr`, `compile_block` | Runtime naming and expression/statement emission |
| `compile_source_to` | Header, declarations, functions and main output |
| `validate_bytes`, `validate_source`, `validate_semantics` | Rejection before writing C |
| `build_native`, `native main` | CLI, SDK discovery and host compiler argv |

## Subset boundaries

Keep top-level `fn`/`const` in the line-start form recognized by its scanners.
The supported scalar types are int, bool, string and void; imports are
strings/fs/os/io. Coroutine lowering, arbitrary modules and full ownership
checking are outside this compiler's supported subset.
Stage0 wrapping/division semantics are not automatically implemented here;
self-hosted arithmetic still emits C operators. Document parity claims by case.
Validate names/types/flow before output creation. CLI failures must preserve an
existing output; source and output may not identify the same file.
Strings/views borrow arena storage; avoid resetting it while scans still refer
to source bytes. Native compiler invocation uses `os_command*` argv, not a shell.

## Fixed point

CMake builds stage0 -> stage1 -> stage2. `forge-selfhost-verify` has stage2 emit
stage3 C, compiles stage3, then compares its stage4 C byte-for-byte with stage3 C.
Generated C and binaries belong in the build tree. Edit this Forge source.
Run `selfhost_regressions` for CLI/semantic behavior as well as the fixed point;
identical compiler output establishes convergence, not full-language correctness.
The installed stage2 executable is named `forge-fg`.
