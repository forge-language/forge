# Ownership, loans, and resource lifetimes

Status: **Draft / Proposed**, 2026-10-17. Owner: Forge maintainers (decision owner unassigned); drafting agent: ownership contract lane. Human decision: **pending** under [RFC_PROCESS.md](../../RFC_PROCESS.md). This document does not accept or implement plan tasks 6–9 or 11–14. No issue was posted by this drafting task.

## Problem and recommendation

Forge's current owned-string check is a binding-consumption check, not a general allocation ownership or lifetime system. Preserve the existing copying string-send boundary, consumption on rejected delivery, unsupported general `move`, and legacy raw C ABI. Add internal resource/loan/effect metadata only after maintainer approval of the rules below. Do not infer exclusivity, `restrict`, zero-copy transfer, or complete safety from `own let`.

`LANGUAGE_SPEC.md` currently describes owned heap strings and general move too broadly. `compiler/semantic.c:expression` rejects `EXPR_MOVE`; `statement:STMT_SEND` accepts send-move only for an initialized, named `own` string and clears that binding's initialized state. Ordinary send accepts integer/bool/opaque values rather than strings. `compiler/codegen.c:emit_owned_send_helper` emits `__forge_send_owned_string`: clone bytes with `fr_own_clone`, abort on clone failure for a non-null source, clear the source binding, then call `fr_send_move`. An `own` literal or arena string is therefore not proof of heap allocation. Existing aliases remain valid until their original backing region expires.

## Current resource transitions (native stage0)

These are source-grounded implementation facts, not new safety guarantees. Runtime and stdlib paths refer to their separate repositories, whose identities are recorded in the accompanying evidence. JS and the self-hosted subset need separate conformance evidence; native pointer, allocator, and thread claims do not transfer to them.

| Resource | Allocation and borrowed region | Cleanup owner | Send accepted / rejected | Alias validity |
| --- | --- | --- | --- | --- |
| Literal string | Generated C literal, static storage | No per-binding free | Through an `own` binding: clone to heap; consume binding on either result | Original literal aliases stay valid |
| Arena string | `string.c:str_alloc` → `fr_arena_alloc(fr_arena_tls(), ...)` | Coroutine arena or fallback thread arena | Send clones bytes; original allocation stays with arena on either result | Valid until owning arena reset/destroy; scope alone does not release bytes |
| `own let` string | Qualifier on a string binding; initializer may be literal, arena, or external | Determined by backing allocation, not the qualifier | Binding becomes moved; fresh heap payload is consumed by runtime even on rejection | Other aliases are unchanged by this copying boundary; external storage still needs its own lifetime contract |
| View | `fr_str_view` allocates descriptor in current arena; borrows source bytes | Arena owns descriptor; source owner owns bytes | No supported owned-view send; integer ABI representation is not a checked transfer capability | Both descriptor arena and source must remain live; immutable source contract |
| Builder / snapshot | Descriptor and growth buffers in arena; `finish` copies; `finish_take` detaches buffer and empties builder | Same arena for builder and both snapshot forms | Snapshot string may be wrapped in `own` and copied on send; no checked builder transfer | `finish` snapshot independent of later mutation; `finish_take` snapshot independent of later contents; both expire at arena reset |
| Mailbox payload | `fr_own_clone` uses malloc; C callers must supply compatible heap payload | Queue after successful `fr_send`; receiver after pop; runtime frees on rejection | `fr_send` returns 1 on enqueue, 0 on null destination/full mailbox; consumes payload in both cases | Payload aliases cannot be used after consumption; independent original Forge string aliases can |
| Resource handle | API-specific factory (e.g. process, mutex, file); integer/pointer ABI is a representation | API-specific caller or scheduler registration; not inferred from integer | No generic checked transfer; copying numeric bits does not transfer cleanup responsibility | Live, correct-kind, non-forged handle and API synchronization are caller obligations today |

The runtime `ownership.c:fr_own_take` separately clears a `char **` without copying. Its existence does not make general Forge move supported. `fr_send_move` returns void and discards `fr_send`'s delivery result. After `fr_try_recv`/`fr_recv` removes a message, the recipient must release its owned payload, normally through `fr_msg_free_payload`; process destruction frees only messages still queued. Passing stack, literal, arena, already-freed, or incompatible allocator storage directly as a non-null `fr_send` payload violates the C boundary contract.

