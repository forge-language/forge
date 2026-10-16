# Compiler regression runners

Root `CMakeLists.txt` registers five C executables and nine Python unittest
programs. There is no local CMake file, pytest setup or shared fixture directory.

## Choose a surface

| Test | Coverage |
| --- | --- |
| `parser_test.c`, `optimizer_test.c` | Direct AST shape and optimization |
| `function_index_test.c`, `local_index_test.c`, `const_index_test.c` | Private header lookup behavior |
| `compiler_test.py`, `factory_faults.c` | Native output, coroutine behavior and GNU-wrapper failures |
| `diagnostics_test.py` | Source positions, text and structured diagnostics |
| `driver_test.py` | SDK discovery, toolchain argv and file/path handling |
| `arithmetic_test.py` | Python int64 oracle, O0/O3 UBSan and JS parity |
| `ownership_test.py` | ASan/UBSan generated programs and bridge lifetimes |
| `module_merge_test.py` | Imported declarations and source metadata |
| `org_integration_test.py` | HTTP/TLS, JSON/string and coroutine integration |
| `js_test.py` | Execute emitted JavaScript under Node |
| `selfhost_test.py` | Stage2 CLI and programs it builds; optional stage0 comparison |

## Runner conventions

Most Python runners accept `--forge --root --lib-dir --cc`, then preserve
remaining arguments for unittest selection. `--root` names the assembled SDK,
not this source checkout. Selfhost additionally accepts `--stage0 --source-root`;
module-merge and JS runners take the compiler as their first positional argument.
Use temporary source/output files, subprocess argv and bounded timeouts, matching
the runner's existing helpers. Assert observable output or diagnostics.
Keep C test assertions active in Release (`-UNDEBUG` or `/UNDEBUG`).

```bash
ctest --test-dir build --output-on-failure -R '^compiler_regressions$'
ctest --test-dir build --output-on-failure -R '^(parser|optimizer|function_index|local_index|const_index)_regressions$'
ctest --test-dir build --output-on-failure -R '^selfhost_regressions$'
```

## Platform and instrumentation limits

Ownership, arithmetic, diagnostics and organization integration register on UNIX.
Factory linker wrapping additionally requires Linux. Ownership sanitizes generated
C plus its bridge; it does not instrument the entire linked runtime.
Node discovery guards the standalone JS suite, but UNIX arithmetic also invokes
Node, so the full suite needs it. TLS skips when `http_has_tls()` is false;
enabled TLS scenarios also need the `openssl` executable.
CTest selfhost regressions do not perform the separate fixed-point build target.
