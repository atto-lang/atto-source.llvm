# AttoLang four local-storage domains: native LLVM implementation boundary

Status: native host substrate plus **authored, not-yet-executed** restricted `.atto` `TaskLocal<int>` compiler lowering and negative/positive compiled-guest harness; **not** general guest typed-key support or cross-backend parity.

| API | Identity | Lifetime | Implementation |
| --- | --- | --- | --- |
| `ThreadLocal<T>` | physical OS thread | thread exit | `PhysicalThreadLocalsV1` in `LocalContextDomainsV1.hpp` |
| `ActorLocal<T>` | tenant + isolate + actor slot + actor generation | actor FINALIZED | `ActorLocalScopeV1` in `LocalContextDomainsV1.hpp` |
| `TaskLocal<T>` | actor identity + Task ID + Task generation | Task retired | `TaskLocalScopeV1` in `LocalContextDomainsV1.hpp` |
| `RequestLocal<T>` | admitted HTTP request + request generation | request terminal and pins drained | pre-existing `RequestContextLocalV1.hpp` |

These are **typed library facilities**, not new `threadlocal`, `actorlocal`, `tasklocal` or `requestlocal` grammar keywords. The common contract is in [conformance PR #331](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/331).

## Implemented native rules

- Task and actor cells are independent of worker TLS, bounded to **32 keys each** and keyed by a host-minted unique typed identity.
- A **scheduler-authenticated** execution stamp is *required at call sites*. The C++ `ExecutionStampV1` is only a data record: **it is not a cryptographic or runtime-issued capability**. Never surface its constructor or `open` to guest source. New `ActorLocalSchedulerTurnV1.hpp` additionally rechecks the real scheduler's current actor claim and current execution record on every host access; this is not yet compiler-lowered actor-local source.
- Actor reads/writes require an exact tenant/isolate/actor incarnation. The actor's single logical turn or legal `critical` section must be authenticated **outside this container**. The scope mutex is not an alternative to actor-state arbitration.
- Task reads/writes require exact Task incarnation; the logical scope may be handed to a continuation on a different OS carrier without copying TLS. Two Tasks on the same actor are isolated.
- A child Task inherits **nothing by default**. `copy_to_child` transfers a snapshot for one explicitly selected `Copy` key. The compiler must additionally establish `Send`, ownership and correct structured-concurrency lineage before making this guest-visible.
- Scope closure atomically fences new slot access and detaches existing cells; destructors and copy constructors execute outside its mutex. In-flight reads may complete with a previously admitted, pinned copy. No borrowed reference is returned.
- Physical TLS is intentionally **not** an execution authority, and its slots are destroyed at OS-thread exit. A Task/carrier migration does not migrate TLS.
- The existing RequestLocal candidate is **request-specific** and independently cancel-fenced. It must not be reconstructed from an actor singleton or a physical thread variable. The current native RequestLocal API is separate from generic TaskLocal/ActorLocal key typing.

## Blocking integration before guest promotion

1. Bind lifecycle creation and close to actual `ActorCell`, Task continuation and request admission/retirement, with **runtime-issued nonforgeable per-turn context authority**; plain host stamps alone are not authoritative.
2. Bind Task cancellation/revocation to TaskLocal close, actor stop/finalization to ActorLocal close, and verified HTTP ingress/response completion to RequestLocal close, after physical work and pinned descendants drain.
3. Add compiler-owned static key descriptors, full `T` type identities and AOT-safe lookup; reject forged key IDs, arbitrary pointers, borrows spanning suspension, non-`Send` transfers and mutable cross-actor aliases. C++ traits alone cannot validate guest types.
4. Enforce byte quotas and allocation-arena ownership in addition to current **slot-count** limits; a slot can currently hold an unbounded host allocation.
5. Execute the newly authored `tests/fixtures/tasklocal-i64-source.atto` and `tests/tasklocal_i64_e2e.rs`: real compiler IR, linked native scheduler, exit status 42, and seven rejected files with no emitted IR. Extend to genuinely general keyed/continuation `.atto` fixtures and then Java/GraalVM, Wasm, .NET NativeAOT and Forge. Unsupported features must fail closed.
6. Verify exact-head native/JDK/NativeAOT/Wasm receipts with nonzero executed steps. **Zero-step CI is NOT_EXECUTED**; a host-only passing test cannot promote guest parity.

## Host test

From repository root, compile with GCC/Clang `-std=c++17 -pthread -Isrc/main/cpp` and run `tests/native_context_domains_v1_test.cpp`. CTest also registers `native_local_context_domains_host_only`.

Checks include: physical TLS isolation and destructor timing; same actor/two distinct Tasks; Task values surviving physical-carrier migration; actor and task generation replay denial; explicit copy-to-child; 32-slot quotas; closed-scope access rejection; 200 concurrent close/write races; reentrant copy and erase safety. The existing `native_request_context_local_v1_test.cpp` separately exercises HTTP RequestLocal on a real native actor scheduler.

No guest `.atto` code was compiled by these host tests.

## New restricted LLVM source-level TaskLocal<int> integration (not parity-certified)

`SourceTaskRuntime.c` now embeds eight fixed-size i64 TaskLocal cells in the actual
`SourceTaskI64` object. The compiled `.atto` specialization still uses only slot zero. Trusted host-only checked slot-index APIs support eight independent cells with get/set/clear, bounds checks, and zero allocation; slot numbers are **not** a guest API. Both checked and fail-closed guest ABI functions
require `ores_native_actor_scheduler_is_current_actor` for the current
scheduler's active Task producer. Foreign threads, stale completions and
retired Tasks cannot read/write it. The slot is zero-initialized and its
presence flag is false for each new Task; full physical shutdown frees it.

The LLVM parser recognizes only this **closed-world fixed-key specialization**
inside a source Task body:

```atto
pub fnc main(): int {
    let Task<int> work = task {
        TaskLocal<int>.set(41);
        return TaskLocal<int>.get();
    };
    let int value = await work;
    return value + 1;
}
```

No general `TaskLocal<T>` object, lexical override, multi-key identity, child
inheritance or source-level suspension inside that producer is admitted.
`TaskLocal<int>.set` accepts only a capture-free int scalar; `get` requires a
previous in-block initialization; the Task must be awaited exactly once.
The compiler binds ABI symbols directly only after parsing and type checking,
and rejects raw FFI function calls from guest source.

`native_tasklocal_i64_compiled_guest` is the executable acceptance gate.
`native_source_task_i64_shared_scheduler_runtime` additionally tests
out-of-turn/stale read/write denials and fresh slot state over repeated
scheduler Task admissions. `native_actorlocal_authenticated_scheduler_turn`
tests two actors with actual scheduler callbacks, rejecting foreign threads,
wrong actor, wrong-generation accesses, and retirement before real actor termination.
The bridge cross-checks the current actor claim against the scheduler's actor
allocation kind/domain/owner/generation, and exposes a terminal-fenced close
that refuses to block a scheduler carrier.

**Evidence status:** [workflow run #41774058053](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/41774058053)
reported `steps=null` for all six jobs (including the compiled-guest job).
They are **NOT_EXECUTED**. New native and guest paths remain draft and
unverified by stepful CI, despite already-authored code and tests.

## Explicit remaining authorization limits

The eight-slot i64 specialization is **not** a generic `TaskLocal<T>`
implementation and cannot be used as evidence of task inheritance or carrier
migration. A future compiler-generated key descriptor must bind each static
key to a slot and enforce ownership, Copy/Send and task lineage. The native
SourceTask runtime still admits only one capture-free live Task per process.

The ActorLocal scheduler adapter proves the *actor incarnation* using a real
actor execution claim and allocation record. The `tenant` and `isolate`
fields in `ExecutionStampV1` remain host-supplied; this adapter alone does
not authenticate tenant or isolate scope. Guest API exposure is blocked until
that authority is derived from trusted scheduler/tenant admission, not caller
supplied identity. The source repository's GitHub Actions remain a zero-step
runner-admission blocker.
