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
