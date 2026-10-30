# CGL dispatch evidence v1 — timer expiry through actor user-code start

Status: **experimental validation contract**. It is neither admission to a
privileged scheduler nor a hard-real-time, carrier-grade, or certification claim.

## The observable path

All timestamps in a trace must use a single **monotonic nanosecond clock** on
the same host; wall clock and mixed clock domains are prohibited.

1. `armed_ns`: actor-owned timer ticket becomes registered with the scheduler.
2. `due_ns`: monotonic target expiry for that ticket (not the callback time).
3. `notified_ns`: scheduler reports a winning expired ticket to the actor.
4. `user_start_ns`: **first instruction of guest actor handler/user code**, after
   admission, queue arbitration, and carrier scheduling. Merely waking a worker
   or invoking the timer callback is insufficient.
5. `user_finish_ns`: final guest instruction; completion time is a separate
   schedulability/WCET concern, not interchangeable with start latency.
6. `deadline_ns`: independently configured latest permitted user start, with
   a separate completion deadline when the workload requires one.

Record the stable actor incarnation/generation, tenant-isolation boundary,
64-bit unique ticket, cancellation acknowledgement, and bounded queue depth.
The trace writer must be bounded and nonblocking on RT paths. Its record-loss
counter is part of the full-run evidence: any lost/missing/corrupt records
invalidate a claimed pass. No trace analysis or dynamic allocation belongs in
the hard-RT carrier's dispatch critical path.

## Observed contract (not a guarantee)

Require a positive, predeclared sample minimum and thresholds for both
`notified_ns -> user_start_ns` and `due_ns -> user_start_ns` in nanoseconds.
Reject or fail on these conditions:

- `armed <= due <= notified <= user_start <= user_finish`, or any timestamp
  absent, inconsistent, in a different clock domain, or overflowing.
- `user_start > deadline`, even when `notified <= deadline`.
- Either measured latency above its declared threshold, no completed samples,
  insufficient samples, or a breached queue-depth cap.
- Duplicated winning ticket; stale actor incarnation/generation; cancellation
  acknowledged and handler user code subsequently started; missing cancellation
  fence acknowledgement or a post-ack notification.
- An event truncated by process shutdown/hot reload, untracked backpressure,
  sampling bias, or any lost trace events. They cannot be silently skipped.

The source-local LLVM `CglDispatchEvidence.hpp` is an **offline** evaluator
for a complete bounded trace array. It does not collect traces from OresVM
and deliberately always reports `hard_realtime_qualified=false`. Its duplicate
check is O(n^2), so it must never be called on the RT path.

## Implemented observation seams (not full timer correlation)

**LLVM experimental native carrier:** `MultiActorCarrier::register_actor` now
accepts an optional caller-owned `CglCarrierDispatchTrace` sink. After
authenticated actor slot/generation admission and immediately around the
native `slice` callback, the worker records monotonic start/finish timestamps,
message sequence, first-slice versus continuation, and actor generation.
The fixed-capacity 256-record sink performs no heap allocation or locking on
the single-worker append path. It detects overflow and readers must wait
until `carrier.join()`. An independently known positive expected event count
is required for full-coverage checks. This is a **native callback entry**,
not the first compiled guest instruction, and the carrier currently does
not know timer-due or timer-notification data.

**GraalVM actual actor runtime:** opt in *before* creating `ActorRuntime`
with `-Dores.runtime.cgl.handlerTrace=true`. The runtime allocates one
fixed-capacity `CglActorMessageTrace` at initialization. It timestamps
both source actor `receiveActor(...)` and host `behavior.onMessage(...)`
around the already-admitted, nonreentrant handler call. No new per-message
sample object is allocated; synchronization is host/JVM **soft-RT only**.
The caller must wait until actors stop, explicitly seal, then inspect a
snapshot against a positive expected admitted-handler count. The buffer
tracks invalid and dropped records and fails closed on either. Mailbox
sequences are zero-based. A source handler method call is **not** necessarily
the first bytecode/instruction executed inside the guest language.

**Still missing:** a common source-owned timer-ticket/join identity linking
arm/due/notify with authenticated mailbox receipt and first actual guest
instruction. Do not synthesize due/notification values, or combine different
clock domains, to make either producer satisfy the offline `DispatchTrace`
contract. Runtime/VM instrumentation and real host trials remain P0 work.

## Runtime parity implementation gate

For **LLVM**, add actor-scheduler instrumentation at ticket registration,
monotonic timer arbitration, mailbox delivery, and the first actual guest
instruction of a nonreentrant handler, using actor-owned preallocated records.
For **GraalVM**, capture the same boundaries with the native/Truffle actor
runtime; do not use a timer callback as a proxy for actual user start or infer
physically separate GC heaps from arena ownership. Verify recorded timing and
identity semantics with a cross-backend golden trace corpus. For both, retain
isolate/tenant generation fences on hot reload and shutdown and verify that
CONTROL/supervision progress persists during mailbox saturation.

## Tiered qualification

**Level 1 — Linux compatibility:** exact-root build, native image ABI and
successful guest execution. ELF admission is structural, not runtime mapping
or dependency certification.

**Level 2 — observed soft real time:** verified timer-to-user-start evidence on
actual target hosts across clean and contended CPU, IRQ, cgroup/SMT pressure,
network storms, bounded queues, and supervisor/fault scenarios. Publish
p50/p99/p99.9/**maximum**, all misses, lost traces, actual policy/affinity and
host/kernel metadata. This can expose violations but cannot prove worst-case
latency across all workloads.

**Level 3 — prospective hard real time (LLVM only):** formal admission with
specified bounds for memory and execution, PREEMPT_RT and scheduler privilege
attestation, exclusive RT CPU/IRQ ownership, locked and prefaulted actor heaps,
AOT-only bounded paths, absence of unbounded allocation/GC/blocking/suspension,
verified WCET/schedulability, and cancellation/rollback recovery. No scalar
host smoke, static ELF check, or sample-only latency pass qualifies as Level 3.

The **exact source commit + root tree**, toolchain, native executable hash,
runner job and executed step count, test profile, kernel image, CPU topology,
policy/priority/cgroups, and captured event count are required in a published
qualification receipt. A zero-step CI failure is infrastructure evidence only.

**CI security boundary:** source-org CI first. A funded private runner in a
different organization is not permission to mirror private source. Require
independent owner approval, effective source-main protection, immutable
allowlisted destination ID and resolved open source-transfer incidents first.
