# Forge measured against other languages

Assessed 2026-09-09 on branch `http-serving-modes-and-runtime-hardening`.

This is a second opinion, written after building a cross-language benchmark
harness rather than reading the existing one. It complements
`docs/review-2026-09-09-refactor-and-language.md`, which verified the refactor
and judged the language on its own terms; this document asks a narrower and
harsher question — put Forge next to the languages it is implicitly competing
with, measure it, and say whether the numbers change the verdict.

They do, in both directions.

---

## Verdict

Forge's concurrency primitive is the real thing. At one million live tasks it
uses **15× less memory than Go and than Erlang**, and finishes 2.5–5× faster
than either. That is not a rounding difference or a benchmark artifact; it is a
consequence of a specific design decision, and it is the best thing in the
project.

The same decision is why its supervisors cannot work. Forge's coroutines are
stackless state machines sharing one address space with no private heap, which
is exactly why they cost 180 bytes — and exactly why nothing in the runtime can
isolate, kill, or restart one. BEAM pays 2,700 bytes per process for a private
heap and mailbox, and what it buys with them is the fault tolerance Forge
advertises in its README and does not implement.

So the headline is not "Forge is faster than Erlang." It is that Forge has
copied Erlang's vocabulary — `process`, `supervisor`, `restart`, mailboxes,
reduction-budget preemption — onto a runtime whose central design choice
forecloses the guarantee that vocabulary exists to express. The performance win
and the missing feature are the same fact viewed from two sides.

On HTTP throughput, Forge is genuinely good and the measurement is genuinely
inconclusive at the top of the table. It beats Go, Node and BEAM decisively on
throughput per core -- 48,211 against `go-raw`'s 12,600 and `erlang-beam`'s
6,370 -- and it cannot be distinguished from C, because neither it nor C is the
bottleneck in this harness. It does not beat Python by any margin this harness
can resolve: `python-sync` reads 46,072 rps/core, 4% behind, because it uses
0.71 cores to serve a third of Forge's requests. That is the metric rewarding a
server for declining to scale, not Python competing.

The finding I did not expect is internal. `forge-uring`, the newest and most
intricate of the three serving modes, is the worst of them per core: 25,111
against `forge-mt`'s 48,211, burning 3.69 cores to 1.91 for 16% fewer
requests. Both `forge-hybrid` and `forge-uring` are strictly worse than the
plain multi-threaded path they were added alongside, and nothing in the
repository distinguishes them for a user choosing one.

I would still not start a project on Forge today, for the reason the earlier
review gave: a language with no way to express failure has to break itself to
become viable. Nothing measured here changes that. What the numbers change is
the reason to care: this is no longer "an unfinished language on a decent
runtime." The runtime is better than decent, and that makes the missing
language work worth doing rather than worth abandoning.

---

## Method, and what it cost to make the numbers mean anything

Every absolute HTTP figure previously in this repository is load-generator
bound and should be disregarded, including the 8,014 rps in
`benchmark/refactor_comparison_results.txt` — that script's own trailing note
says as much. The generator was a Python thread pool contending on the GIL, so
it measured Python.

What replaced it:

- **A Go load generator** (`benchmark/loadgen`), stdlib only. Against the C
  reference it sustains ~158k rps where the Python harness managed ~8k, a 20×
  difference. Two concurrent generator instances reach 170,676 rps versus
  163,678 for one, so a single instance extracts ~96% of its own ceiling.
- **Enforced wire parity** (`benchmark/xlang/probe_wire.py`). Every server must
  answer with the same 70 bytes and two headers; the harness refuses to time
  one that does not. Getting Go and Node to comply required
  `w.Header()["Date"] = nil` and `res.sendDate = false` — suppression calls
  nobody writes in production, which is itself worth noticing.
- **Throughput per core as the headline metric**
  (`benchmark/xlang/monitor.py`). Raw rps rewards a server for consuming more
  of the 18 available cores. The monitor walks the whole process tree for CPU
  jiffies and peak RSS, so a server that reaches its number by burning six
  cores is reported as having done so.

### Where these numbers are weak

Stated plainly, because two of these caveats are load-bearing:

1. **Forge and C are not distinguishable here.** `c-pthread` reads 1.78 cores
   and `forge-mt` 1.91, both while topping the raw-rps column -- the signature
   of a generator-bound measurement rather than a fast server. The 25% gap
   between them on rps/core is smaller than this harness's own run-to-run
   spread (caveat 3). Any claim that Forge approaches C, or that C beats Forge,
   is unsupported here; only servers below roughly 60k rps are genuinely
   server-bound.
