# Native actor-resource registry — audit and test boundary

Status: **trusted host substrate, not source-level lifecycle implementation**.

The C++17 `ActorResourceRegistry` on PR #178 provides per-incarnation,
monotonic LIVE → RETIRING → CLOSING → RECLAIMED tracking and a bounded
reverse-ordered trusted resource-close list. Three types of activity ticket
(turn, loan, FFI callback) must all quiesce before cleanup starts; foreign,
wrong-role, duplicate and stale ticket releases fail closed. A concurrent
reclaimer observes CLOSING and never invokes closers again.

## 2026-10-11 hardening

- Cap per-activity outstanding tickets at 4,096 and registrations at the
  configured capacity (maximum 4,096).
- Limit retained failure diagnostics to 16 entries and 256 bytes each.
  Keep the **total failure count** even if optional diagnostic allocation
  fails. Never abandon later closers because an earlier one threw.
- Move the final Result into stored state and commit RECLAIMED before
  returning it, preventing error-report allocation from trapping the registry
  in CLOSING.
- Stress 32 concurrent retiring incarnations; during an in-flight close,
  concurrent reclaimers see CLOSING. Exactly one callback runs.
- Reject foreign actor-incarnation and activity-role tickets, duplicate
  releases, and attempts to acquire new work after retirement.

Focused **exact Git-blob** C++17 source/testing files were built and
executed locally:

```sh
g++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
  -pthread -I src/main/cpp tests/actor_resource_registry_test.cpp -o registry-release
./registry-release

g++ -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -pthread -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I src/main/cpp tests/actor_resource_registry_test.cpp -o registry-san
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1 ./registry-san
```

Both standalone runs passed. These results **do not** imply that the whole
LLVM source compiler or complete CMake/CTest suite passed; GitHub Actions
on this organization's source repository still fails before starting a step.

## Terminal failure and quarantine (October 12)

A close attempt is not proof that its underlying socket, FFI callback, or
arena was released. The registry now distinguishes `Reclaim::failed` /
`Phase::failed` from `Reclaim::complete` / `Phase::reclaimed`.
`Result.closed` counts successful callbacks only; `Result.attempted`
counts attempts, `Result.failures` records terminal errors, and
`Result.skipped` counts native allocations intentionally left untouched.

Trusted I/O, timer, and FFI revokers run before **all** native allocation
closers regardless of registration order. If a revoker fails, no native arena
closer is admitted. If an allocation closer fails, remaining arena closers
are skipped. Terminally failed registries retain the entire close-entry
vector in `quarantined_`, including its captured RAII owners, to avoid
accidentally releasing native memory by destroying a skipped closure.
`quarantined_resources()` exposes this retained count for diagnostics.

**Integration requirement:** A supervisor must retain ownership of a
terminally failed registry until an independently safe reclamation or
quarantine strategy is established. Destroying that registry can release
its callback captures; this component cannot itself guarantee process-
lifetime quarantine. Never mark `actor.done` successful or physically
reclaim an actor arena based on a failed result.

The exact current Git header and regression test were compiled and executed
with C++17 strict Release `-Werror`, and ASan/UBSan/LeakSanitizer. Tests cover
fail-closed status, callback-before-arena ordering, skipped native destructors,
captured RAII retention, duplicate/foreign ticket rejection, and concurrent
exactly-once completion. These targeted tests do **not** establish full LLVM
compiler integration.

## Additional type-safety audit

Activity tickets now have a private constructor and can only be minted
through `acquire_turn`, `acquire_loan` or `acquire_callback` on the
owning trusted registry. Their actor incarnation and activity role are checked
again at release. Compile-time tests reject default construction, aggregate
construction, and public four-field construction. This is host-side type
safety, **not** a cryptographic defense against arbitrary memory corruption.

After these changes, exact Git-source tests passed GCC 14 C++17 Release
(`-Wall -Wextra -Wpedantic -Werror`), GCC ASan/UBSan/LeakSanitizer, and
Clang 17 Release `-Werror`. The full LLVM repository CMake/CTest and native
actor runtime integration have **not** executed.

## ActorGroup `get_done()` quiescence bridge (October 12)

`NativeActorRetirementBarrier.hpp` now connects a *trusted* per-incarnation
`ActorResourceRegistry` to `NativeActorGroup::RuntimeFinalizer`.
It does **not** enable source-level `async tear_down()`. Its host `poll()`
does not run guest callbacks or block a physical carrier.

