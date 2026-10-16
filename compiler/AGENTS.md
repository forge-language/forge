# Stage0 compiler internals

This flat directory implements the C compiler. `main.c` owns phase ordering;
`driver.c` invokes the host toolchain. Self-hosted Forge code is separate.

## File map

| Files | Responsibility |
| --- | --- |
| `lexer.*`, `parser.*`, `ast.*` | Tokens, recursive parser and AST allocation/free |
| `semantic.*` | Visible signatures, initialization, control flow and send checks |
| `optimize.*` | AST folding before every output mode |
| `codegen.c`, `codegen_js.c`, `codegen.h` | C/library/coroutine output and JS output |
| `module_loader.*`, `mod_registry.*` | Source modules versus SDK imports, symbol mangling |
| `source.*`, `symbols.*` | Byte-based source spans, diagnostics and symbol JSON |
| `function_index.h`, `const_index.h`, `local_index.h` | Pass-owned lookup accelerators |
| `common.h` | Strings and checked array growth helpers |

## Borrowed storage

`ForgeStr` borrows bytes; keep the entry source and imported `FileModule` buffers
alive until their AST users are freed. Source spans are raw byte offsets with
an exclusive end; position conversion belongs in `source.c`.
Extend declaration storage with checked helpers; do not publish capacity changes
before allocation succeeds. Imported declarations retain their source metadata.

## Lookup invariants

Function and constant indexes borrow immutable arrays for one pass. Sort auxiliary
entries, never AST declarations: first-declaration resolution and output order
must survive duplicate names. Small/unqueried owners stay cheap; promotion spends
a comparison budget rather than allocating eagerly.

Local indexes store binding positions, not pointers into reallocating arrays.
Types, initialization, flow and ownership remain in the owning binding stack.
Mirror every push and scope truncation; preserve latest-binding shadowing.
Callers enforce the `ForgeStr` first-member/stride contract with layout checks.
Constant indexes cache names only; inferred type/value can depend on local scope.

## Backend constraints

Read the integer contract before replacing generated arithmetic. Preserve traps
and intermediate overflow, short-circuiting and single evaluation of operands.
Proof-based loop lowering needs rejection cases for changed bounds, effects,
shadowing and overflow, as well as a fast-case example.

Coroutine suspension uses generated state-machine labels. Hoist declarations
that must survive re-entry, while evaluating their initializers at the original
statement; jumping past initialization must not recreate or lose state.
Generated scheduler/process construction checks failure before ownership transfer.
Native emission changes do not automatically apply to the JS or self-host backend.

## Verification pointers

Use `tests/parser_test.c` and `tests/optimizer_test.c` for AST behavior;
`tests/*_index_test.c` for lookup boundaries and mutation/lookup sequences.
Generated behavior belongs in compiler/arithmetic/ownership/JS regressions.
For diagnostic changes, check both JSON spans and legacy text through
`tests/diagnostics_test.py`. Native output alone cannot establish JS parity.
