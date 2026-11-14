# Native receiver quiescence and physical resource retirement v1

**Status: host/native runtime slice on draft PR #1793. This is not evidence that
two source-level AttoLang `receive` callbacks run concurrently on LLVM.**

`NativeActorReceiveFence` provides a bounded actor-incarnation-scoped
admission and cleanup fence. It does not create an actor or an OS thread and
does not run guest code. A `ReceiveLease` pins one potentially concurrent
receive invocation; future compiler lowering must keep the token alive through
all suspension/resume paths, and `try_enter` must be called at the real
scheduler admission point, not merely parser acceptance.

The gate owns a single shared, mutex-protected state:
`actor_id`, `incarnation`, bounded active count, STOP/graceful state and
teardown-start/active/finish flags. Each RAII lease references that state,
not a raw arena address. Once STOP seals admissions, no future receive may
enter. Only a gracefully stopped, receive-quiescent actor may begin one
guest teardown callback. Cancellation during an active teardown does not
make that callback safe to release; physical memory still waits for its
receipt. Runtime operations never execute guest callbacks under this lock.
No tracing collector or dedicated reclamation thread is required.

`NativeActorRetirementBarrier` accepts an optional validated
`NativeActorReceiveFence` and now refuses native resource registry closers
or `get_done` until the fence is ready. A wrong actor/incarnation token is
rejected. Existing group/carrier, FFI resource registry, native loans and
source cleanup receipts remain independent mandatory conditions; this
addition does not weaken them. Callers that do not pass the fence retain
their previous serial-actor behavior. Runtime source actors must explicitly
wire the fence before using it to claim concurrent receive safety.

## Additional validation and exact-blob local smoke

The native gate now rejects zero actor ID, zero incarnation and invalid
receiver capacity **before** allocating its trusted state. This matches
the `ActorResourceRegistry` identity requirements and avoids inconsistent
generation-zero actor receipts.

On 2026-11-14, the local standalone test compiled the exact Git blob
`681442c854abc8acf8009f1ccf1467af5f8dff95` of
`NativeActorReceiveFence.hpp` (verified with `git hash-object`).
The executable test passed **2,000 two-carrier STOP/admission races**
under both Clang 17 and GCC 14 C++17
`-Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined`,
including wrong-generation, capacity, repeated release, teardown and
generation-zero negatives. Leak detection was enabled. This is direct
execution of the exact **header** in a standalone test, not an execution
of `NativeActorRetirementBarrier` or full repository CTest target.
The native retirement integration tests remain authored, NOT_EXECUTED.

## Evidence and next execution gates

The existing CMake test `native_actor_done_resource_quiescence` has new
checks for two simultaneous outstanding receive receipts, actual registry
close once, DONE still pending, teardown pinning, stale generation, and 64
two-physical-thread STOP races. It is a **host-only test** until the
compiler emits real `.atto` concurrent receive/continuation invocations.

Standalone reconstructed-header smoke checks passed C++17 strict warnings
and Clang/GCC AddressSanitizer+UndefinedBehaviorSanitizer, including 64
two-thread stop/admission rounds. The local smoke is not an exact GitHub
PR-tree execution receipt or actual guest source lowering.

Run `ctest -R '^native_actor_done_resource_quiescence$' --output-on-failure`
after building the exact PR head, then repeat with ASan/UBSan and compiled
AttoLang source. Source-org GitHub Actions workflows currently fail with
no executed steps. The private-source export gate in
`docs/security/CI_PRIVATE_SOURCE_MIRRORING.md` denies ad-hoc mirror runs;
never mistake alternative-org runner availability for authorization.

The next compiler integration must preserve per-invocation owned logical
stacks across await/select, include one lease per live invocation, enforce
serial actors' single-live-invocation restriction, and handle all native
resource pins before physically freeing the actor arena. Five-backend parity,
full guest AOT and process-level zero GC remain unverified.


## Source-local full native retirement target (no LLVM SDK required)

The PR adds `ATTO_ACTOR_RETIREMENT_HOST_ONLY` to CMake. Unlike an isolated
standalone receive-gate test, this option builds the **same production**
`ActorOwnedI64Heap.c`, `NativeActorRetirementBarrier.hpp` and
`native_actor_retirement_barrier_test.cpp` used by the full native test
suite. It can be run from an approved local checkout without an LLVM SDK,
Rust runner or cross-organization private Git transfer.

