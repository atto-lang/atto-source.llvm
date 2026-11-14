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
