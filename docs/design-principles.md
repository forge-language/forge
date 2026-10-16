# Forge design principles

**Safe ownership. Massive concurrency. Native speed.**

These goals describe one design: explicit ownership makes resource costs visible,
lightweight processes make independent work practical, and native compilation
keeps that work efficient. Performance changes must preserve the program's
meaning and the ability of other processes to make progress.

Forge is experimental. Full ownership, alias and lifetime checking remain work
to do. These principles guide implementation and review; they do not certify
memory safety or establish performance for unmeasured programs.

## Preserve observable behavior

An optimization needs a reason that the replacement computes the same result,
including failure. Respect the [integer contract](integer-arithmetic.md), operand
evaluation counts, short-circuiting, side effects and live values. A mathematically
equivalent formula can still be wrong when Forge multiplication wraps. An empty
loop can still have a trapping initializer. Allocation failure and rejected sends
are part of resource behavior, not exceptions to correctness.

Construct a resource owner completely before publishing it. A non-NULL factory
result must have every required queue lock, native primitive and event loop.
Failure releases the resources acquired so far and preserves any existing global
owner or provider. Check these conditions at construction, keeping dispatch free
of repeated validity checks. Generated callers must check factory results before
transferring ownership or starting work. Borrowed child-list growth commits its
pointer and count together; failure preserves the previous list.

When equivalence depends on input bounds, check those bounds before entering the
specialized path. Retain the original checked implementation for other inputs
and unsupported targets. Decline a transformation when its proof is incomplete.
Keep compiler proof scans and synthetic-name searches bounded as well: compilation
time and memory are costs users experience.

Derived compiler data belongs to the pass that uses it. Build lookup indexes
after declarations are loaded, borrow names only while their storage is valid,
and release the indexes before that storage changes or the request ends. Preserve
declaration order and namespace precedence when accelerating lookup. Document
the remaining scans and allocation costs; a faster index does not make every
part of compilation logarithmic or establish a new type rule.

Charge setup to work actually performed. A declaration or imported module alone
does not justify building a cache. Bound deferred setup explicitly, and measure
unqueried owners, sparse queries and early hits alongside expensive searches.

Name indexes should lead back to the current binding. Read initialization and
ownership state from that binding, and pair index updates with every scope change.
Use stable positions across owner-array reallocations, check layout assumptions
at compile time, and fail explicitly when an internal index contradicts its owner.
Reusing scratch capacity must respect the lifetime of keys from earlier scopes.

A stable declaration does not imply a context-independent analysis result.
Cache its identity separately from inferred types or expression values. Preserve
when analysis observes live scope and flow state; memoizing those results needs
its own context key and equivalence argument. A lookup optimization must not
silently become a language-semantics change.

For example, binary modular exponentiation can use reciprocal multiplication
when every intermediate product fits signed int64. A power-of-two divisor also
permits masking, provided the reduction preserves the sign of each wrapped
product. Other inputs retain the original checked operations. The compiler recognizes bindings and operations;
function names, benchmark names and expected output values are not proof.

## Make ownership and resource costs explicit

Moving a value should have a specified owner before and after the operation,
including failure paths. A copy, arena lifetime, mailbox transfer and borrowed
alias have different costs and obligations; document the actual boundary.
Stage0 currently copies an owned string into a mailbox payload on send. This is
not evidence of zero-copy ownership or complete lifetime checking.

Remove redundant allocation or copying only when the affected resource is private
and its lifetime is established. A fast path must not lose cleanup or expose a
partly constructed object on failure. Keep runtime and standard-library ABI
changes in their owning repositories and pin tested dependency revisions.

Starting workers is an ownership transition too. Keep a listening socket and its
queued connections alive until a worker owns them. Allocation, thread or listener
creation failure must leave a usable resource for the fallback. Test connections
queued before startup and new connections after partial failure; a successful
steady-state benchmark does not establish reliable startup.
Defer shared access until a receiving worker exists, so an early fallback can
retain the resource's original access rules as well as a valid handle.

Stopping execution and destroying its owner are separate operations. Retained
work must either be resumed or have its owned storage reclaimed exactly once at
terminal destruction. Cleanup must not invoke user work as a side effect, free
borrowed arguments, or revisit arguments already transferred out of a queue.
Release batch waiters only after their published work has completed or been
discarded, and keep publication quiescent while destroying its owner.

## Concurrency includes progress

A throughput improvement must preserve the scheduler's synchronization and
cooperative yield boundaries. Faster code between yields does not authorize
removing a yield or holding a shared lock longer. Review cancellation, mailbox
lifetime, idle wakeups and progress of competing work when those paths change.

Define what a count observes. Queued, claimed and completed work are distinct
states. Update accounting in the same critical section as ownership transfer.
A deferred ledger must include work already in flight when observation starts,
and retain the existing backlog across stop/restart. A snapshot is not a capacity
reservation across separate callers or API calls.

Use ownership invariants before duplicating state. A private queue's empty state
can be inspected while an enclosing owner mutex excludes every mutator; that
does not make a shared standalone queue safe to inspect without synchronization.
Keep the observation and claim in the same protected interval, and preserve
selection order and callback boundaries. A new readiness counter carries update,
layout and failure-path costs even if its getter is cheap. Measure those costs
against coroutine and mixed-work controls as well as native-only work.