Before calling *any* resource destructor, `poll()` requires the finalizer's
generation-owned ticket to report both **group detached** and **no active or
reserved user/CONTROL turn**. Registry reclamation separately requires zero
outstanding registry turn, callback and loan tokens. Returning `busy` or
`not_admitted` must never settle `get_done()`.

Successful reclamation is followed by exactly one generation-checked finalizer
acknowledgment. A closer failure is instead delivered as exceptional DONE
only after the exact failed registry is retained by
`NativeActorResourceQuarantine`. If that bounded store is full, DONE remains
pending; no callback capture can be silently released. If the supervisor
itself is destroyed without having independently verified isolate shutdown,
the quarantine deliberately retains its state until process termination,
rather than invoking unsafe native RAII destructors.

Trusted supervisors may call
`release_after_verified_isolate_shutdown()` **only** after independent
proof that the entire native isolation domain and all FFI producers are
permanently stopped. Timeouts, stop requests or an exceptional DONE are not
such proof. The host needs explicit alerting and policy for persistent
quarantine exhaustion and isolate restart.

`tests/native_actor_retirement_barrier_test.cpp` integrates the actual
`ActorOwnedI64Heap.c` with a cross-turn heap loan and ActorGroup DONE
observation. It also covers an admitted guest turn racing group close,
failed FFI revocation, quarantine retention/capacity, and the rule that
group detachment precedes physical reclamation. CMake registers this as
`native_actor_done_resource_quiescence`.

**Important boundary:** The registry still requires trusted scheduler and
resource adapters to mint/release a registry activity ticket for every
actual native loan, callback and turn. A caller that bypasses that tracking
can defeat quiescence. The actor compiler and all concrete native resource
paths are not yet wired to this host bridge. Full CMake/CTest and native
sanitizers on the combined PR tree remain required before any merge.

## October 12 destructor and quarantine-overflow audit

The actor-retirement barrier now treats lost registry ownership as
**non-evidence** of quiescence. Destroying a LIVE, RETIRING or FAILED registry
while it still has registered native resource closures or quarantined captures
must not trigger RAII teardown of memory reachable by an unrevoked callback.
Its fail-closed destructor intentionally retains those captures for process
lifetime rather than introducing a use-after-free. If allocation of the
emergency retention object fails, the process terminates instead of freeing
unsafe ownership. This is an emergency safety fallback, not an ordinary
reclamation strategy: applications must bound quarantine pressure and restart
or retire unhealthy isolates. An emergency-retained registry may consume
memory until process termination.

Verified isolation shutdown uses
`NativeActorResourceQuarantine::release_after_verified_isolate_shutdown()`
to discharge the bounded quarantine without retrying resource closers. The
quarantine is thereafter sealed against new holders. Previously overflowed
registry ownership can be discharged using the trusted
`release_overflow_after_verified_isolate_shutdown()` operation, also without
retrying a closer. Once that proof is available, the barrier may settle the
overflowed actor's `get_done()` **exceptionally** instead of leaving it
pending forever. A failed resource revocation must never produce successful
DONE, including after isolate shutdown.

Regression `native_actor_retirement_barrier_test.cpp` covers premature
overflow discharge rejection, sealed quarantine, verified-discharge terminal
failure and no retry of already attempted FFI closes. These exact new changes
still require executable full CMake/CTest and native sanitizer CI on an
authorized runner.

## Required before integration or merge

1. The supervisor or owning actor runtime must keep the registry alive,
   retire it, and retry `try_reclaim` when quiescence arrives.
2. **Do not** mark an actor's `done` successful solely because `Reclaim::complete`
   is returned: check `Result.failures` and other finalization errors.
3. Native resource closers are trusted, nonblocking and idempotent; arbitrary
   guest callbacks must never be registered. A shutdown deadline does not
   grant permission to free pinned/native memory while a loan survives.
4. Connect the registry to actual socket/FD/FFI registrations, actor-owned
   arenas and compiler-generated field destructors. The registry itself
   does not track arbitrary raw pointers or identify a leaked FFI callback.
5. Implement compiler-lowered source `tear_down`, cancellation/panic
   bypass, and shared Graal/native semantic fixtures before claiming parity.
