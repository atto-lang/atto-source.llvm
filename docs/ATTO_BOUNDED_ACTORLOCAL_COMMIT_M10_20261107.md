# M10 — ActorLocal bounded commit / STOP linearization (host-only hardening)

**Status:** implemented narrow native scheduler admission repair; source-verified, **NOT_EXECUTED** under GitHub Actions. This is not a complete generic ActorLocal guest mutation engine.

## Audit finding and security invariant

The pre-M10 `ores_native_actor_scheduler_commit_if_running_v1` held the lifecycle mutex and verified that the *target actor* was RUNNING, but it did not authenticate the **calling physical carrier and execution claim**. Possessing a live actor reference from a foreign OS thread was enough to trigger a trusted host callback while the actor was active. That violated the documented separation of actor identity from execution authority.

M10 requires all of the following **before** the scheduler runs a bounded commit callback:

1. Caller is presently executing this exact scheduler's authenticated physical carrier.
2. Caller owns the same actor scheduler domain, actor slot and generation.
3. The scheduler's lifecycle lock confirms the exact actor ref remains live, non-retired and the entire scheduler remains active.
4. The actor's execution-authority mutex confirms the caller's carrier/sequence/generation claim is still **RUNNING**.
5. The bounded callback itself performs **no allocation, blocking, guest code, external mutex acquisition, scheduler API calls, or destructors**.

The lifecycle lock already serializes STOP / sealed STOP / scheduler retirement. If a bounded callback wins the lock, its state mutation linearizes **before STOP**. If STOP wins first, callback admission is denied. The callback must touch only preallocated, trivially updated native host state.

## Changed native files

- `src/main/cpp/oreslang/runtime/NativeActorScheduler.c`: exact-carrier and RUNNING execution-claim checks, maintaining scheduler-lock → authority-lock ordering.
- `src/main/cpp/oreslang/runtime/NativeActorScheduler.h`: documents the physical carrier requirement.
- `tests/native_actor_bounded_commit_carrier_stop_v1_test.c`: two real native actor callbacks on one scheduler, lock-free atomic scalar increments, same-scheduler cross-actor refusal, foreign thread denial while RUNNING, STOP-before-attempt denial, and continued sibling progress. Requires `atomic_is_lock_free` before using any scalar in the bounded callback.
- `tests/native_actor_bounded_commit_mutation_witness.sh`: two temporary-source negative mutations. The **foreign-carrier** mutant deletes caller authentication and accepts a foreign thread while its target is RUNNING. The **stop-race** mutant changes RUNNING claim checks to STOPPING-permitting ownership, allowing a post-STOP write. Both must fail with SIGABRT 134 at the expected test assertion; compiler failures, timeouts and unrelated crashes are not negative evidence.

The existing five-lane native workflow and the offline host-only verifier compile and run this test together with the earlier ActorLocal lifecycle tests. All static file pins are checked against the exact Git tree. **No native compiler-backed run is yet observed** because source-org GitHub Actions still reports zero-step jobs.

## Explicit limitations — do not promote generic guest ActorLocal writes

The existing `LogicalLocalScopeV1` uses a mutex-protected `unordered_map` and potentially allocating/destructing `shared_ptr` cell replacements. It is **not safe** to call `scope.set` or `scope.erase` under the scheduler's lifecycle mutex. Simply surrounding a generic map mutation with pre- and post-STOP checks is **not atomic** and cannot be called a STOP-linearized commit.

The new native C11 test uses a **pre-existing lock-free scalar cell**, not an alternate `ActorLocal` map. It proves a narrow transport- and guest-independent **bounded callback admission contract**, not that the current generic ActorLocal map has become transactional.

### Required next implementation layer

Keep **one canonical ActorLocal store**. Prepare (allocate and measure) a replacement outside the scheduler lock, bind it to a tenant/isolate/actor generation and a unique mutation ticket. Only a bounded, nonblocking metadata/pointer publication may occur inside a scheduler-owned commit gate. Retire superseded values **after** leaving that gate, with reader lifetime pins and deterministic supervisor reclamation. Scope closing is valid only after the scheduler's exact-incarnation terminal barrier. Every rejected/stale mutation must preserve the old value, quotas and accounting.

Before any generic guest lowering, add deterministic negative tests for stopped actor admission, stale generation, foreign scheduler actor, cancellation during preparation, concurrent STOP vs commit, duplicate ticket replay, quota overflow, reentrant destructors, and reader pinning; then execute compiled `.atto` fixtures and prove five-backend parity with exact-head receipts.

**Release gates:** stepful source-owned runner canary, GCC/Clang optimized and sanitizers, the two negative mutations, exact Git head/tree/workflow-job receipts and independent review. Neither this note nor local source-free checks override those gates.
