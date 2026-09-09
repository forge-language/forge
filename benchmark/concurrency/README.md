# Cross-language concurrency-primitive benchmark

Forge's headline claim is a "Hybrid Lightweight Process + Coroutine" model:
cheap concurrency on an M:N work-stealing scheduler with Erlang-style
reduction-budget preemption. HTTP hello-world throughput does not test that
claim. This benchmark does: it measures the cost of *one concurrency unit* in
each runtime, in time and in bytes.

Run it:

```sh
./run_concurrency_bench.sh                       # full sweep, 3 reps
REPS=5 N_VALUES="1 1000 10000" ./run_concurrency_bench.sh
```

Results land in `results/concurrency_results.txt`, with every individual run
recorded in `results/raw.tsv`.

## What is measured

For each language and each N in {1, 1000, 10000, 100000, 1000000}:

1. **Wall-clock milliseconds** to spawn and complete all N units, measured
   externally around the whole process (median of 3 runs, with min and max so
   variance is visible).
2. **Peak resident set size** of the process.
3. **Bytes per concurrency unit**, derived as
   `(peak_rss_at_N - peak_rss_at_N=1) * 1024 / N`.

The results file also carries a second table of *internal* timings — each
program's own measurement, started immediately before its spawn loop — so
process and runtime startup can be separated from the concurrency cost.

### The unit of work, identical everywhere

Every implementation does exactly this per unit:

```
suspend once (yield / await / Gosched / sched_yield)
increment a shared counter
```

and the parent then waits for all N. Every run **asserts the final counter
equals N** and the harness reports `WRONG_COUNT` if it does not, so a runtime
cannot score well by dropping work. The counter is printed, which is what
keeps the increment from being optimised away.

Per-language primitive and suspension point:

| Language | Primitive | Suspension | Counter |
|---|---|---|---|
| Forge | `spawn` coroutine | `yield` | C helper, atomic (see below) |
| Go | goroutine | `runtime.Gosched()` | `sync/atomic` + `sync.WaitGroup` |
| Rust (async) | `tokio::spawn` task | `tokio::task::yield_now()` | `AtomicI64` + `JoinHandle` |
| Rust (thread) | `std::thread` | `thread::yield_now()` | `AtomicI64` + `join()` |
| Java | **platform** thread | `Thread.yield()` | `AtomicLong` + `join()` |
| Node.js | `async` function | `await null` (microtask turn) | plain `let` + `Promise.all` |
| Python | asyncio task | `await asyncio.sleep(0)` | plain global + `asyncio.gather` |
| C | `pthread` | `sched_yield()` | `atomic_llong` + `pthread_join()` |

Compiled languages are built with real release optimisation: `gcc -O2`,
`cargo --release` (`opt-level=3`, `lto`, `codegen-units=1`), `go build`
default, and Forge through its own AOT native pipeline.

### How RSS is measured

GNU `/usr/bin/time` is **not installed** on this host, so `src/runner.c`
stands in for `time -v`: it `fork`/`exec`s the child, waits with `wait4()`,
and reports `ru_maxrss` — the same kernel counter GNU time prints as "Maximum
resident set size", and the same value as `VmHWM` in `/proc/self/status`.
One harness measures all seven languages, which is the only reason the RSS
column is comparable at all.

### Memory capping and failure handling

Each run executes inside a transient systemd scope with
`MemoryMax=4500M MemorySwapMax=0`. Disabling swap for the scope matters: with
swap available, a runtime that overshoots merely thrashes and reports a
*capped* `ru_maxrss` that understates its real demand. With swap off it is
OOM-killed instead, which is an honest result.

Lightweight runtimes additionally get `RLIMIT_AS = 3500 MB` on the child, so
they hit a graceful in-language allocation failure (with a real error message)
before the cgroup killer fires. The OS-thread runtimes are **not** given an
address-space cap, because each thread reserves ~8 MB of lazily-faulted
virtual address space; an `RLIMIT_AS` would fire on address space those
runtimes never touch. They are bounded by the cgroup cap and by the kernel's
own limits (`threads-max`, `ulimit -u`) instead.

A hard failure — OOM kill, thread-creation failure, timeout, wrong counter —
is recorded in the results table with its actual error text. Nothing is
silently dropped.

## Asymmetries we had to accept

These are real and they matter when reading the table.

### 1. Java on this host has no lightweight primitive at all

The JDK is 17.0.19. Virtual threads (JEP 444) landed in **JDK 21**. The Java
column therefore measures **OS platform threads** and belongs next to C and
Rust-thread, not next to Forge, Go, Node and Python. It is *not* evidence
about what Java concurrency costs on a modern JDK.

### 2. Forge needs a C helper to hold the counter

Forge 0.3.0 cannot express this benchmark's verification on its own:

- there are no module-level variables and no atomics, so nothing can be shared
  between coroutines;