2. **rust-axum is handicapped.** It is the one server that would not conform to
   the wire spec: 148 bytes and four headers, because hyper adds `date` and
   axum adds `content-type`. It is doing more work per response than everything
   else in the table. Its p50 and p99 are the best measured, 1.6 ms and
   11.6 ms, and its `max_ms` is 4.1 *seconds*, reproduced in all three runs --
   a stall rather than load, and worth investigating separately.
3. **The machine degrades over a suite, the mitigation is partial, and two of
   the three instruments built to prove otherwise failed.** This box idles at
   72 C and reaches 98-104 C under load, where it drops to about 1,650 MHz
   against a 4,500 MHz rated maximum. The first suite in this shape ran each
   server's three measurements back to back and declined monotonically inside
   every one of them, which made position in the server list and thermal state
   the same variable. Those results were discarded.

   The rebuilt harness gates each measurement until the package falls back to
   78 C, rotates through all eleven servers each round rather than finishing
   one before starting the next, and records temperature and mean clock in
   every row. Round-major ordering is the part that worked, and it is the part
   that matters: every server is sampled in all three rounds, so a per-round
   effect lands on all of them alike and the medians stay comparable.

   The gate did not stop the decline. Nine of eleven servers still fell
   monotonically across rounds -- `forge-mt` 177,140 -> 98,351 -> 90,794 rps,
   `c-pthread` 154,125 -> 113,610 -> 99,731 -- and rounds 2 and 3 began at the
   same 79 C while differing by 10-12%, so start-of-run die temperature does
   not explain it. Whatever the mechanism is, it has a longer time constant
   than the die, and I did not identify it. The two servers that do not decline
   (`python-sync` 32,414 -> 32,711 -> 32,980, `erlang-beam` 46,592 -> 51,408 ->
   51,882) are the two lowest on rps/core, which is consistent with the decline
   touching only servers limited by the machine rather than by their own
   runtime -- suggestive, not established.

   `start_mean_mhz` turned out to be worthless: it is sampled while cores idle
   immediately after the gate, so it measures idleness rather than capability,
   and it has no rank agreement with throughput in ten of eleven servers and
   inverse agreement in the eleventh. One `forge-mt` row also records 93 C
   despite the gate having returned, and I could not reproduce a cause; the die
   reading is momentary and that row is an unexplained outlier. Treat the
   recorded thermals as a disclosure of what was tried, not as evidence the
   comparison is clean.

   Consequences for reading the table: **run-to-run spread reaches 49%, not the
   15% I first assumed.** Round 1 is inflated for every machine-bound server
   and the median of three excludes it, which is what medians are for. But a
   gap smaller than roughly 2x among the top four rows is unresolved by this
   harness, and that is precisely the range Forge and C fall in.
4. Generator and servers share the same 18 cores, over loopback. This
   compresses the top of the table and is the direct cause of caveat 1.
5. The generator sets `SO_LINGER {on,0}`, so client connections are reset
   rather than closed gracefully, and no server's connection-teardown path is
   exercised. Process shutdown was a separate instance of the same blind spot:
   the harness's own teardown looped on `kill -0` with no delay, spent its
   hundred iterations in microseconds, and reached `SIGKILL` before any server
   could run a signal handler -- every server in every previous run was torn
   down as though it had crashed. Fixed here, but it means graceful shutdown
   has still never been measured for any of these servers, Forge included.
6. **The instrument was a participant.** The per-run resource monitor scanned
   every entry in `/proc` on each 100 ms tick -- about 2,800 processes -- which
   cost roughly 0.4 of a core while measuring servers that consume about 2.
   Worse, if its target never appeared it spun at 20 ms per iteration forever;
   one such orphan outlived its suite and sat at 36% of a core for seventeen
   minutes, quietly taxing every measurement taken after it. Both are fixed
   (bounded startup wait, topology rediscovered once a second rather than every
   tick, 6.7x cheaper at identical readings), but any figure in this repository
   predating that fix carries an unknown amount of the instrument in it.
7. Python's server originally called `listen(128)` where every other server
   uses 8192. At concurrency 400 that overflows the accept queue, the kernel
   drops SYNs, and the retries showed up as multi-second latencies — measuring
   the `listen()` argument, not Python. Corrected to 8192 before the run
   reported here.

### A note on Forge's own contribution to the method

Forge has **no HTTP keep-alive**. All four `Connection:` sites in
`stdlib/http.c` hardcode `close`. Connection-per-request is therefore the only
shape in which Forge can be compared to anything, so that is the shape every
server was measured in. This is a limitation of Forge presented as a
methodology choice, and it flatters nobody: keep-alive is where the other
runtimes' request pipelines would start to pay off.

