# Repository separation

Compiler baseline: `forge@725aefb`. The original history and files remain available
in Forge Git history. New repositories record this source in their notices;
existing editor repositories retain their original histories.

| Former path in forge | New repository/path |
| --- | --- |
| `runtime/` | `forge-runtime/src/` |
| `include/forge_runtime.h`, runtime headers | `forge-runtime/include/` |
| `tests/{arena,work_queue,scheduler}_test.c` | `forge-runtime/tests/` |
| `stdlib/`, standard module headers | `forge-stdlib/src/`, `include/` |
| `third_party/opencl/` | `forge-stdlib/third_party/opencl/` |
| `tests/{string,os,fs}_test.c` | `forge-stdlib/tests/` |
| `lsp/`, `tools/forge-lsp/` | `language-server/src/`, `native/` |
| `scripts/{install-claude-lsp.sh,patch_forge_lsp.py}` | `language-server/scripts/` |
| `editors/vscode/` | `vscode-extension/` root |
| `editors/{vim,nvim}/` | `editor-configs/{vim,nvim}/` |
| `benchmark/`, measured reports | `forge-benchmarks/benchmark/`, `docs/` |
| `lean/` | `forge-proofs/` root |

Platform, browser, web and PostgreSQL repositories were already separate and
continue using their own directories. Active unmerged native-backend worktrees
are not moved or rewritten by this migration.

## Dependency contract

`forge-runtime` is independent. `forge-stdlib` links the runtime, and `forge`
fetches both at fixed commits. Each component installs a CMake package and public
headers. Forge assembles a build SDK at `build/{bin,include,lib}`; no checkout
needs to be nested inside another source tree. Local source overrides are explicit,
so a dirty sibling checkout never silently replaces a pinned dependency.

Runtime headers no longer include stdlib I/O headers. Generated C explicitly
includes `forge/io.h`. Existing C consumers that call standard APIs must include
those headers and link `ForgeStd::stdlib`, rather than relying on a runtime/std
static-library cycle. The standard package exports its runtime dependency.

Independent library builds and tests:

```sh
cmake -S ../forge-runtime -B ../forge-runtime/build
cmake --build ../forge-runtime/build -j2
ctest --test-dir ../forge-runtime/build --output-on-failure
cmake -S ../forge-stdlib -B ../forge-stdlib/build \
  -DFORGE_RUNTIME_SOURCE_DIR="$PWD/../forge-runtime"
cmake --build ../forge-stdlib/build -j2
ctest --test-dir ../forge-stdlib/build --output-on-failure
```

The original measured reports are preserved in forge-benchmarks rather than copied
into compiler checkout. Measurements from older revisions remain historical data.
Compiler examples and library fixtures stay in forge because compiler regression
and self-hosting tests use them.