| Event | Current lifetime/cleanup effect | Proposed checked rule (not implemented) |
| --- | --- | --- |
| Scope exit / early return | No general destructor insertion for arbitrary `own` or handle locals | Release supported live owned resources exactly once in reverse initialization order; exclude borrowed, moved, released, and uninitialized slots |
| Branch join / loop back edge | `semantic.c:snapshot`, `restore`, `flow_block`, `statement` conservatively track initialized bindings | Binding-ID CFG join must preserve any reachable moved/released path; no read/resend unless reinitialized; early exits and repeated iterations need explicit cases |
| Assignment / shadowing | Reassignment can initialize consumed binding; nested bindings are distinct | Cleanup old owned resource before overwrite only under approved resource effect; distinguish field partial moves (unsupported) and shadowed bindings |
| Yield / await | Coroutine state survives suspension; no general loan/worker-eligibility proof | Preserve loans only if owner survives, address stays stable, and migration/shared-access constraints hold |
| Function return | String pointer return has no general region proof | Returned loan must name a surviving argument/owner region; reject escape from local owner |
| Arena reset | `fr_arena_reset` rewinds block positions; storage can be reused without immediate free | Reject reset if a loan will be used afterward; last-use ending a loan permits reset, but does not itself free owner |
| Arena destroy / explicit free | Destroys backing allocation; aliases become invalid | Invalidate dependent regions/loans and reject later access, including view descriptors |
| Process/scheduler teardown | `fr_process_destroy` frees coroutine states, arenas, queued owned payloads; `fr_scheduler_destroy` stops then destroys registered processes | Define cancellation joins and exactly-once cleanup for supported resources before advertising cancellation safety |
| Allocation/publication failure | String clone failure aborts before source clearing; rejected `fr_send` frees payload; factory paths have explicit partial cleanup | Sentinel/failure must not become a live owner; document each allocation/publication transition and error-use obligation |
| Abort / forced termination | No language cleanup/unwind guarantee; generated clone failure calls `abort()` | Retain no-cleanup guarantee; do not promise destructors or unwinding |

Reset is a semantic invalidation even when ASan sees still-allocated memory. Normal cleanup, leak freedom, memory safety, and abort behavior are separate claims.

## Proposed checked model and boundaries

Represent resource identity separately from scalar bits, with kind, allocator/deallocator pair, owner region/generation, initialization, and states Owned, Borrowed, Moved, Released. A source binding consumed by copying send is Moved while its underlying arena allocation remains live; these must be separate facts. Unknown binary APIs remain opaque legacy boundaries. A signature alone proves neither lifetime nor safe callback retention.

Use shared loans for simultaneous immutable reads and an exclusive loan for mutation. Reject overlapping exclusive/shared access to the same place. Reborrow must be bounded by its parent loan; suspend conflicting parent access until the child loan ends. End loans at last reachable use, retain owner cleanup until its own release point, and retain loan conflicts through branches, loops, and suspension. Function summaries must tie returned borrows and retained callback contexts to input/owner regions. A view has two dependencies: descriptor region and source-byte region. A builder snapshot has an independent byte allocation from future mutations but still belongs to its arena. Unknown FFI aliases cannot silently enter the checked region model.

Transfer-safe means an owned value may cross workers; share-safe means references can be accessed concurrently under the resource's synchronization rules. Neither implies the other. `scheduler.c:worker_main` steals coroutine/native work; `arena.c:fr_arena_tls` first consults a coroutine provider, then uses thread-local fallback storage. Stable arena blocks alone prove neither thread safety nor owner survival. Thread-affine OS handles, fallback thread arena loans, unsynchronized mutable builder aliases, and retained callback contexts need explicit restrictions. A live borrow across yield is acceptable only after those conditions are proven; blanket permission or blanket prohibition is not the proposal. Self-referential coroutine state needs stable address evidence or rejection.

An effect manifest beside SDK signatures should specify allocate/borrow/release/consume/reset, success and failure ownership, resource kind, nullability/sentinels, thread affinity, callback argument/return/ABI, retention lifetime, and must-use result requirements. A future checked facade must reject forged, wrong-kind, stale handles and mismatched callbacks while preserving raw ABI adapters. Bounds checks on valid handles do not validate arbitrary integers or stale descriptors. Checked string/view indexing must preserve existing failure values and overflow rules; raw pointer indexing stays outside its guarantee.