```sh
EXPECTED_HEAD="$(git rev-parse HEAD)" \
  bash scripts/test-native-actor-retirement-host-v1.sh
```

The script fails on a source HEAD mismatch, builds with **both Clang and
GCC**, enables ASan/UBSan with leak detection, and runs
`native_actor_done_resource_quiescence` through CTest. It leaves no
persistent source copies in another repository.

The production integration test now also launches two real host threads,
holding independently admitted receive invocation leases while the group
stops. Releasing only one must keep native resource destruction and DONE
pending. Releasing both permits one teardown; only its completion permits
exactly one native closer and DONE. A separate capacity-one serial gate
test prevents a second receive while the first logical invocation is
suspended or physically active.

**Evidence boundary:** The isolated exact-header sanitizer smoke executed
locally on 2026-11-14: Clang and GCC each passed an additional **1,000**
two-thread STOP/admission rounds against exact blob SHA
`681442c854abc8acf8009f1ccf1467af5f8dff95`, verified with
`git hash-object`. The new **production integration** CMake/CTest path
has been authored but **not executed on the full source checkout**.
No compiled `.atto` actor or full-AOT result is claimed.

## Two-host-carrier real arena retirement test refinement (2026-11-14)

The host-only `concurrent_host_receivers_join_native_resource_retirement`
test now allocates and reads an actual
`OresActorI64Heap` for actor 23/incarnation 4. It retires the heap
through the group callback and permits `ores_actor_i64_heap_try_destroy`
**only** from the runtime registry closer, after the two admitted receiver
leases and approved teardown are physically complete. The test checks
that a native allocator closer is invoked once and that DONE cannot
settle while a live receive or lifecycle hook still owns memory.

The scalar `OresActorI64Heap` ABI itself still admits only one turn
at a time; the two host threads own distinct **receive-fence receipts**,
not two compiler-generated concurrent guest stack frames. Do not call
this a source-level concurrent actor execution proof.

The exact previously verified native header blob
`681442c854abc8acf8009f1ccf1467af5f8dff95`
passed another local 1,000 STOP/admission rounds under Clang 17 and
1,000 under GCC 14, with C++17 strict warnings and ASan/UBSan plus
leak checking. The **full** updated actor-heap/CMake CTest is
**NOT_EXECUTED** on the source PR tree. GitHub Actions job reports still
show failure before any executable job steps.

## STOP sealing moved into trusted native retirement

Audit found that `NativeActorRetirementBarrier::retire()` could publish
native resource retirement while the optional receive gate still admitted
new invocations. Previous tests hid the gap by separately calling
`receives->seal_stop(true)` **after** `group.close()`.

The barrier now seals its attached receive gate as the first operation of
`retire(bool allow_graceful_teardown=false)`. The default is terminal/
abortive and **never grants guest tear_down**. A verified graceful stop
must explicitly call `retire(true)`. STOP then cannot admit a new receive
after the barrier's retirement callback returns. A later force seal may
downgrade graceful retirement, never upgrade abortive to graceful.

For native heap/I/O callbacks that do work **before** calling `retire()`,
the trusted coordinator must call `barrier.seal_stop(graceful)` as the
FIRST callback operation; `retire()` repeats the seal defensively. The
real actor-heap test now seals before retiring its scalar heap.

The new regression invokes group close without any separate caller-side
receive seal and verifies abortive STOP refuses further receive admission,
forbids guest teardown, retains DONE while a live receiver pin exists,
then closes one registered native resource and resolves DONE after the
receipt retires. The graceful two-host-carrier test also asserts receipt
denial immediately after group close. These full native integration tests
are **AUTHORED_NOT_EXECUTED** on the exact PR source checkout; the local
exact-blob `NativeActorReceiveFence.hpp` tests passed 2,000 two-carrier
races with Clang 17 and 2,000 with GCC 14 under ASan/UBSan (4,000 total).
No compiled guest or five-backend concurrency certificate is claimed.

## Finalizer identity binding (2026-11-14)

