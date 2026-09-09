# Forge: refactor verification and language assessment

Reviewed 2026-09-09 on branch `http-serving-modes-and-runtime-hardening`.

Scope: independent verification of a five-way parallel refactor across the
compiler, runtime and standard library, followed by an assessment of the
language design. Complements `docs/code-review.md`, which covered the CLI and
onboarding only.

---

## Verdict

The refactor is sound and should be kept. Every claim the five working groups
made was independently reproduced, and two of them understated their own
result: the arena allocator was leaking and returning misaligned pointers, and
the scheduler had eight data races. All are now gone, and the compiler emits
byte-identical C for all 21 examples.

The language underneath is a different matter. Forge has no way to express
failure, no test suite, and a remotely reachable memory-exhaustion bug in its
TCP layer that has nothing to do with this refactor. It is a good runtime
attached to an unfinished language.

---

## What was verified

Two of the five groups hit rate limits before answering follow-ups, so
everything was rebuilt from scratch and re-derived rather than taken on trust.
A baseline worktree at the preceding commit provided the control.

| Check | Method | Result |
| --- | --- | --- |
| Clean release build | Fresh CMake configure, full compile | 0 errors, 2 pre-existing warnings |
| Example programs | Run each to completion | 16 / 16 exit 0 |
| Compiler output | Generated C diffed against baseline | 21 / 21 byte-identical |
| Runtime behaviour | Program output diffed against baseline | 13 / 15 identical |
| Arena allocator | Address + UB + Leak sanitizers | clean |
| Scheduler | ThreadSanitizer, 30 lifecycle cycles + 200 coroutines | 0 races |
| HTTP serving modes | 40 concurrent clients x 25 requests, per mode | 3000 / 3000 served |
| Slowloris defence | Partial header, never completed | dropped at 5.45 s |
| Test suite | CTest | 2 / 2 pass |

The two differing runtime outputs are a wall-clock timestamp and coroutine
interleaving order. Eight baseline runs produced the same variation, so neither
is a regression. The interleaving did expose a separate defect, recorded below.

HTTP was additionally checked live: GET, POST with body, and a 3 KB header
block all answered correctly across the multi-threaded, hybrid and io_uring
modes.

---

## What the refactor actually fixed

The work was scoped as cleanup over code a prior audit had declared correct.
It was not. Running the same targeted harness against both trees turns two of
the groups' claims into measurements.

| Defect | Baseline | After |
| --- | --- | --- |
| Arena leak, 200 reset-and-refill cycles | 41.4 MB leaked in 9,803 allocations | 0 |
| Arena alignment, 16 B and above | 8-byte aligned, silently wrong | correct through 256 B |
| Scheduler data races | 8 reported by ThreadSanitizer | 0 |
| Work-queue steal cost | walks victim list under its lock | constant time |
| Total C source | — | 877 lines removed net |

The alignment bug is the one worth pausing on. The allocator rounded up the
offset within a block rather than the address, and the block header is 24
bytes, so every request for 16-byte alignment or better received a pointer that
was merely 8-byte aligned. Any code placing a lock, an atomic, or a vector type
in arena memory was relying on undefined behaviour.

The eight races sit exactly where the runtime group said they applied fixes: an
unlocked append to the process coroutine list while other workers walked it,
unsynchronised reads of the run flag in the worker loop, the shutdown publish,
and the queue depth counter. The lost-wakeup fix in shutdown is the kind of bug
that produces an occasional unreproducible hang in production and nothing else.

Three of the five groups changed code that a documented, exhaustive audit had
already signed off as correct.

### Structural work

- **Compiler backend.** A 1,585-line code generator split into six translation
  units behind an internal header, with duplicated emission paths merged.
  Verified behaviour-preserving by byte-identical output.
- **Networking.** Four serving modes that each re-implemented the accept loop,
  header framing and connection setup now share one implementation. The
  duplication had already drifted: the TLS and routing paths still carried the
  small-buffer header bug fixed in the main path earlier, so a normal browser
  request over TLS was dropped without a response.
- **Runtime.** Two near-identical work queues unified onto one deque, with a
  bounded node free list so the steady-state path does not allocate.

---

## Findings this review adds

Defects in code the refactor did not touch. Each was reproduced before being
written down.

### CRITICAL — Any TCP client can exhaust server memory (fixed in this pass)

`fr_tcp_recv` (`stdlib/tcp.c`) read in a loop until the peer closed, doubling
its buffer with no ceiling. A client that connected and streamed without ever
closing drove allocation until the process died. No authentication, no special
payload, one socket.

Measured before the fix: a single client on one connection took the server from
3.9 MB to 1.29 GB resident in under ten seconds, still climbing when the test
was cut.

Fixed by capping the total read at 8 MB and failing the read past that, which
matches the function's existing NULL-on-error contract. After the fix the same
exploit leaves the server flat at 3.9 MB and resets the connection. A
legitimate 1 MB round-trip still echoes byte-exact, and all 15 example programs
still pass.

The read-until-close shape remains, and it is worth revisiting separately: it
makes request-response impossible on a persistent connection, because the
server cannot see a message until the client half-closes. The bundled
`tcp_echo` example only works with clients that shut down their write side.
That is an API design question, not a crash, so it was left alone here.

### HIGH — There is no test suite

The project ships two integration checks: a CLI smoke test and an
external-library link test. There are no unit tests for the compiler, the
runtime, or the standard library.