An established owner guard can also make a nested private mutex redundant, but
only when every publication, claim, count and cache access obeys that same guard.
Keep standalone queues synchronized and initialization/destruction explicit.
Share structural queue logic so error and cleanup behavior cannot drift between
the two access policies; separate linkage can keep unused entry points out of
standalone consumers. Prove the guard with actual lock events and a failing
missing-guard control. Fewer locks do not establish better tail latency: preserve
adverse p99 observations beside throughput gains.

Failure can satisfy a completion predicate too. If a rejected publication retires
the last active task, wake its completion waiter under the same owner mutex.
Do not rely on a worker eventually issuing that wake when no worker received
the task. Mailbox acceptance, dispatch success and callback completion are
separate events with separate ownership obligations.

Serialize lifecycle publication too: an old stop must clear its registration
before a new start publishes the same object. State which caller owns the
object's lifetime and may wait for its workers; a mutex cannot make an expired
borrow valid.

CPU availability has an owner and an observation time too. Distinguish online
hardware, a calling thread's allowed CPU mask, and bandwidth quotas. Automatic
pool sizing should use available CPUs without silently replacing inherited
placement policy; explicit pinning is a separate caller decision. Sparse CPU
IDs are not ordinals. Keep hardware-observation APIs distinct from sizing APIs,
bound large-mask probing, and state when a fallback may overprovision. Verify
actual thread masks in measurements rather than trusting a helper's name.
Preserve older dependency behavior through explicit capability checks when a
new optional runtime facility is used across repositories.

Measure fairness, errors and tail latency alongside completed work. Distinguish
spawned processes, open connections, in-flight requests and application users.
A finite connection cohort does not establish sustained service capacity, and a
cached response workload does not establish application performance with a
database, TLS or routing.

## Earn speed with reproducible evidence

Use the same source workload, inputs, toolchain flags and dependency revisions
for a comparison. Check outputs independently before accepting timings. Alternate
baseline and candidate runs, record the environment, retain raw samples and
report variation. Include guard misses and existing fast paths when adding a
specialization; extra code and dispatch have costs too.

Measure the compiled program produced by the compiler being proposed. Build
dependencies must invalidate downstream artifacts when their compiler changes;
passing a check against stale output is not evidence for the current source. Label
experiments that patch generated C as prototypes. Keep discarded approaches and
their evidence available, especially when a proxy such as fewer syscalls improves
while the actual workload slows down. A speedup belongs to the measured workload
and environment; it is not a language-wide guarantee.

Prefer reusable transformations with small, reviewable proofs. If an optimization
adds complexity without a measured benefit, improve or remove it. Native speed
includes keeping ordinary programs and the compiler practical to understand.

## AI work stays inspectable

**AI builds. AI tests. AI reviews. Humans decide.**

Use independent agent review to challenge arithmetic proofs, binding assumptions,
failure paths and measurement methodology. Preserve the actual requests and
completed findings. A launched task, routing failure or partial response is not
a completed review. Resolve findings against source and executable evidence;
agreement among agents does not replace verification.

Describe what changed, what was measured and what remains unverified. Human
maintainers retain release and merge decisions under the [RFC process](../RFC_PROCESS.md).

## Evidence for an optimization review

- State the recognized operation, its preconditions and the equivalence argument.
- Exercise accepted inputs, guard boundaries, failure paths and rejected shapes
  against an independent oracle. Cover applicable backends and public entry points.
- Provide reproducible before/after commands, source and raw measurements with
  the scope and limitations of the performance claim.
- Run relevant regressions and self-hosting checks; record any checks that could
  not be performed instead of treating them as passes.

See the [architecture](../ARCHITECTURE.md), [roadmap](../ROADMAP.md) and
[historical optimization experiments](optimization-rules.md).

The [HTTP startup study](../../forge-benchmarks/docs/compiler/http-startup-2026-10-15.md) exercises queued
connections, partial worker failures and the boundary between a tested local
dependency candidate and a published revision.

The [constant lookup study](../../forge-benchmarks/docs/compiler/constant-lookup-2026-10-15.md) separates
declaration indexing from contextual expression checking and measures deferred
setup against unqueried, sparse and early-hit owners.

The [native depth study](../../forge-benchmarks/docs/compiler/native-depth-2026-10-15.md) checks deferred
accounting against protected queue snapshots, in-flight callbacks and retained
backlog, with no-query controls and per-job latency measurements.

The [lifecycle study](../../forge-benchmarks/docs/compiler/scheduler-lifecycle-2026-10-15.md) exercises
overlapping start/stop calls and terminal cleanup of runtime-owned native work.

The [CPU affinity study](../../forge-benchmarks/docs/compiler/cpu-affinity-2026-10-15.md) separates online
CPU observations, allowed-mask pool sizing and explicit placement, retaining
throughput and queue-latency tradeoffs under restricted CPU masks.

The [dispatch study](../../forge-benchmarks/docs/compiler/scheduler-dispatch-2026-10-15.md) compares extra
readiness accounting with protected private queue observations and checks
completion signaling when publication of the last task fails.

The [factory failure study](../../forge-benchmarks/docs/compiler/factory-failure-2026-10-16.md) injects
construction failures at allocator, native primitive and event-loop boundaries,
checks generated caller cleanup, and retains normal dispatch controls.

The [queue ownership study](../../forge-benchmarks/docs/compiler/queue-owner-2026-10-16.md)
checks externally serialized queue access, byte-identical standalone linkage,
paired dispatch gains and the observed blocked-work tail-latency costs.
