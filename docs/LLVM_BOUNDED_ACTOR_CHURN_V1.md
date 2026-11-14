# LLVM bounded actor churn: compiler-owned synchronous v1

**Status: draft. Native frontend/LLVM emission is committed, but the exact
compiler-backed `.atto` workflow has NOT executed. This is not general
actor-in-loop support or zero-GC certification.**

## Admitted source pattern

```atto
define actor Worker as {
  const base: int = 40;
  pub fnc on_message(int n): int { return base + n; }
}
pub fnc main(): int {
  let iteration: int = 0;
  while iteration < 128 do
    const worker = spawn Worker();
    stop worker;
    iteration = iteration + 1;
  done
  return iteration;
}
```

The LLVM frontend admits one nonnegative int counter, an upper bound
0..1024, one fresh actor `spawn` each trip, an immediate mandatory
`stop` of that same scoped actor, and counter += 1. Braced
`while (counter < bound) { ... }` is also admitted. Any return must
read the counter exactly; the actor ref never becomes a scalar value.

The compiler emits a **real conditional LLVM CFG**, with calls to
`oreslang_source_actor_spawn`,
`oreslang_source_actor_wait_ready_blocking`, and
`oreslang_source_actor_stop` in that order in the loop body. `stop` is the existing trusted host request + physical join:
each actor's scheduler/heap must reach terminal quiescence before control
returns to the next loop iteration. Sequential 128 constructor-ready actor incarnations do
not require 128 concurrently resident actor slots. The trusted-main-only
readiness barrier uses a condition variable and **must not be called from an
actor carrier**. It is not a substitute for nonblocking Future/get_ready. The existing runtime
reopens its group once the last actor is synchronously stopped.

## Deliberate, fail-closed exclusions

- `await actor.get_ready()`, `await actor.get_done()`, message send/receive
  inside the loop, `rt cooperate`, capture/escape, multiple actors per
  iteration, `break`/`continue`, early returns and exceptions are
  **not admitted** by this initial CFG slice.
- This synchronous trusted-main host-join API must not execute inside an
  actor carrier or be mislabeled a nonblocking await implementation.
- Runtime loops with data-dependent bounds, nonconstant increments,
  arbitrary loop locals, borrowed pointers and unbounded execution are
  still blocked pending dedicated effect/ownership/suspension proofs.
- The stricter 128-actor async fixture in PR #1783 remains **not supported**
  and must NOT be substituted with this synchronous test to claim
  `get_done()` parity.

## Exact-head test contract

`scripts/test-source-actor-bounded-churn-v1.sh` enforces immutable
source HEAD and blob identity, builds real LLVM IR at O0/O2, audits LLVM IR
for known GC constructs, requires **call sites** to production actor
runtime symbols within the emitted loop CFG, and executes real linked ELF
at both optimization levels.

- Each O0/O2 matrix run executes the 128-actor braced guest four times
  (native exit status 128, no stdout/stderr).
- It also compiles and executes the canonical `do/done` 7-actor guest
  (exit 7), and zero-trip variant (exit 0).
- Six negative `.atto` fixtures are required to be compiler-rejected
  with no emitted IR: missing stop, double stop, count exceeding the
  bound, async suspension, stopping a foreign reference, and a forged
  trusted-runtime readiness ABI symbol.
- A separately linked real native guest built from the same generated LLVM
  IR and production C actor runtime with `ORESLANG_SOURCE_ACTOR_TESTING`
  must execute under ASan/UBSan at **both O0 and O2**. Its actual runtime
  receipts must prove exactly 128 constructors executed and reached READY,
  128 complete stop/joins, and 128 group teardowns. Immediate spawn/stop
  without this readiness fence can legitimately cancel before construction;
  the exit status alone would not be acceptable evidence.
- A reserved source-actor or scalar-heap ABI name declared by user code must
  be rejected by the compiler rather than linked as a fake host runtime.
- Successful results produce only `SOURCE_ACTOR_BOUNDED_CHURN_*`
  receipts with `async_guest=NOT_EXECUTED` and
  `zero_gc_certified=NO`. O0/O2 compilation, native execution and
  compiler-negative rejections must all succeed.

## Production-runtime READY concurrency regression matrix

A separate **host ABI** test, not the `.atto` guest, now exercises the actual
`SourceActorBootstrap.c` and its real VM/actor heap under Clang O0/O2
ASan+UBSan. Its exact-head runner is
`scripts/test-source-actor-ready-barrier-v1.sh`, with cases in
`tests/source_actor_ready_barrier_races_v1.c`.

For each optimization level it requires:

- 16 real actor constructors held before initialization and 16 held after
  constructor execution but before READY publication, with a trusted host
  thread confirmed to have entered the actual condition-variable wait.
  Wakeup must occur without a lost signal, and all 32 actors must physically
  terminate after successful READY.
- Eight additional actor lifetimes where READY was already published before
  the host wait begins. Those calls must take the nonblocking fast path.
- A real stop-before-READY cancellation **after** a host waiter has entered
  its wait. The waiter must wake and fail stop with SIGABRT and a test-only
  `SOURCE_ACTOR_READY_BARRIER_CANCELLED` marker; returning READY, hanging,
  or an unrelated sanitizer crash is not an acceptable pass.
- A real source actor message callback attempting the host-only READY barrier
  from a VM actor carrier. The call must fail stop with SIGABRT and a
  `SOURCE_ACTOR_READY_BARRIER_CARRIER_DENIED` test marker, never park that
  carrier. The guard checks *all four native VM domains*.
- Test-only markers are compiled only with
  `ORESLANG_SOURCE_ACTOR_TESTING`; regular source guest builds do not print
  them or expose the waiter-observation hook.

Successful exact-head results would thus require **80 actual constructors and
physical joins**, **64 observed CV waits**, **16 READY fast paths**, and
**four explicit fail-stop negative executions** over O0/O2. These are
**test requirements, not recorded test results**: source organization Actions
currently fails prior to executing steps. Host ABI success remains distinct
from compiler-generated `.atto` execution and whole-process zero-GC proof.

The source feature branch was synchronized with LLVM main commit
`3667b0255269ca30de381077435253f1b40be28f` using merge commit
`4cc1fbc33c07ccda9a0a677e2c46c5b4e35f68ed`. The merged tree retained
all 17 preexisting feature files and every unrelated upstream file. The
overlapping `Parser.cpp` received the exact upstream lexer/import additions
without losing actor-loop checks. Later feature changes remain on top of that
merge.

## Next work before promotion

1. Restore source GitHub Actions runner admission (#159): no-executed-step
   runs are neither compiler passes nor meaningful test failures.
2. Validate independent closed-loop heap and scheduler instrumentation,
   including ASan/UBSan, retained bytes and the physical free boundary.
3. Merge/rebase known-GC linked ELF security gates from #1780 only after
   its independent review and exact-head native checks.
4. Implement general actor refs across async continuations and loop phis,
   typed DATA messaging, `get_ready()` and `get_done()` cancellation,
   linear/affine ref ownership and compiler borrow-checking (#1787).
5. Enforce source `main` branch protection and independent approval
   (#915); do not mirror private source before reviewed carrier
   authorization is live.

This slice proves only what it actually executes. Native actor loops are
not equivalent to full async actor lifecycle, hard real-time safety or
whole-process zero-GC.
