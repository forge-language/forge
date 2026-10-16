# Forge project guide

Generated: 2026-10-16. Discovery base: a750b60 on main, with local changes.

## Scope

Forge is an experimental systems language: C11 stage0 compiler, Forge-written
self-hosted subset, native C output and a separate JavaScript backend. Ownership,
concurrency and native speed are design goals; complete memory safety is not
implemented. Read `docs/design-principles.md` before changing optimization policy.

## Repository boundaries

| Location | Responsibility |
| --- | --- |
| `compiler/` | Stage0 lexer, AST, semantic checks, optimization, backends and CLI |
| `bootstrap/compiler.fg` | Independent self-hosted compiler; supported subset only |
| `cmake/` | Pinned component assembly and installed SDK helpers |
| `tests/` | Compiler, generated-program and SDK regression runners |
| `examples/`, `libs/` | Executable examples and generated library fixtures |
| `docs/` | Language decisions, diagnostics, integration and design guidance |
| `../forge-runtime/` | Scheduler, queues, events, arenas and platform primitives |
| `../forge-stdlib/` | Standard-library implementations and bridges |
| `../forge-benchmarks/` | Standalone performance fixtures, runners and raw reports |

Sibling paths describe the local multi-repository layout, not vendored sources.
Runtime and stdlib come from FetchContent when local overrides are absent.
Benchmark tools are independent consumers and are not compiler build inputs.
Moved compiler studies live in `../forge-benchmarks/docs/compiler/`.

## Where to look

| Task | Start here |
| --- | --- |
| CLI flags, phase ordering | `compiler/main.c`, `compiler/driver.c` |
| Language syntax or AST lifetime | `compiler/parser.c`, `compiler/ast.h` |
| Name/type/flow checks | `compiler/semantic.c` |
| Arithmetic or loop lowering | `compiler/optimize.c`, `compiler/codegen.c`, `docs/integer-arithmetic.md` |
| Native/JS parity | `compiler/codegen.c`, `compiler/codegen_js.c`, `tests/js_test.py` |
| Imports and ABI names | `compiler/module_loader.c`, `compiler/mod_registry.c` |
| Diagnostics/editor output | `compiler/source.c`, `compiler/symbols.c`, `docs/lsp.md` |
| Self-hosted behavior | `bootstrap/AGENTS.md`, `tests/selfhost_test.py` |
| Installed consumers | `cmake/ForgeConfig.cmake.in`, `examples/external-project/` |

## Code map

`main` parses, loads modules, checks semantics and optimizes before selecting
check-only, symbols, library, C/native or JS output. `--check` includes optimization.

| Symbol | Location | Role |
| --- | --- | --- |
| `parse_program_named` | `compiler/parser.c` | Source-aware AST construction |
| `forge_load_modules` | `compiler/module_loader.c` | Resolve and merge imports |
| `forge_check_program` | `compiler/semantic.c` | Partial name/type/flow validation |
| `optimize_program` | `compiler/optimize.c` | AST constant folding |
| `codegen_emit`, `codegen_emit_js` | `compiler/codegen.c`, `compiler/codegen_js.c` | Backend emission |
| `forge_driver_compile_program` | `compiler/driver.c` | SDK paths, C compiler argv and linking |

Map verified through AST and source inspection. C/Python LSP servers were absent
at discovery; semantic reference centrality is unmeasured.

## Build and verify

Run from this repository. For local component work:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DFORGE_RUNTIME_SOURCE_DIR="$PWD/../forge-runtime" \
  -DFORGE_STDLIB_SOURCE_DIR="$PWD/../forge-stdlib"
cmake --build build -j2
ctest --test-dir build --output-on-failure
cmake --build build --target forge-selfhost-verify -j2
```

Omit the two overrides to reproduce the published revisions in
`cmake/Dependencies.cmake`. Update pins only to published immutable revisions;
local candidates use source overrides. Build outputs form an SDK with
`bin/`, `include/`, `lib/`; test `--root` means that SDK directory.

Release is the default, LTO is opt-in. The default build includes stage2;
CTest and the explicit self-host fixed-point target are separate checks.
Python3 and Node are needed for the full UNIX regression set. CI also installs
the SDK and builds/runs all three `examples/external-project/` consumers.

## Change boundaries

Preserve traps, side effects, evaluation order and signed integer contracts when
optimizing. A benchmark name or expected output is not a proof of a rewrite.
Record baseline/candidate identities, paired observations and adverse cases in
the benchmark repository; correctness regressions stay with their component.

Untracked is a Git classification, not proof that a file is disposable. During
requested cleanup, remove a duplicate only after its canonical copy is verified;
record paths and hashes for unique evidence moves. Generated caches can be
deleted after checking no active process uses them. Preserve unrelated user work
and served website report projections; avoid blanket `git clean`.
