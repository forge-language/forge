# Toolchain / Developer-Experience Benchmark

This directory measures **developer experience and deployment** metrics for
Forge, alongside C (clang), Rust, Go, Java, Node.js, and Python — the things
that decide whether a language is pleasant to work in day-to-day and cheap to
ship, as opposed to `benchmark/`'s existing HTTP-throughput benchmarks. It
does not touch `compiler/`, `runtime/`, `stdlib/`, `include/`, or any other
`benchmark/` subdirectory.

## Run it

```sh
benchmark/toolchain/run_toolchain_bench.sh
```

Writes `benchmark/toolchain/results.txt` and also prints to stdout. Any
toolchain not found in `PATH` (or, for Forge, no compiler found at
`build-xlang/bin/forge`, `build/bin/forge`, or `build-bench/bin/forge`) is
skipped with a `SKIPPED: <lang> - <reason>` line rather than failing the run.

Tunable via environment variables:
- `BUILD_RUNS` (default `3`) — repetitions per build-time measurement; the
  script reports the median.
- `COLDSTART_RUNS` (default `60`) — repetitions per cold-start measurement.

The script creates its own scratch directory (`.scratch/`, deleted at the end
of a successful run) and never touches anything outside
`benchmark/toolchain/`.

## What's measured, and how

### Test programs (`src/<lang>/`)

Two programs per language:
- **hello** — prints one line.
- **concurrency** — spawns 10,000 lightweight units of work that each yield
  once, then joins them all. This is a heavier, more realistic compile-time
  stress test than hello world, and mirrors what
  `benchmark/forge/bench_coro.fg` (100,000 units) measures at runtime — here
  at the 10,000-unit scale the assignment calls for, and used purely to
  produce a nontrivial compile unit, not to measure scheduler throughput.

  Each language uses its own idiomatic "lightweight unit of concurrency,"
  not a forced-identical primitive, since that's what a developer would
  actually reach for:
  - **Forge**: `coroutine` + `spawn`, same shape as `bench_coro.fg`.
  - **C**: pthreads with a 64 KiB stack (C has no built-in coroutines).
  - **Rust**: `std::thread` (deliberately not an async runtime — pulling in
    Tokio etc. would mean compiling a dependency tree, which would measure
    crate-fetch/build time, not the language's own single-file compile
    speed).
  - **Go**: goroutines + `sync.WaitGroup`, `runtime.Gosched()` as the yield.
  - **Java** (JDK 17, predates virtual threads/JEP 444 in JDK 21): plain
    `Thread` with a 256 KiB stack, `Thread.yield()`.
  - **Node.js**: async functions scheduled via `setImmediate()` (the
    event-loop-native "yield once, resume next turn" primitive).
  - **Python**: `asyncio` tasks, `asyncio.sleep(0)` as the yield.

  All programs were run manually once to confirm each actually compiles and
  executes correctly (not just parses) before the benchmark script uses them.

### Section 1 — Compile / build time

For each language and each program, both a **cold** and a **warm** build are
timed (median of `BUILD_RUNS`, default 3):

- **Cold** = fresh, empty output directory and, where the toolchain has a
  persistent build cache (Go's `GOCACHE`), a *fresh empty cache directory
  too* — i.e. "first build on a machine that has never built this before."
  Each of the `BUILD_RUNS` cold repetitions gets its own fresh directories,
  so this is the worst case, not an average that benefits from a warmed-up
  filesystem cache.
- **Warm** = a persistent build/output directory that is reused, with a real
  one-line edit appended to the source file before each timed rebuild (an
  "edit-save-rebuild" inner-loop, not just a re-run of the identical
  command). For Go this exercises its real build cache (`GOCACHE` persists
  across the warm repetitions, so unchanged stdlib packages hit cache and
  only the touched file's package recompiles); for the others, none of them
  have a persistent single-file build cache, so warm and cold numbers should
  land close together — and the results confirm that (this is itself a
  finding, not a bug).

Interpreted languages (Node, Python) report `n/a (interpreted)` for both, as
instructed — there is no compile step to time.

**The Forge `.fg`→C vs. clang split** (the most interesting number here):
Forge's `--emit-c` flag runs only the frontend (parse + codegen to C text),
with no C compiler invoked at all. The script times that in isolation, then
separately times `clang -std=c11 -O3 -c … -o …` (the compile stage) and the
final `clang … -o …` link, using the exact same flags
`compiler/driver.c` passes by default (`-O3`, no LTO unless
`FORGE_ENABLE_LTO` is set — verified by reading the driver source). The sum
of the three stages is printed alongside a directly-measured, un-decomposed
`forge <src> -o <bin>` invocation as a sanity cross-check; they should be
(and are) in the same ballpark, with the residual being fork/exec and
temp-file overhead not attributed to any single stage.

**Single-threaded builds**: per the task's instructions, builds are made
single-threaded where a language exposes that knob, so timings reflect
compiler work rather than differing default parallelism on this 18-core
box:
- Go: `go build -p 1` (caps parallel build actions at 1).
- Rust: `rustc -C codegen-units=1` (forces single-unit, serial codegen —
  rustc has no stable frontend `-j` flag; this is the closest real one).
- clang, Forge (which just shells out to clang), and javac compile a single
  translation unit / file each — there is no parallelism to disable.

### Section 2 — Artifact size

For each compiled language: unstripped binary size, and stripped size
(`strip` run on a copy, original left untouched), plus `ldd` output so a
statically-linked binary (Go) is visibly distinguished from a dynamically
linked one (C, Rust, Forge).

For Java: `.class` file size and a `.jar` built with `jar cfe`, plus an
explicit note that neither is a standalone deployable artifact — it needs a
JVM.

For Node/Python: source file size, plus an explicit note of the interpreter
dependency and that machine's installed interpreter size (cross-referenced
to Section 4) — a tiny source file is not the deployment story by itself.

### Section 3 — Cold start

Time from process exec to completed output, for the hello-world program,
per language. `hyperfine` is used if installed; this environment does not
have it, so the script's documented fallback runs instead: a pure-bash loop
(`EPOCHREALTIME`, no per-sample subprocess forked for timing itself) over
`COLDSTART_RUNS` (default 60) iterations, reporting median and p99 in
milliseconds.

### Section 4 — Toolchain footprint

Best-effort installed size per toolchain, using whatever signal is
reasonably available for that toolchain (`pacman -Qi` package size on this
Arch machine, or `du -sh` of the toolchain's own install directory),
explicitly excluding unrelated cached artifacts that happen to live nearby
(this machine's `~/.cargo/registry` is downloaded crate cache from prior,
unrelated Rust builds — not part of the Rust toolchain; its `nvm` Node
directory also holds several unrelated globally-installed npm packages).
Where a reasonable number can't be determined, the script says "unknown"
rather than guessing.

## Caveats

- All timings are wall-clock on a shared, otherwise-idle machine at the time
  of the run; expect run-to-run noise, especially for the sub-200ms
  measurements. `results.txt` is the median of 3 build runs / 60 cold-start
  runs specifically to blunt that.
- The `.fg`-to-C vs. clang split's three sub-stages are timed as separate
  process invocations reconstructing Forge's own driver command, so their
  sum is an approximation of (not identical to) the single, real
  `forge <src> -o <bin>` invocation timed alongside it for cross-check.
- "Cold" Go builds with an empty `GOCACHE` recompile the Go standard library
  packages the program imports from scratch, which is why Go's cold numbers
  are far higher than its warm numbers (seconds vs. hundreds of
  milliseconds) — this is real, not a bug, and is exactly the tradeoff a
  cache-based toolchain makes.