The native retirement barrier previously bound its optional cleanup and
receive fences to `ActorResourceRegistry::actor_id()/incarnation()`, but
did not bind its `NativeActorGroup::RuntimeFinalizer` to the same pair.
A wrongly wired trusted coordinator could therefore attach actor B's DONE
capability to actor A's resources.

`RuntimeFinalizer::belongs_to(actor, incarnation)` is a native-only,
read-only identity predicate. The barrier constructor now rejects a
finalizer whose actor ID **or generation** differs from the resource
registry, before it can retire resources or acknowledge another actor.
The finalizer's existing opaque ticket remains the platform authority
when it actually settles DONE; this numeric guard is defense in depth,
not a replacement for ticket validation.

The production native retirement test now includes
`foreign_finalizer_cannot_acknowledge_another_actor_registry()`:
a foreign actor with the same generation, and the same actor ID with a
foreign generation, must both be rejected. The unrelated groups then
close and finalize independently to show the invalid construction did
not consume or damage the foreign capabilities.

The focused host-only CI workflow's path filters now include
`ActorGroupLifecycle.hpp` and `ActorResourceRegistry.hpp`.

**Evidence boundary:** This is a reviewed/authored production CTest
regression, not an executed full native repository build. The source
Actions job still fails before executable steps. The exact receive
header standalone sanitizer smoke remains separately verified; it
does not certify `RuntimeFinalizer` or `NativeActorRetirementBarrier`.

## Exact actor ticket ownership (2026-11-14; supersedes numeric finalizer matching)

Further review found that (actor ID, generation) and even
(runtime ID, actor ID, generation) are insufficient as physical lifetime
authority. Separate runtimes can reuse numeric actor identifiers, and
a runtime can recycle an old numeric identity after physical DONE.
The earlier `RuntimeFinalizer::belongs_to(...)` numeric predicate has
therefore been **removed**.

The native resource registry now provides `bind_owner(NativeActorRef)`
for the trusted coordinator to call once after `NativeActorGroup::adopt`.
It rejects an actor/generation mismatch, duplicate binding, and
post-retirement binding. The stored owner retains the opaque actor
lifetime ticket; `matches_finalizer` delegates to the runtime
finalizer's existing exact `matches(ref)` ticket check.

`NativeActorRetirementBarrier` now rejects any unbound registry or
finalizer not attached to the **same opaque actor ticket**, before any
native resource retirement or DONE settlement. All 14 existing
production retirement-test registry instances were updated to bind
the corresponding adopted ref. The intentional unbound negative case
remains unbound. Further native CTest cases reject:
- Unbound registry retirement.
- Wrong actor ID or generation.
- Different runtime with equal numeric actor ID/generation.
- Reused (runtime, actor ID, generation) tuple with a *new* opaque ticket.
- Duplicate or post-retirement registry owner binding.
- Finalizer from another group whose actor registration remains valid.

The SDK-free `ATTO_ACTOR_RETIREMENT_HOST_ONLY` build now contains
**both** `native_actor_done_resource_quiescence` and the production
`native_actor_resource_registry_quiescence` test. The checked-in
script requires exactly two targets and runs Clang and GCC with
ASan/UBSan. Retirement runs with LeakSanitizer enabled. The resource
registry failure tests intentionally quarantine unresolved native
callbacks, so their distinct sanitizer invocation disables only
LeakSanitizer and **does not constitute a leak-free registry claim**.

Audit additionally found a malformed shell-test tail in an intermediate
script commit. It was corrected, and the byte-exact final script
blob `654f24ed6d5e9f9f6a06c452cc0959940608e2e0`
was reconstructed locally and passed `bash -n`, with the local
`git hash-object` matching GitHub.

**Execution boundary:** The new full C++ retirement/registry integration
targets are still **NOT_EXECUTED** on a source checkout. Source Actions
have not executed job steps. Isolated exact-source receive-gate smoke
tests remain separately verified with sanitizers; they do not prove
this new opaque owner-binding code passed CTest.

## Adversarial exact receive-header smoke (2026-11-14)

