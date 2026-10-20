# Logical execution and continuation trace substrate

This layer extends the native trace model below source-level throw, raise,
and panic rendering. It is deliberately independent of carrier threads and
actor heap addresses.

## Identity model

Four identities must remain separate:

1. **ExecutionId** — one logical guest execution/turn. A resume receives a new ExecutionId.
2. **ExecutionHandle** — generation-checked handle into ExecutionContextStore.
3. **ContinuationHandle** — generation-checked handle into ContinuationStore.
4. **AllocationDomainRef** — actor/task/runtime allocation authority. For actor
   heaps this is {ACTOR, runtime-domain, actor-slot, actor-generation} and is
   stable across many ExecutionIds.

Actor slot zero is valid. An actor heap capability is fenced by runtime domain
and actor generation, not by treating a non-zero actor number as authority.

No persisted trace record contains a pthread id, native frame address, actor
heap pointer, exception pointer, or guest value.

## Storage

Both logical stores use fixed-capacity eight-lane AoSoA blocks. Public handles
are {u32 slot, u32 generation} with slot zero and generation zero invalid.
Generation overflow permanently retires the slot instead of wrapping, so stale
handles cannot regain authority through ABA.

The slot is **not** a per-store array index. Execution and continuation stores
reserve disjoint ranges of positive 32-bit slot identifiers through one atomic
process-wide allocator. Each store translates its reserved global slot back to
a local AoSoA lane only after checking the slot belongs to that store.
Ranges are never recycled, including after closing and reopening a store.
Consequently a handle from a different store or an earlier instance cannot
validate solely because its generation matches. This preserves the existing
8-byte handle and 64-byte execution context ABI without a pointer or side
table in persisted records.

The shared slot namespace is intentionally finite: at most 2^32-1 slots
can be reserved over the **entire process lifetime**, including closed stores.
A later open fails closed if reserving its full requested capacity would wrap
or exhaust that namespace. Callers should therefore use long-lived bounded
stores, not create a new store for each execution or actor.

Execution records retain their parent execution while live. Continuation
records retain their originating execution and their parent continuation.
Release walks ancestry iteratively rather than recursively.

OresNativeExecutionContext remains exactly 64 bytes and pointer-free.
OresNativeSuspensionLink is exactly 32 bytes and persists:

    { ExecutionId, ContinuationHandle, parent ExecutionHandle }

The suspended execution becomes parent_execution of the resumed execution.

## Suspension and resume

ores_native_await_trace_suspend is the compiler/runtime hand-off at a
suspension point. It publishes a continuation, binds that weak continuation
handle into the current execution record, and returns a pointer-free
OresNativeSuspensionLink.

ores_native_await_trace_resume_begin receives a fresh scheduler-minted
ExecutionId. It inherits the allocation-domain capability from the continuation,
creates a new execution record whose parent is the suspended execution, and
allows exactly one claimant to resume the continuation.

The first compiler integration now uses this ABI for exactly one constrained
source form in trusted zero-argument main:

    await actor_ref.get_ready();

The compiler splits post-await sends plus a side-effect-free scalar result into
a root-actor CONTROL continuation. A first root CONTROL turn persists the
scheduler-minted ExecutionId and OresNativeSuspensionLink, then returns so its
native worker stack is destroyed. Actor readiness completes a one-shot native
Future which admits a second root CONTROL turn. That turn has a fresh
ExecutionId, calls resume_begin, verifies it inherited the same root actor
allocation-domain capability, exposes the current ExecutionHandle to diagnostics,
executes the generated continuation, and calls resume_finish.

The trusted outer main waits outside all VM carriers and performs source stop
only after the continuation returns. This first profile rejects multiple awaits,
multiple actor refs, suspended scalar/class/array locals, callable final results,
actor-context await, generic Future<T>, select, cancellation and borrow-across-
suspend. Those remain fail-closed until typed MIR, cleanup/unwind state and
general continuation frames exist.

## Trace reconstruction

At a failure site the public renderer may compose:

    currently available native/symbolized frames
        +
    continuation ancestry
        +
    parent execution ancestry
        +
    actor / supervisor causal boundaries

ores_native_logical_trace_snapshot reconstructs the logical portions into
caller-owned bounded buffers and performs no allocation. Actor-message and
supervisor-CONTROL boundaries are explicit continuation kinds rather than fake
native stack frames.