---

## HTTP: throughput per core

Medians of three runs, 300,000 requests at concurrency 400, connection per
request, all on one 18-core box over loopback. Sorted by throughput per core.

| Server | rps | cores used | **rps/core** | p50 ms | p99 ms | max ms | peak RSS | resp bytes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `c-pthread` | 113,610 | 1.78 | **60,431** | 2.6 | 18.8 | 47 | 1.9 MB | 70 |
| `forge-mt` | 98,351 | 1.91 | **48,211** | 2.8 | 22.0 | 49 | 4.2 MB | 70 |
| `python-sync` | 32,711 | 0.71 | **46,072** | 8.7 | 37.7 | 51 | 9.8 MB | 70 |
| `forge-hybrid` | 93,884 | 3.05 | **30,782** | 3.1 | 24.2 | 49 | 4.6 MB | 70 |
| `forge-uring` | 82,616 | 3.69 | **25,111** | 3.3 | 29.1 | 62 | 5.1 MB | 70 |
| `rust-axum` | 47,478 | 2.97 | **15,566** | 1.6 | 11.6 | **4,089** | 5.5 MB | *148* |
| `node-single` | 11,786 | 0.86 | **13,705** | 26.9 | 72.4 | 89 | 124 MB | 70 |
| `go-raw` | 63,002 | 4.92 | **12,600** | 3.7 | 40.8 | 75 | 21 MB | 70 |
| `go-nethttp` | 38,353 | 4.86 | **7,764** | 7.0 | 36.2 | 47 | 20 MB | 70 |
| `erlang-beam` | 51,408 | 8.07 | **6,370** | 5.0 | 35.6 | 73 | 102 MB | 70 |
| `node-cluster` | 14,653 | 6.50 | **2,254** | 18.8 | 65.4 | 111 | **2,372 MB** | 70 |

Five things in that table matter, and one of them is about the metric rather
than the servers.

**Read the two throughput columns together or not at all.** `python-sync`
places third on rps/core while serving a third of Forge's requests, because it
uses 0.71 of a core and a per-core figure rewards a server for declining to
scale. It is not competitive with Forge on anything; it is a single process
that saturates itself and stops. The per-core column answers "how much does
this runtime extract from the hardware it occupies," which is the right
question for comparing runtimes and the wrong one for sizing a deployment.

**Forge's best mode is within noise of C.** 48,211 against 60,431 is a 25% gap,
and the run-to-run spread on this box is larger than that. Both servers sit
under 2 of 18 cores while topping the raw-rps column, which is the signature of
a generator-bound measurement, not a fast server. The correct statement is that
this harness cannot separate them — not that Forge approaches C.

**Forge's newest serving mode is its worst.** `forge-uring` burns 3.69 cores to
`forge-mt`'s 1.91 and returns 16% *fewer* requests for it — 25,111 per core
against 48,211, a 48% regression on the metric. `forge-hybrid` sits between
them on the same pattern. io_uring and the hybrid scheduler are the most
recently added and most intricate parts of the HTTP layer (`2fd4d94`,
`d2f1f41`), and on this workload both are strictly worse than the plain
multi-threaded path they were added alongside. That is not an argument against
io_uring, which wins on workloads this benchmark does not contain; it is an
argument that three serving modes ship with no evidence distinguishing them,
and a user picking one is guessing.

**`rust-axum`'s tail is four seconds.** It has the best p50 and p99 in the
table by a wide margin — 1.6 ms and 11.6 ms — and a `max_ms` of 4,089. A
latency profile that good through the 99th percentile and that bad at the top
is a stall, not load: something blocks for seconds while the steady state stays
excellent. It reproduced in all three runs. Note also that axum is the one
server that would not conform to the wire spec, answering 148 bytes and four
headers, so it is doing more work per response than everything else here.

**Erlang pays 8 cores for 51k rps, and that is the whole argument of this
document.** `erlang-beam` has the second-worst throughput per core in the
table, using more than four times Forge's cores for half its requests. That is
what a private heap and mailbox per connection cost. It is also what buys the
supervision Forge's README describes and does not have. The two facts are the
same fact.

---

## Concurrency: the one place Forge is decisively better

This is the measurement that matters most, because cheap concurrency is
Forge's entire thesis.

### The comparison had to be rebuilt before it meant anything

The pre-existing sweep compared Forge against `bench_go.go`, which spawns N
goroutines that begin retiring while the loop is still spawning. Peak-alive
never approaches N. Forge has no choice but to hold all N alive — a
`process main` body runs to completion before `fr_scheduler_run()` drains
anything — so the two programs were measuring different situations, and the
comparison flattered Go by more than two orders of magnitude.