## Existing syntax examples

These are executable source forms already supported by stage0, not approval of new policy. Their runtime verification status is in the evidence ledger.

Copying send preserves the original arena alias even when destination zero rejects delivery:

```forge
import strings;
native main {
    own let text: string = str_concat("arena", " payload");
    let alias: string = text;
    send 0, Drop, move text;
    println(alias);
    return 0;
}
```

Expected native stdout: `arena payload` followed by newline, exit 0. Replacing `println(alias)` with `println(text)` must be rejected as use of moved owned value. A general `move(text)` expression is unsupported; no general move result type or ownership transfer is introduced here.

Builder snapshots remain unchanged after further append, before arena reset:

```forge
import strings;
native main {
    let builder: int = str_builder();
    if (builder == 0) { return 1; }
    if (str_builder_append(builder, "a") == 0) { return 2; }
    let first: string = str_builder_finish(builder);
    if (str_builder_append(builder, "b") == 0) { return 3; }
    println(first);
    println(str_builder_finish_take(builder));
    return 0;
}
```

Expected with successful allocations: stdout `a\nab\n`, exit 0. These legacy functions may return null on allocation failure; this example is not a checked nullable-owner demonstration.

Future accepted/rejected cases are specified as behavior, not invented executable syntax: permit view use followed by last-use reset; reject reset followed by view use; permit return of a borrow tied to a live argument; reject return of a local owner's borrow; permit synchronized shared access; reject concurrent mutable aliases; permit yield with proven surviving migration-safe owner; reject thread-affine loan migration. Compiler tests for these guarantees must fail before and pass after their later implementation.

## Decisions requiring human acceptance

| Decision | Recommendation | Alternative / unresolved detail |
| --- | --- | --- |
| Send cost and rejection | Preserve clone, clear binding, consume payload on rejection | Recoverable delivery status/retry requires a new API; do not return the freed payload |
| General move and new borrow syntax | Keep general move unsupported; begin with internal loan metadata | Explicit public borrow/reference spelling and lifetime annotation syntax remain undecided |
| Raw FFI / `unsafe` | Preserve legacy ABI with explicit excluded guarantees; additive checked facade | Keyword, warning level, migration period, and opaque-call restrictions require acceptance |
| Cleanup | Supported owner-specific reverse initialization cleanup proposal, no generic user destructors/unwind | Scope/overwrite/cancellation ordering and resource set need approval plus counters and fault injection |
| Failure-use | Manifest must-use effects; explicit handling of success/sentinel | Warning versus error, compatibility period, explicit discard spelling, and send-result exposure remain undecided |
| Transfer/share | Explicit resource capabilities and proof at spawn/send/suspension | Thread-affine cancellation executor, retained callback lifetime, and stable coroutine storage rules require runtime agreement |

Rust's shared/exclusive loans, transfer versus sharing, and drop-state separation are design references from the plan, not imported syntax, traits, ABI, or a proof of Forge safety. A whole Rust trait system, Pin, Result/Option, and panic unwinding are outside this draft. Reject zero-copy substitution because current source aliases may survive and arena storage cannot be freed as mailbox storage. Reject automatically freeing every `own` string because literals and arena allocations are valid initializers.

## Compatibility, cost, and acceptance evidence

Keep existing native ABI symbols and scalar handle representations at the legacy boundary. New checked metadata has compile-time cost; send retains O(n) byte copy plus heap allocation, views retain arena descriptor allocation, builder finish copies O(n), and finish_take detaches without copying a nonempty buffer. No performance improvement is claimed. Publish any future runtime/stdlib ABI change from its owning repository before updating immutable dependency pins.

Required acceptance evidence: accepted and rejected binding/branch/loop/shadow cases; successful receive with independent payload; null/full-mailbox rejection and exactly-once free counters; pending-message destruction; clone and factory allocation faults; reset/return/yield loan negative cases with safe controls; callback/forged-handle boundary tests; worker 1/2/4 runs; whole-runtime/stdlib ASan/UBSan and a separate supported TSan lane. Sanitizer success covers only executed inputs and schedules. Reset reuse needs a generation/semantic oracle, not ASan alone. JS and selfhost require explicit supported/excluded rows. The paired source/binary artifacts and unresolved gaps are recorded in `.omo/evidence/contracts-20261017/ownership-rfc.md`; no missing experiment is counted as passed.
