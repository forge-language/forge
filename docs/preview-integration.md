# Preview integration into the organization compiler

This branch merges the preview history through `ba36611f92fdd444a4c29827d9020e8bcff05dde` into the existing organization history through `7392f22`. It keeps the organization compiler's symbol maps, typed printing, decoded string escapes, nested coroutine locals, eager leading-let initialization, installed CMake package, native LSP, JSON path helpers, and HTTP routing/TLS/serving modes.

The preview adds checked constant folding, imported extern merging, JavaScript output, CLI bounds, reusable aligned arenas, string views/builders, pooled queue nodes, and scheduler conditions that distinguish runnable work from parked coroutines.

Conflict resolutions preserve the organization runtime's public integer return contract for `fr_sched_pool_submit` and `fr_native_queue_push` (zero means success), queued-native-work inspection, and per-coroutine scratch arenas. The new scheduler commits coroutine results under its state mutex; it does not retain the older atomic queue-claim protocol alongside that mutex protocol. Arena provider publication uses an atomic function pointer, and coroutine arenas remain alive across worker migration until their owning process is destroyed.

The organization lexer already decodes string escapes. JavaScript emission therefore escapes decoded bytes once; a literal backslash followed by `n` stays literal. `str_builder_finish` is registered in the organization's sorted string-return table, and the new strings functions appear in LSP completion results.

The external-project verification exposed an existing driver defect: `-L` directories were parsed but omitted from the linker command. Linking now includes these directories, allowing an installed SDK to build and link the separate `greeting_ext` archive.

Validation completed on Linux x86_64 with GCC 13.3:

- Debug and Release builds compile all examples and the native LSP.
- Nine CTest suites pass, including six organization integration tests covering HTTP prepared, worker, hybrid, io_uring fallback, sendfile, routing and TLS responses; LSP initialization/completion/shutdown; nested JSON and escapes; and coroutine arena isolation.
- The bootstrap stage 2/stage 3 fixed point produces identical generated C.
- `cmake --install` to an isolated prefix followed by the external-project example builds and prints `Hello from an external project, Forge`.

OpenSSL was enabled. liburing was unavailable, so the io_uring fallback was tested rather than native io_uring; GPU support was disabled for the build. The performance report describes the preview snapshot measured there, not a new benchmark of the organization compiler's additional HTTP implementations. The existing Lean CI job remains in place; proof source did not change in this integration.
