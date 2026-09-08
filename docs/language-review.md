# Implementation review — 2026-09-08

Forge is a useful language prototype with an unusually broad working surface:
C generation, native executables, libraries, coroutines, a scheduler and an LSP.
The next priority should be correctness and precise guarantees before adding
more serving modes or syntax. This review is targeted, not a complete security
or concurrency audit.

## Corrected in this review

- External libraries: the compiler parsed `-L` but discarded the directories
  when linking. Installed `bin/forge` prefix detection had also been lost during
  the previous PR conflict resolution. Both are restored and tested by building
  and executing the external-project example outside the repository.
- Path precedence: resolve toolchain defaults after processing CLI options so
  explicit `--forge-root` is honored before environment and automatic detection.
- Static archive dependencies: rescan the standard library after the runtime,
  which can introduce additional standard library references.
- Arena allocation: reset previously left a chain that the next expansion
  overwrote, leaking retained blocks. Allocation now traverses and reuses that
  chain. Alignment now uses the actual returned address; invalid alignments
  and overflowing allocation sizes are rejected.
- Process output: reaching the 1 MiB capture limit no longer stops pipe reads.
  The remaining output is drained so the child can finish before `pclose` waits.
  The compiler subprocess wait retries interrupted waits. Windows redirection
  restores descriptors even after a partially failed setup.
- Standard function result types: direct printing of the new document store,
  JSON, process, filesystem and LSP string results uses string output rather
  than numeric pointer output.
- Windows temporary files: removed an undefined move flag, limited collision
  retries and handled non-collision errors. Linux tests do not establish Windows
  compatibility; that path still needs a Windows build and execution job.
- README: corrected the temporary-C pipeline, scheduler and supervisor claims.

## Remaining language design priorities

1. Add a semantic checking pass before C generation. `--check` currently parses,
   loads modules and optimizes, then returns without running code generation or
   a dedicated type checker. It must not be treated as a guarantee of type-safe,
   executable code. Define type errors, call signatures and ownership rules in
   this pass and use its diagnostics in the LSP.
2. Define scheduling and failure guarantees. The reduction budget counts
   `run_coro_step` calls; it cannot interrupt a step that does not return.
   `restart_policy` is stored by supervisor creation but not consumed by a
   restart mechanism. Automatic fault recovery requires explicit restartable
   state and lifetime rules, rather than only a policy enum.
3. Make standard library signatures authoritative metadata. Module registration
   and the code generator's string-return table are separate and can drift.
   A typed registry should eventually drive call validation, code generation,
   completion and documentation together.
4. Specify integer overflow, floating-point behavior, string lifetimes and
   ownership across threads. Optimizer identities must preserve these semantics,
   including floating-point special values and side effects. A Lean proof about
   the modeled subset is not a proof of all C backend or scheduler behavior.
5. Extend CI beyond compilation. The regression suite added here builds native
   code, links an external library, executes an LSP diagnostic exchange, and
   exercises allocator and subprocess behavior. Concurrency stress tests,
   sanitizer runs and real Windows/macOS jobs remain necessary before claiming
   production reliability across platforms.

Run the targeted Linux regressions with `python3 tests/test_regressions.py`.
They use GCC (or `CC`), `ar` and Python's standard library and disable optional
TLS/GPU/io_uring backends in their direct builds. They do not exercise those
backends or replace the CMake build job.

## Optimization follow-up

The follow-up suite passes eight tests, including AddressSanitizer and UBSan
checks of arena reset/reuse and a native differential test at `-O0` through
`-O3`. Optimization levels now select both the C compiler level and, for `-O0`,
disable Forge's optional AST optimization pass.

Removed untyped algebraic identities (such as `x * 0`) from Forge's optimizer:
these could erase NaN values or change expression types. Constant-condition
branches retain their lexical blocks. The typed C optimizer can still remove
unnecessary arithmetic and branches. Floating literals now use C99 hexadecimal
notation to preserve double precision and floating rather than integer typing;
non-finite folding results remain expressions. Integer comparison folding also
releases its operand nodes.

`python3 benchmark/compare_optimization.py` reproduces a 10-million-step integer
recurrence benchmark with seven measured process runs and a warmup per level.
On this Linux x86-64/GCC 13.3 environment the median times were 75.94, 74.79,
74.27 and 75.55 ms for O0/O1/O2/O3 respectively. All produced checksum
1435658887 and 22584-byte executables; compilation took approximately 105–112 ms.
Samples varied substantially (roughly 42–78 ms across optimized runs).
These data do not demonstrate a meaningful O2/O3 advantage. Keep the existing
O3 default, offer explicit levels for reproducible comparisons, and do not
claim general throughput gains from this single microbenchmark.
