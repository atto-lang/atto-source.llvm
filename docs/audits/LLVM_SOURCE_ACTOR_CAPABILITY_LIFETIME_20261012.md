# LLVM source actor handle lifetime audit — 2026-10-12

Status: **focused implementation and funded CI VERIFIED** on draft PR #239,
which stacks on draft PR #180. Do not treat this as a production release
certification or merge authorization.

## Confirmed vulnerability

The previous `void*` public ABI exposed the heap address of `SourceActor`.
`send_wire`, `send_variant`, `await_ready`, `pipeline_connect`,
`expect_status` and `stop` dereferenced it *before* validating the live
registry identity. The producer gate only protected callers already admitted
before stop closed the gate; it did not protect a late caller from dereferencing
reclaimed actor memory, nor prevent address-reuse ABA.

## Repair

- Spawn returns a monotonically issued 64-bit public identity encoded in an
  opaque `void*` value, not the address of `SourceActor`. This ABI requires
  a supported native target with at least 64-bit `uintptr_t`. The integer
  identity is never reused across scheduler open/close or actor-slot recycling.
- Public operations resolve identity under `group.mutex` by comparing the
  supplied token with live registered actor IDs. Unknown/stale/retired tokens
  never become dereferenceable actor pointers.
- Typed send obtains a producer lease while holding the same registry lock
  that stop uses to close admission. After releasing the lock, the lease
  guarantees actor lifetime through detached snapshot validation and
  nonblocking bounded DATA publication. Stop waits for leases before releasing
  schemas and destroying the actor.
- Scalar sends keep the registry lock through nonblocking scheduler admission.
  `await_ready` preserves a public token when invoking a compiled CONTROL
  continuation rather than leaking the internal `SourceActor*`.
- Pipeline registration resolves every borrowed guest ref under the registry
  lock; status and cancellation bind to that same identity. Cancellation
  publishes its terminal state under the registration barrier.
- Internal scheduler callbacks still use live actor pointers within their
  physical turn lifetime; they never publish those pointers to guest code.

## Executed regression and formal evidence

1. [Exact-source funded CI #38215345913](https://github.com/zed-pkg-test/java-app/actions/runs/38215345913):
   11 identical source Git blobs, GCC strict `-Werror`, 30 native repetitions,
   15 Clang ASan/UBSan repetitions. The fixture checks stale send after stop,
   4 live producer threads racing stop, and first/second retired handle
   rejection after successive actor-slot reuse.
2. [Bounded TLC + native CI #38215738454](https://github.com/zed-pkg-test/java-app/actions/runs/38215738454):
   82 distinct reachable lifecycle states, no invariant violations; a deliberate
   stale-slot-alias mutant produces the expected `NoStaleAdmission`
   counterexample. The finite model checks the registry/lease protocol, not
   the C memory model.
3. [Guest compiler + native execution CI #38216044075](https://github.com/zed-pkg-test/java-app/actions/runs/38216044075):
   exact source blobs of LLVM `Parser`, `LlvmCodegen` and `main` verified;
   real `.ores` guest programs compiled to LLVM IR and native executables.
   Signed-i64 fixture delivered 9 messages with final state `-4294999972`;
   three-actor `self.forward`/`self.backward` round-trip produced typed
   result `19`. Ten native repetitions each. The same workflow reran strict
   native/sanitizer and TLC gates.

## Security boundaries and remaining release gates

- These handles protect against **lifetime ABA** and post-free dereference.
  Monotonic public IDs are **not cryptographic secrets or general authorization
  capabilities**. The restricted compiler must not permit untrusted numeric
  pointer casts or attacker-controlled arbitrary `void*` calls. Runtime
  authentication and cross-context authority binding remain separate work.
- The bootstrap still uses fatal trusted-main checks for certain invalid
  calls. Typed send uses recoverable `0` for stale IDs; an invalid trusted
  stop or scalar API can still terminate the process.
- The new formal model is bounded and is not an implementation-refinement proof.
  No claim covers arbitrary `Future<T>`, suspended cancellation, multiple
  in-flight pipeline flows, dynamic fan-out/fan-in, or cross-backend general
  semantics.
- The full original LLVM CMake/CTest suite, Java/Graal AOT and hybrid,
  source-level differential Java/LLVM regression matrix, and parent-stack
  integration review remain outstanding.

**Conclusion:** generation-safe public-identity lookup and typed-send
reclamation fencing passed focused exact-source native, sanitizer, guest,
and bounded formal gates. Keep PR #239 and parent #180 draft.