This is the root cause of everything above it. A leaking allocator, pointers
misaligned for every alignment a caller would actually request, and eight data
races all survived a documented review of 178 findings, because review is being
asked to do a job only tests can do. The harnesses written for this review
found all three in about twenty minutes.

### MEDIUM — Printing a line is not atomic

`println("worker start", id)` compiles to separate calls per argument, each
writing and flushing independently (`stdlib/io.c`). Two coroutines on different
worker threads interleave inside a single line.

Observed: `worker startworker start3worker start12|worker done2||worker done3|`

For a language whose headline feature is cheap concurrency, the first program a
new user writes produces corrupted output.

### MEDIUM — Full mailboxes drop messages silently

Process mailboxes are a fixed ring of 256 messages (`MAILBOX_CAP`). The 257th
is discarded and the sender is told nothing. The refactor correctly stopped
this leaking the payload, but the message is still lost.

Forge borrows its process and supervisor vocabulary from Erlang, where
mailboxes are unbounded and overload manifests as memory pressure a supervisor
can act on. Silent loss is a different contract, and not the one the syntax
advertises.

### MEDIUM — The toolchain cannot build an instrumented binary

The compiler drives the C compiler itself and passes no sanitizer flags
through, so a Forge program cannot be built under Address or Thread Sanitizer.
Producing the evidence in this review required linking harnesses against the
runtime archives by hand.

For a systems language with manual ownership and a threaded scheduler, this is
the diagnostic that matters most, and users have no route to it.

### LOW — Two loose ends

The compiler leaks a small allocation per coroutine parameter list
(`compiler/parser.c`, in `parse_params`), harmless for a short-lived process
but enough to make the compiler unrunnable under a leak checker without
suppressions. Separately, `web_server.fg` compiles but has no build target, so
it is never exercised.

---

## The language, judged on its own terms

Forge presents itself as lightweight processes and coroutines with supervisors,
ownership and pattern matching, compiled ahead of time to native code. The
runtime delivers on that. The language does not yet.

### There is no way to express failure

The keyword list is complete and contains nothing for errors. No exceptions, no
result or option type, no panic, no error type in the type system. Standard
library functions signal failure the way C does, by returning a sentinel, and
the caller has no construct for propagating it.

This is the central problem. A language organised around supervisors and
restart policies is a language about what to do when things go wrong, and it
cannot represent a thing going wrong. A supervisor can restart a process, but a
function cannot tell its caller that the file did not open. Every serious Forge
program will invent its own convention, and none of them will compose.

### The type system stops early

Integers, floats, booleans, strings, pointers, structs and enums, with no
generics. No reusable container can be written, so every program needing a list
of its own type either writes it again or drops to raw pointers. Strings are
bare C pointers, carrying no length, so the ownership annotations that guard
them cannot help with a value that truncates at the first zero byte.

### What is genuinely well judged

- **Emitting C and handing it to clang.** Unfashionable and correct. It buys a
  mature optimiser, every target the host compiler supports, and a readable
  intermediate form, at a stage where a native backend would consume the whole
  project.
- **The scheduler design.** Many-to-many threading, work stealing, and a
  reduction budget that preempts a coroutine after a fixed step count. That
  last detail is lifted from the Erlang virtual machine and is the right thing
  to copy: it is what stops one tight loop from starving everything else. Now
  verified race-free.
- **Ownership as annotation rather than proof system.** Move semantics without
  a borrow checker is a real position on the design space, not a half-built
  one. It asks less of the programmer than Rust and gives more than C.

### Tooling

The self-hosting compiler is 946 lines, which makes it a demonstration rather
than a second implementation. The language server is a thin TypeScript wrapper.
Documentation covers the happy path. None of this is unreasonable at this
stage, but it should not be read as a working ecosystem.

---

## Would I build on it

No, and the reason is not the bug list. Bugs get fixed, and the ones found here
were found quickly once a sanitizer was pointed at the right place.

The reason is that error handling is not a feature you add later. It changes
every function signature in the standard library and every call site in every
program written before it lands. Adopting Forge today means writing code
against a language that has to break itself to become viable. The same is true,
less severely, of generics and of strings that know their own length.

Recommended order of work. The first item was done in this pass; the rest are
open.

1. ~~Cap the TCP read.~~ Done. Giving it a real message boundary is still open.
2. Build a test suite, starting with the allocator and the scheduler. The
   evidence that this pays for itself is in the table above.
3. Decide what failure looks like in the language, before the standard library
   grows further.
4. Make one print statement produce one line.
5. Pass sanitizer flags through the driver so users can find their own bugs.

The runtime is the hard part of a language like this, and it is in better shape
now than the audit trail suggested it was before. The gap is in the language,
and it is a design decision, not an engineering backlog.

---

## Reproducing this review

```sh
# Baseline control
git worktree add /tmp/forge-baseline HEAD
cmake -S /tmp/forge-baseline -B /tmp/forge-baseline/b -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/forge-baseline/b -j"$(nproc)"

# Refactored tree
cmake -S . -B build-verify -DCMAKE_BUILD_TYPE=Release
cmake --build build-verify -j"$(nproc)"

# Sanitizer trees (arena lives in forge_std, scheduler in forge_runtime)
cmake -S . -B build-asan2 -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=thread -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
```

Generated-C equivalence is checked with `forge <example>.fg --emit-c -o <out>.c`
against both compilers and `cmp`. Note that the example binaries themselves
cannot be built under a sanitizer; harnesses must link the runtime archives
directly, which is finding five above.