A fresh local test used the **byte-exact checked-in native receive gate**
blob SHA `681442c854abc8acf8009f1ccf1467af5f8dff95`,
verified with `git hash-object`. Independently compiled by
Clang 17 and GCC 14 (`-std=c++17 -O1 -g -pthread -Wall -Wextra
-Wpedantic -Werror -fsanitize=address,undefined`), both executions passed:
4,096 simultaneously admitted move-only leases; rejection at
capacity; rejection of a wrong incarnation without losing a receipt;
cross-actor lease move assignment releasing the previous actor's pin;
lease destruction after the gate factory was destroyed; a live
teardown surviving terminal-downgrade; and 2,000 two-carrier
STOP/admission races **per compiler** with zero leaked receipts.

The production `native_actor_retirement_barrier_test.cpp` now also
covers 4,096 admission capacity, lease moves and post-factory
lifetime, and increased its integrated STOP stress rounds from
64 to 256. This **new full production test is not yet executed**
against the complete source repository. The standalone exact
receive-gate smoke does not certify registry opaque-ticket binding,
native arena CTest, compiled `.atto`, or full AOT.

The host-only native CI script has been reconstructed from the exact
GitHub blob `654f24ed6d5e9f9f6a06c452cc0959940608e2e0`
and **passed `bash -n`**, with local `git hash-object` equal
to the remote Git blob SHA. This is syntax verification only;
the script's full native test commands remain not executed.

## Native ActorGroup STOP dispatch and physical callback receipt (2026-11-14)

An additional trusted host race was found in `ActorGroupLifecycle.hpp`:
`stop_workers()` marked workers `stopping` while still holding the
admission monitor, then notified readiness observers before issuing
STOP. A blocked notification delayed physical STOP indefinitely.
Concurrent `close()` skipped workers already marked `stopping`,
allowing close to return without submitting their stop callback.

`stop_workers()` now dispatches requested stops **before** invoking
external readiness observers. `close()` dispatches for every detached
entry (including those marked stopping), relying on the existing
ticket-local atomic `stop_issued.exchange(true)` for exactly-once
delivery. This removes both the missing dispatch and callback-order
starvation windows.

Another independent race allowed `RuntimeFinalizer` to observe a
detached group while its previously dispatched STOP callback was
still physically executing on another carrier. The shared actor
ticket now has a separate `stop_dispatch_finished` receipt, published
with release ordering **after the callback actually returns or
throws**. Detached `RuntimeFinalizer::ready_for_actor_turn_quiescence`,
`finalized_after_close`, and the live group
`NativeActorGroup::finalized` (when STOP has been requested) reject
premature DONE/resource release until this receipt is visible.
The callback completion bit alone does **not** certify successful
native cleanup: the retirement registry, receive gate, cleanup gate,
I/O loans, and quarantine checks remain independent requirements.

Three deterministic native group regressions were added to the
production `actor_group_lifecycle_test.cpp`:

1. STOP callback blocks physically while `close()` runs on another
   carrier; both resource readiness and DONE must remain pending.
2. A readiness subscriber blocks while `stop_workers()` runs; STOP
   must have been dispatched exactly once *before* notification.
3. The legacy `group.finalized(ref)` API must not settle DONE
   while a trusted STOP callback is still executing.

The `ATTO_ACTOR_RETIREMENT_HOST_ONLY` SDK-free CMake branch now
requires **three** production CTests: actor resource registry,
retirement barrier, and group lifecycle. The exact checked-in host
script SHA `044431747fd10ff6e7f67935f82f6cd270f93b45`
was reconstructed locally, verified with `git hash-object`, and
passed `bash -n`. Retirement and group lifecycle run with
Clang/GCC ASan/UBSan **and** leak detection. The resource registry
test keeps ASan/UBSan, but has LeakSanitizer disabled because its
negative tests intentionally retain unsafe failed-callback captures
until independently verified isolate shutdown.

**Evidence boundary:** The new full group/registry/retirement CTests
are written but still **NOT_EXECUTED** on the exact PR checkout.
The source Actions jobs do not report executable steps. The isolated
byte-exact source receive gate passed 2,000 two-carrier STOP/admission
races under each of Clang 17 and GCC 14, ASan/UBSan; the Java source
gate passed independent strict-JDK21 checks. Neither result proves
that the complete new C++ group header compiles or that an actor
emitted by the AttoLang compiler runs concurrently.