I wrote hold-live variants for Go and Erlang
(`benchmark/concurrency/src/bench_go_live.go`, `bench_erl_live.erl`) that park
every task, confirm all N are alive, sample memory at that instant, and only
then release them. Numbers below are medians of three runs at N = 1,000,000.

| Runtime | Time to 1M live + drain | Memory | Per live task |
| --- | --- | --- | --- |
| **Forge** | **1,056 ms** | **176 MB** | **180 B** |
| Go 1.26.4 | 2,873 ms | 2,677 MB | 2,741 B |
| Erlang/OTP 29 | 5,020 ms | 2,644 MB (processes) | 2,708 B |

Forge is 15× leaner than both and 2.7–4.8× faster. I re-derived this twice
because the result was large enough to look like a bug, and it is not: the
mechanism is visible in the source.

### Why, and what it costs

`runtime/scheduler.c:751` allocates a coroutine with a single
`calloc(1, sizeof(fr_coro_t))`. There is no stack. `struct fr_coro` carries
`state`, `state_size` and `step` — the coroutine is a state machine, and
`compiler/codegen_coro.c` emits the switch that drives it. 180 bytes is the
struct plus its allocator overhead, and it does not grow with call depth
because there is no call depth to grow.

Go pays ~2.7 KB for a real, growable stack. Erlang pays ~2.7 KB for a private
heap and a mailbox. Both are buying the same thing: a task that can be reasoned
about, and killed, independently of every other task.

Forge buys none of that, and the consequences are not theoretical:

- A coroutine cannot be given its own heap, so it cannot be torn down
  independently. `FR_CORO_ERROR` exists in the enum and is compared against in
  three places (`scheduler.c:178,240,363`); **nothing ever assigns it**. A
  coroutine has no way to enter an error state.
- `restart_policy` is assigned once at `scheduler.c:879` and never read again.
  There is no signal handling anywhere in `runtime/`, `stdlib/` or `include/`.
- Supervisors parse, and `compiler/codegen.c:283-293` faithfully emits
  `fr_supervisor_create` and `fr_supervisor_add_child`. Nothing restarts
  anything. A real crash takes the OS process down with it.
- `README.md:99` promises "Elixir-style fault-recovery policies";
  `docs/first.md:28,207` and `docs/second.md:307,722` promise 장애 복구 and
  내결함성. None of it is implemented.

The stackless design is a good and defensible choice — it is why the numbers
above are what they are. Presenting an Erlang supervision story on top of it is
not.

Two honest footnotes. Erlang's figure is `erlang:memory(processes)`, what the
processes themselves cost; the emulator's RSS including preallocated carriers is
~3.0 GB, and either basis gives the same ratio. And the workload is spawn, park,
wake, exit — it does not exercise message passing, per-task heap growth, or
preemption under load, all of which BEAM is built for and Forge is not.

---

## What the earlier review found, re-checked

These were reproduced independently, because two of them change how the
performance numbers should be read.

### `-O2` turns a crash into silent garbage

`compiler/driver.c:73` hardcodes `-O2`. Integer division by zero at `-O0`
raises SIGFPE. At `-O2` the same program prints an ASLR-varying poison value —
94523918147728, 93857590513808, 93882198683792, 94427590480016 across runs —
and **exits 0**.

Forge defines no semantics for division by zero, so it inherits clang's
undefined-behaviour exploitation, and the driver hardcodes the optimisation
level at which the failure is silent and the exit code is a lie. For a language
with no error handling, this is the worst available default.

### The ownership checker misses the case that matters

Read-after-move is correctly rejected (`forge: use of moved value 'a'`).
**Move-after-move compiles clean** and silently yields an empty value. Every
`move` also emits clang qualifier-discarding warnings out of `fr_own_take`
(`include/forge/ownership.h:14`).

### Diagnostics point at a file that has been deleted

Forge performs very little semantic analysis of its own. Type errors, undefined
symbols and arity mismatches are all caught by clang, and clang's raw output
reaches the user citing C types, C line numbers, and a temp file path that the
driver has already unlinked. No `.fg` line number appears anywhere. Arity errors
are actively misleading — a two-parameter coroutine reports "expected 3, have 2"
because of an injected `fr_process_t *proc`.

This is the single biggest day-one obstacle for anyone trying the language.

### Also confirmed