- `on receive(...)` on a process is parsed but **never emitted by codegen**, so
  the message-handler route does not exist;
- `send`/`recv` do work, but the process mailbox is a fixed 256-slot ring
  (`MAILBOX_CAP` in `runtime/scheduler.c`) that **silently drops** on
  overflow, so counting N ≥ 1000 completions by message passing is impossible;
- `process main`'s body runs to completion *before* `fr_scheduler_run()` drains
  any coroutine, so the program cannot observe its own completion.

So `src/bench_counter.c` supplies the atomic counter and the internal timer,
linked in through Forge's own `extern fn` FFI. What it does per unit — one
`atomic_fetch_add` — is exactly what Go, Rust and C do with their standard
libraries, so the *work* stays equal. The asymmetry is that Forge is the only
language that had to reach outside itself to do it, and that its internal
timing is stamped by a library destructor at process exit, so it includes
scheduler teardown.

### 3. Forge's N is a compile-time constant

Forge has no string-to-int conversion in its standard library, so N cannot come
from `argv`. The harness rewrites `const UNITS` and recompiles per N. This
changes nothing about the measurement (the spawn loop still calls into the
runtime N times) but it is why `build/bench_forge_*.fg` exist.

### 4. Peak *simultaneous* liveness is runtime-determined, not forced to N

This is the most important caveat in the whole benchmark.

The specification is "spawn N units, each suspends once and completes, wait for
all N". It does **not** pin how many units are alive at the same instant, and
runtimes differ enormously here:

- **Go** begins running goroutines while the spawn loop is still going, so
  units retire continuously and stack memory is recycled. Its peak RSS at
  N=1000000 comes out *lower* than at N=100000 — a non-monotonic result that
  is proof of overlap, not of a smaller per-unit cost. Go's bytes/unit figure
  is a **lower bound**, not a per-unit price.
- **Forge** cannot overlap: `process main`'s body must finish spawning all N
  before the scheduler runs any of them. All N coroutine states are
  materialised before the first one executes, so Forge's bytes/unit is a
  **true** per-unit cost, measured under the harshest possible interpretation.

Comparing those two numbers directly flatters Go and penalises Forge. Read the
bytes/unit column with that in mind.

### 5. Bytes/unit is only meaningful at large N

At N=1000 the RSS delta over baseline is a few hundred kilobytes, most of it
one-time runtime warm-up (arena blocks, goroutine/task pool priming, JIT
metadata) rather than per-unit cost. Divided by 1000 it produces figures of
several kilobytes per unit that are pure noise. Read bytes/unit at N=100000 and
N=1000000; treat the small-N column as an artifact.

### 6. Startup and JIT warmup

The wall-clock column is whole-process time and includes interpreter/VM
startup, which is why Node, Python and Java look worse there than in the
internal-timing table. The internal table starts each timer immediately before
the spawn loop.

Two runtimes cannot exclude warmup even internally: **Node** (V8 has not
tiered up the unit function when the timer starts) and **Java** (same for
HotSpot). Both are therefore measured cold, and both would improve on a warmed
loop. Neither was given a warmup pass, because a warmup pass would have to
spawn the units it is trying to measure.

### 7. Rust appears twice

`std::thread` is the OS-thread variant; the tokio task variant is built
**offline** against the tokio already present in the local cargo registry
cache (the same 1.53 line the sibling `benchmark/axum` project pins), reusing
that project's `CARGO_TARGET_DIR`. If tokio were unavailable offline the
script would skip `rust_async` with a `SKIPPED:` line and Rust would be
represented by `std::thread` alone.

### 8. Counter contention differs slightly

Go, Rust, Java, C and Forge increment a genuinely atomic counter contended
across cores. Node and Python increment a plain variable, because both are
single-threaded by construction and an atomic there would be measuring
something that runtime never has to pay. This favours Node and Python by the
cost of an uncontended-to-contended cache line, which at these N is small
relative to the spawn cost but is not zero.

## Layout

```
run_concurrency_bench.sh   builds what it can, runs everything, writes results
src/runner.c               uniform wall-clock + peak-RSS harness (stands in for time -v)
src/bench_counter.c        atomic counter + internal timer for Forge, via extern fn
src/bench_forge.fg         Forge coroutines
src/bench_go.go            Go goroutines
src/bench_c.c              C pthreads (OS-thread baseline)
src/BenchThread.java       Java platform threads
src/bench_node.mjs         Node async functions
src/bench_python.py        Python asyncio tasks
rust_thread/               Rust std::thread crate
rust_async/                Rust tokio task crate (offline build)
results/                   generated: concurrency_results.txt, raw.tsv
build/                     generated: binaries, per-N Forge sources, build logs
```

Nothing outside this directory is read for configuration or written to, except
that `rust_async` and `rust_thread` share the existing
`benchmark/axum/bench_server/target` cargo target directory to build offline.
