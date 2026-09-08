# external_project

Build a standalone CMake project outside this repo, using an installed Forge toolchain via
`find_package(Forge)`.

## Source

- `examples/external-project/CMakeLists.txt`
- `examples/external-project/main.fg`
- `examples/external-project/libs/greeting_ext/greeting_ext.fg`

## Features

- `cmake --install` — package the compiler, runtime archives, headers, and CMake config into a prefix
- `find_package(Forge REQUIRED)` — locate an installed Forge toolchain from any CMake project
- `forge_add_library()` / `forge_add_executable()` — the same helpers used in-tree, now usable
  out-of-tree
- `EXTRA_ARGS` / `EXTRA_DEPENDS` — cross-link an executable against a library built earlier in the
  same project
- Contrasts with [use_library](use_library.md) (in-tree library build)

## Code

`libs/greeting_ext/greeting_ext.fg`:

```forge
library greeting_ext {
    import strings;

    export fn hello(name: string): string {
        return str_concat("Hello from an external project, ", name);
    }
}
```

`main.fg`:

```forge
import io;
import greeting_ext;

process main {
    println(greeting_ext.hello("Forge"));
}
```

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(forge_external_example LANGUAGES C)

find_package(Forge REQUIRED)

forge_add_library(greeting_ext "${CMAKE_CURRENT_SOURCE_DIR}/libs/greeting_ext/greeting_ext.fg")

forge_add_executable(hello "${CMAKE_CURRENT_SOURCE_DIR}/main.fg"
    EXTRA_ARGS -I "${CMAKE_BINARY_DIR}/generated/libs" -l forge_greeting_ext
    EXTRA_DEPENDS forge_lib_greeting_ext
)
```

## Build

First, install a Forge toolchain to a prefix (from this repo):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /path/to/forge-install
```

This installs the `forge` binary, `libforge_std.a` / `libforge_runtime.a`, headers, and a relocatable
`ForgeConfig.cmake` package under `/path/to/forge-install`.

Then, from the external project, point `find_package` at that prefix and build normally:

```bash
cd examples/external-project
cmake -B build -DCMAKE_PREFIX_PATH=/path/to/forge-install
cmake --build build
./build/bin/hello
```

`forge_add_library` compiles `greeting_ext.fg` into `libforge_greeting_ext.a` + `greeting_ext.h` inside
the external project's own build tree. `forge_add_executable` always searches that build tree's `lib/`
directory in addition to the installed toolchain's runtime libs, so `-l forge_greeting_ext` resolves
without any extra linker configuration — `EXTRA_ARGS`/`EXTRA_DEPENDS` only need to name the library
being linked and its build target.

Manual equivalent (no CMake):

```bash
/path/to/forge-install/bin/forge --lib libs/greeting_ext/greeting_ext.fg \
    -o libforge_greeting_ext.a --header greeting_ext.h \
    --forge-root /path/to/forge-install --lib-dir /path/to/forge-install/lib

/path/to/forge-install/bin/forge main.fg -o hello \
    --forge-root /path/to/forge-install --lib-dir /path/to/forge-install/lib \
    -L . -l forge_greeting_ext -I .
```

## Expected output

```
Hello from an external project, Forge
```

## In-tree vs out-of-tree

| Approach | `forge` binary | `--lib-dir` | Extra library search path |
|----------|-----------------|-------------|----------------------------|
| In-tree ([use_library](use_library.md)) | Built by this repo | This build's `lib/` | Not needed — everything lives in one `lib/` |
| Out-of-tree (this example) | Installed toolchain (`find_package(Forge)`) | Installed prefix's `lib/` | Consuming project's own build `lib/`, added automatically by `forge_add_executable` |
