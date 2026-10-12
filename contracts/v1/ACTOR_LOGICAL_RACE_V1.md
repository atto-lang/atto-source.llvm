# Logical races across actor suspension — v1 contract

Status: **runtime foundation only**. This contract is shared verbatim between the Java/GraalVM and LLVM repositories. The host-side `VersionedActorCell` primitive is implemented in both; it is **not** yet an Oreslang guest-language intrinsic, compiler effect pass, or production actor-state integration.

## Safety distinction

- **Data-race freedom** protects memory. It does not prove that a value computed before `await` is still valid afterward.
- **Serial actor:** the scheduler keeps the *logical* actor turn admitted across awaits (including all completion barriers), while releasing the physical worker. Mutations are serialized by that real runtime lane, not a guest assertion.
- **Reentrant actor:** other actor turns may run while one awaits. A snapshot-derived mutation must be checked against the current generation and revision at its single atomic commit point, or carried out through another verified atomic state-transition protocol.
- **State loan:** an exclusive `handle_state` loan cannot cross an await, cooperative yield, escaping closure, cancellation, or actor teardown. An immutable *copy* may cross an await without carrying the loan or scheduling capability.

## Scalar optimistic-commit primitive

`VersionedActorCell` stores one signed 64-bit scalar, a revision 0..2^63-1, and a terminal retired flag. `snapshot()` captures the scalar and an unforgeable host-owned cell identity/revision. `tryCommit(snapshot, replacement)` is a **single** atomic state transition, with results:

- `COMMITTED`: the snapshot's origin matches, cell is live, and current revision is unchanged; replace the scalar and advance the revision.
- `CONFLICT`: a different successful commit advanced the revision, including when it wrote the previous value back (ABA).
- `FOREIGN`: the snapshot belongs to another actor state incarnation/cell.
- `RETIRED`: actor state has been permanently retired; in-flight await results can never resurrect it.
- `EXHAUSTED`: 63-bit revision is at its maximum. Do **not** wrap, reset, or retry under the old incarnation.

`snapshot()` after retirement rejects. Both implementations have thread-safe linearization of snapshot, commit and retirement. The Java implementation uses a CAS over the complete immutable scalar state; native C++17 uses a short mutex-protected transition because standard C++17 cannot atomically CAS a portable pair of 64-bit fields. Neither retains a native mutex/OS worker across guest code, `await`, or I/O. No guest expression is re-evaluated inside automatic CAS retries.

Tokens/snapshots are runtime-local and must not be serialized or treated as ActorGroup/actor-admission capabilities. Each actor incarnation receives a fresh cell. The snapshot retains a reference to its original cell to prevent identity/address ABA. The actor lifecycle must retire the cell before reusing state storage.

## Language-level API (PROPOSED, not implemented)

The anticipated lowering looks like:

```csharp
// The critical callback ends and its state loan retires before awaiting.
val snapshot = self.handle_state(critical |s| -> s.snapshot_scalar("balance"))
val approved = await authorize_payment()
val outcome = self.handle_state(critical |s| ->
    s.try_commit_scalar(snapshot, snapshot.value - approved.amount))
```

These method names and source syntax are illustrative and currently **must not be claimed to execute** on either compiler. The compiler/runtime integration must expose *only* verified actor state capability operations, not a freely constructible `VersionedActorCell` or mutable guest snapshot token.

## Proposed compile-time actor reentrancy effect

A later checker should track `actor.state.read`, `actor.state.write`, `suspend`, live state loans, actor-wide serial admission, and optimistic-version evidence through CFG joins and transitive calls:

1. Always reject `suspend` while an actor-owned state loan is live (including nested closures, `finally`, `select`, exception/cancellation exits).
2. Permit copied immutable snapshots across suspension; do not permit live state references or borrowed mutable aliases.
3. A reentrant handler that uses a pre-suspension state-derived value to mutate actor state after resumption must either use a single atomic checked commit/transaction or be rejected with a stale-state diagnostic. Merely acquiring another lock after the await does not prove the prior read is current.
4. Recognize a serial actor exception **only** when the trusted runtime proves actor-wide single-flight admission across HTTP/mailbox/other ingress and retains it across all awaits. An annotation alone must not waive the rule.
5. Reject stale continuation generations and cancellation/teardown races before acquiring a fresh state loan. Never replay a guest callback automatically to resolve version conflict.
6. Multi-field invariants require a single versioned aggregate transaction. The scalar prototype does **not** certify arbitrary structs/Maps, remote I/O transactions, distributed ordering, or exactly-once external side effects.

## Verification and release gates

The first slice includes deterministic stale/foreign/ABA/retired/exhaustion and 32-way concurrent single-winner tests. Required future gates include native ASan/UBSan and Release builds (assertions may be compiled out), Java Maven/JUnit and native CTest on exact source, actor lifecycle/cancellation and scheduling integration, real `await` guest fixtures for both compilers, aggregate state CAS, type/effect checker verification, and native-image/AOT. **Do not merge or claim compiler-enforced logical-race prevention based on this runtime-only slice.**