The runtime never keeps native stack pointers across suspension and never copies
an entire native stack at each await. Native frames are observed only while the
current native failure stack is alive; older causal structure comes from the
logical stores.

## Admission / retirement invariants

- A store cannot close while logical records remain live.
- Continuation publication pins its originating execution.
- Child execution publication pins its parent execution.
- A continuation may be claimed for resume exactly once.
- Allocation authority is inherited from the suspended continuation, never
  synthesized from the fresh resumed ExecutionId.
- Parent execution links stay within one runtime domain. Cross-actor causality
  is represented as an explicit boundary segment instead of fabricated callers.
- Stale generation handles fail closed.
- Trace snapshot truncation is explicit and bounded by caller-provided buffers.

The host regression crosses an AoSoA block boundary and suspends through
actor-message, await, and supervisor-CONTROL boundaries. Each resumption runs
on a fresh pthread after the suspending C stack has returned, then reconstructs
the full logical ancestry without any saved stack address.

## Failure rendering hand-off

While a compiler-generated root continuation is executing,
`oreslang_source_actor_current_execution_handle` returns the live
generation-checked ExecutionHandle and
`oreslang_source_actor_snapshot_current_trace` snapshots its logical ancestry.

`NativeFailureTrace` adds the allocation-free failure-side layer. Capture
takes an authoritative current ExecutionHandle plus the live stores, pins that
current execution while the multi-lock snapshot is taken, and records the
effective failure kind and numeric code. A stale current handle fails outright;
a missing older ancestry node is instead reported through the trace's explicit
`corrupt` marker. This distinction prevents a stale failure site from being
presented as a successful-but-corrupt trace.

The renderer writes only value metadata into caller-owned storage:

    panic code=77 execution=<slot>@<generation>
      execution runtime=<domain> id=<id> ... allocation=actor:<domain>/<slot>@<generation>
      execution runtime=<domain> id=<parent> ...
      boundary await source=<token> codegen=<generation> state=<state> ...

It allocates no memory, owns no native frame pointer, supports a size-only pass,
and reports output truncation explicitly. Current native/symbolized machine
frames remain a separate prefix captured while the failing native stack is
alive.

The source actor runtime exposes
`oreslang_source_actor_capture_failure_trace(kind, code, ...)`, so future
lowered throw/raise/panic code does not receive or expose the underlying
ExecutionContextStore or ContinuationStore. It is valid only while the traced
resumed continuation is active.

This branch still does **not** admit unsupported general source raise/panic
syntax. The failure kind is diagnostic metadata after lexical unwind has chosen
the effective control-plane effect. The next exception-lowering slice should
invoke this capture after defer/finally unwind and before actor-local hook /
supervisor promotion, then prepend any current native symbolized frames.


## Post-unwind failure dispatch pipeline

`FailureDispatchPipeline::finish_capture_dispatch` is the first runtime API
that makes the exceptional-control ordering executable instead of relying on
call-site convention:

    defer / finally unwind
        ->
    effective typed failure selected
        ->
    logical failure trace captured
        ->
    actor-local on_throw / on_raise
        ->
    actor-boundary supervisor CONTROL publication

The trace therefore describes the failure that actually survived lexical
cleanup. For example, an incoming Throw whose defer panics is captured as
Panic, with the original Throw retained as a bounded suppressed diagnostic.

Actor-local handling happens only after that capture. If an actor-local hook
handles Throw/Raise, no supervisor event is published and the child's
pre-reserved supervisor slot remains available because the actor continues.
If the hook itself escalates (for example an on_throw callback panics), the
supervisor receives the hook's terminal effect while the diagnostic still
contains the original post-unwind trace. This preserves both causal evidence
and correct terminal control semantics.

A failure to capture diagnostics never hides or downgrades the terminal effect.
A stale current ExecutionHandle produces a bounded
`logical failure trace unavailable` diagnostic and the typed failure still
travels through supervisor CONTROL. Invalid native/FFI effect tags are
normalized by EffectUnwind before trace capture and therefore appear as Fatal,
not as Return or catchable Throw.

ProcessExit is also diagnostically distinct from Fatal in NativeFailureTrace,
even though actor-boundary policy maps it to supervisor HostFailure.

This pipeline remains runtime substrate. General source-level unhandled
`throw`, `raise`, and `panic` lowering is still fail-closed. The compiler
step that admits those forms must route its completed lexical unwind into this
pipeline rather than directly publishing to supervisor CONTROL or rendering a
trace before cleanup has finished.
