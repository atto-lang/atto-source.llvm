# Native LLVM source actor: generation-safe identity v1

**Draft, not verified by native CI.** Implements issue #1802 for new
compiler-emitted source ActorRefs. Production `SourceActorBootstrap.c` and
the LLVM compiler are independent of other Atto backends.

## Identity invariant

The compiler now models its opaque `ActorRef` as a **process-monotonic,
nonzero 64-bit actor ID** passed as an integer by value through the
compiler/runtime ABI. **It is not a pointer cast to 64 bits.** Every actor
spawn mints the next ID under `group.mutex` and fails stop before the
counter wraps. The counter NEVER resets when the native VM/scheduler group
closes or reopens.

All checked public host entry points resolve ID to the **currently live
registered actor** under `group.mutex`. In particular:

- `spawn_id` produces a new actor identity, distinct from any retired
  identity even after malloc address or scheduler slot reuse.
- `ready_status_id` returns -1 for missing, invalid and retired identity,
  without dereferencing any stale allocation.
- `try_send_id` and `try_request_stop_id` return false on invalid or
  revoked identity, without affecting the current incarnation.
- `send_id` and `request_stop_id` fail stop if the checked operation is
  not admitted; their v1 source wrappers do not silently drop messages.
- `join_done_id` claims **exclusive physical teardown** under the group
  mutex before leaving it to wait for DONE. The claimant alone may free
  the heap and allocation. `stop_id` is stop-then-join.
- `await_ready_id` and `await_done_id` preserve the original actor ID
  through the existing root CONTROL logical suspension/Future protocol.
  Root continuations call a genuinely typed `int64_t fn(uint64_t actor_id)`
  callback instead of casting the legacy `int64_t fn(void* actor)`.
  DONE reserves join authority before suspension, and physical teardown
  waits for admitted root observers to drain.

The above replaces allocator-address authority on **all compiler-emitted
source actor paths** in this branch: spawn, send, stop, READY and DONE.
The original pointer ABI remains for historical C host tests and explicit
compatibility; it **still has malloc-address ABA** and must be audited,
deprecated or removed before making general untrusted actor/FFI claims.

This is a trusted compiler/runtime ABI, **not a cryptographically unforgeable
bearer token**. An actor ID must not be accepted from attacker-controlled
native FFI inputs without separate capability authentication and
permissions checks. The ID is not a guest integer and cannot be returned or
sent as an ordinary scalar in supported Atto source. This backend work
does not imply parity for Java, .NET, WASM, Rust or Forge.

## Deterministic ABA verification contract

`tests/source_actor_checked_identity_aba_v1.c` links the actual
production SourceActorBootstrap, native scheduler, VM and actor-owned heap.
A test-only, one-entry allocator freelist returns **exactly the same
SourceActor allocation address** on the next spawn. That means malloc
address and VM actor slot both repeat, while the minted 64-bit actor ID
is required to change. The freelist is removed from ordinary production
builds and never retains runtime tombstones there.

At Clang O0 **and** O2 with ASan/UBSan:

- 128 real actor lifetimes and 127 forced exact address reuses;
- every old ID must report no live READY, no accepted send and no accepted
  stop, including while the replacement actor is live;
- fail-stop old-ID send, stop and join subprocesses require SIGABRT with an
  exact test-only rejection marker and no sanitizer errors;
- real heap destruction and VM group teardown must produce 128 physical
  lifecycle receipts per level, not synthetic test-main success strings.

`tests/fixtures/source-actor-checked-id-v1/` adds `.atto` guest
fixtures for single-actor send/stop, five actors, checked get_ready
continuation, checked get_done continuation, and negative ActorRef escape.
The exact-head O0/O2 compiler suite requires actual LLVM calls to the
checked-ID runtime symbols, rejects all legacy pointer ABI callsites,
screens emitted IR for known GC constructs, compiles native ELF, and
executes each positive guest four times. A successful outcome would be
**32 real native guest executions** with no source-level fallback.

## Security and CI boundaries

- The exact-head runner must match source commit/tree and input blob SHA,
  and must not publish source-bearing artifacts to another organization.
- This is not linked-image collector proof, hard-real-time WCET proof, or
  proof of native FFI unforgeability. Existing formal actor borrow/await
  checks remain independent obligations.
- The source organization's Actions jobs have been failing **before
  executing any steps**. A source-free canary on an alternate private org
  previously succeeded, but it does not constitute a build, sanitizer
  pass or private-source transfer authorization.
- `main` protection and the private carrier governance contract still
  prohibit copying private LLVM source to an alternate organization until
  independently reviewed protected policy and incident gates succeed.
  Nothing in this PR authorizes mirroring.

**Release hold:** retain draft; require actual compiler build,
ASan/UBSan O0/O2, native `.atto` execution, independent review, and
the linked artifact/ABI security gate before merge. Coordinate with
bounded actor loop PR #1789 after independent review; neither PR should
overwrite the other's runtime work.