`match` is not exhaustiveness-checked and falls through silently with exit 0. A
failed `fs_read` is indistinguishable from an empty file. `ptr` is unusable
because no dereference operator exists — the parser's only `TOK_STAR` mapping is
`BIN_MUL`, so `*p` is a parse error. And `println("a", b)` compiles to one
write-and-flush per argument, so two coroutines interleave inside a single line:
the first concurrent program a newcomer writes produces corrupted output.

---

## Startup and toolchain

Cold start, median and p99 over 60 runs:

| Language | Median | p99 |
| --- | --- | --- |
| C | 1.5 ms | 2.3 ms |
| Rust | 2.0 ms | 2.9 ms |
| Go | 2.5 ms | 3.6 ms |
| **Forge** | **5.4 ms** | **7.8 ms** |
| Python | 23.2 ms | 33.5 ms |
| Node | 36.5 ms | 45.3 ms |
| Java | 43.6 ms | 50.7 ms |

Forge is in the compiled-language band, which is where it belongs, but it is
3.6× C for a reason that is pure accident: a 49 KiB hello-world transitively
links `libOpenCL`, `liburing`, `libssl`, `libcrypto`, `libz`, `libbrotli*` and
`libzstd` through `libforge_runtime.a` and `libforge_std.a` whether or not the
program uses any of them. That is a build-system fix, not a design problem.

Compile time is ~3.8 ms in Forge's own frontend, flat, and ~113 ms in the clang
stage it delegates to. Emitting C and handing it to clang remains the best
decision in the project — a mature optimiser and every host target, for a
fraction of the cost of a native backend. The 254 MiB clang dependency behind a
320 KiB compiler is the honest price.

---

## Would I bet a project on it

No, and the numbers sharpen rather than soften the reason.

Before measuring, Forge looked like an unfinished language attached to a decent
runtime, and the rational advice was to wait. After measuring, the runtime is
not merely decent — the concurrency primitive is better on its own terms than
the two production runtimes it is competing with, by a margin large enough to be
worth building on.

That makes the gap more frustrating, not less. What stands between this and
usable is not performance work. It is:

1. **Decide what failure looks like.** Still the blocker. It changes every
   standard-library signature and every call site written before it lands.
   Nothing else on this list matters until it is settled.
2. **Stop shipping `-O2` with no defined UB semantics.** Either define division
   by zero and the other cases, or stop hardcoding the optimisation level that
   makes them silent. A wrong answer with exit 0 is worse than a crash.
3. **Map clang diagnostics back to `.fg` source, and stop deleting the temp
   file when a diagnostic references it.** Cheap, and the difference between a
   language someone can try and one they cannot.
4. **Either implement supervision or remove it from the documentation.** The
   current state — full syntax, real codegen, no runtime behaviour, four
   documents promising fault tolerance — is the one option worse than either.
5. **Close move-after-move**, and make one `println` produce one line.
6. **Build a test suite.** The previous review's evidence stands: a leaking
   allocator, universally misaligned pointers and eight data races survived a
   178-finding audit, and targeted harnesses found all three in twenty minutes.
7. **Cut a serving mode, or benchmark the three that exist.** `forge-hybrid`
   and `forge-uring` are both worse per core than the plain multi-threaded
   path, and `forge-uring` is worse by 48%. Three modes with no measurement
   distinguishing them is three times the surface to keep correct for no
   established gain.

Items 2 through 5 are days of work. Item 1 is a language design decision, and
item 6 is the thing that would have prevented most of this document. Item 7 is
the one this document created.

The runtime is the hard part of a language like this, and Forge has largely
built it. The unfinished half is the cheaper half, which is either encouraging
or damning depending on whether anyone finishes it.

---

## Reproducing

```sh
# Cross-language HTTP suite (builds nothing; expects the servers prebuilt)
REQUESTS=300000 CONCURRENCY=400 SUITE_RUNS=3 benchmark/run_crosslang_benchmark.sh
# One server only:
benchmark/run_crosslang_benchmark.sh forge-mt

# Concurrency, hold-live variants
cd benchmark/concurrency
go build -o bin/bench_go_live src/bench_go_live.go && ./bin/bench_go_live 1000000
erlc -o bin src/bench_erl_live.erl
erl +P 2000000 -noshell -pa bin -run bench_erl_live main 1000000
```

`benchmark/.tool-versions` pins `erlang 29.0.3`, scoped to that directory so the
Forge build itself never depends on an Erlang toolchain. Note that `erl` behind
an asdf/mise shim **exits 0 while printing "No version is set"**, so exit status
is not a valid availability check.

Raw data: `benchmark/crosslang_results.{txt,jsonl}`,
`benchmark/concurrency/results/{raw,go_live,erlang_live}.tsv`.
