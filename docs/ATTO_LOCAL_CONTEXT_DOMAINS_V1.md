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

## Native TLS teardown regression (November 4, 2026)

PhysicalThreadLocalsV1 now marks its storage closing before releasing per-thread values. Get, set and erase reject reentrant value destructors during thread exit. A native regression covers 32 thread-exit cycles. The local C++17 teardown model passed GCC, Clang and Clang ASan/UBSan; these results do not certify the full repository binary.

The native local-context GitHub workflow now checks out the exact PR head and verifies refreshed header and test object hashes. The source organization's Actions jobs remain unexecuted. A separate source-free GitHub Actions canary in benefactor-cc/benefactor-e2e (run 41776988571) completed five steps successfully, demonstrating runner capacity only. No AttoLang source was moved. The repository's cross-organization private-source approval list remains empty, so source mirroring is prohibited until independent authorization.

## 2026-11-04 four-domain follow-up audit

**RequestLocal reentrant copy safety:** `RequestContextLocalV1::get_copy`
now pins the typed cell under its mutex and copies outside that mutex.
Previously a value's C++ copy constructor could call `close`, cancel the
request, or replace its own cell while the lock was held, causing deadlock.
The pinned shared cell stays alive during the copy; a read authorized before
concurrent cancellation may complete with an owned snapshot. The regression
adds 48 alternating reentrant close/cancel copies, a replacement destructor
reentry probe, and assertions that no later read or set succeeds after close.

**Trusted request actor-turn adapter:** `RequestLocalSchedulerTurnV1.hpp`
checks the real scheduler's current actor claim, allocation kind/domain/slot/
generation and the request scope's actor binding before invoking host storage
APIs. A raw scope pointer outside an actor turn cannot use this adapter.
However, it does **not** authenticate the request ID, tenant or isolate from
an HTTP ingress/continuation ticket; this is an important blocker before
exposing any RequestLocal accessor as a guest intrinsic.

**Shared host value type admission:** `LocalValueAdmissionV1.hpp` is used by
all four host storage domains to reject raw pointers/references, top-level
`std::reference_wrapper`, `std::basic_string_view`,
`std::initializer_list`, C++20 `std::span`, and types with potentially throwing destructors.
This is only shallow C++ host admission; composite types may still contain
borrowed aliases, so guest compiler `Copy`/`Send`/ownership analysis remains
mandatory. Negative `static_assert` cases were added to Task/Actor/Thread
and Request host tests.

**Testing boundary:** The exact Git blob
`7360f5a623093d367578ab3e9d811c521003d450` for the new admission
policy compiled and executed with GCC -O2, Clang -O2, and Clang ASan/UBSan
locally, including positive and negative compile-time assertions. A separate
source-free RequestLocal pin-then-copy model passed 1,000 synchronous
reentrant close operations and 200 concurrent-close runs with GCC, Clang,
Clang ASan/UBSan, and GCC TSan. **Neither model proves that the full RequestLocal
runtime and scheduler tests executed.** Those native exact-head and compiled
`.atto` gates still require real source CI with nonzero steps.

Both native-local-context and native-request-context GitHub workflows now
verify the **exact pull-request head** and current policy, source, test and
scheduler Git blob hashes. Zero-step CI failures are runner admission failures,
not an AttoLang pass or failing unit-test result. No private-source export to
another organization occurred; the authorization hold remains in force.


## 2026-11-04 follow-up: composite alias admission and RequestLocal erase

### Corrected replacement regression

The authored RequestLocal destructor regression had a false premise: defining a
custom destructor suppresses C++'s implicit move constructor. The original
`ReentrantRequestDrop` probe was **copied**, not moved, during `set(T)`.
That caused the captured `on_drop` callback to run during the first insertion
rather than exactly once on actual cell replacement. The test now explicitly
moves `std::function` callbacks and disarms moved-from temporaries. A
standalone C++17 reproduction of old vs fixed semantics ran with GCC and
Clang; old=2 premature callbacks, fixed=1 owner callback.

### RequestLocal quota reclamation

`RequestContextLocalV1::erase<T>` now removes one key and returns a quota
slot, returning false for absent, closed, or cancelled requests. The
`shared_ptr` is retired **after** unlocking, so value destructors may
reenter the same request safely. Added positive 32-slot quota reuse, duplicate
erase denial, cancelled/closed erase denial, deterministic reentrant
destructor -> set replacement, and concurrent erase/set/cancel tests.

### Deepened host-only alias restrictions

`LocalValueAdmissionV1.hpp` now rejects direct `std::shared_ptr`,
`std::weak_ptr`, `std::function`, and `std::any`. Its recursive STL
container specializations reject hidden pointers or borrowed views inside
`optional`, `pair`, `tuple`, `array`, `vector`, `deque`, `list`,
`forward_list`, `set`, `unordered_set`, `map`, `unordered_map` and
`variant`; in C++20, every `std::ranges::view` is rejected, including
reference views. This is intentionally conservative: `rt share` must use a
dedicated, governed host-guest capability, not an opaque C++ smart pointer.
C++ cannot inspect arbitrary user-defined struct fields, so **this is still
not a substitute for AttoLang compiler-level recursive ownership/Send/Copy
checks**; guest exposure remains denied.

The **exact checked-in policy blob**
`7b79138951aa878fe2ee43cfcef21277cb68b256` matches the locally compiled
header and passed compile-time positive/negative assertion suites with GCC
C++17/C++20, Clang C++17/C++20 and Clang C++20 ASan/UBSan.
Full RequestLocal and ActorLocal/TaskLocal/ThreadLocal repository tests have
only been **authored**, not executed on the source-org CI runner; do not
promote model/isolated header evidence to full guest parity.
