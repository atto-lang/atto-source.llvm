# Host-owned physical STOP evidence — native restricted supervisor slice

**Status:** implemented and verified for **process-local native physical actor
termination**. Not a general guest STOP proof and not a persistent or
cryptographically authenticated external-host receipt.

## Trust and authority boundaries

The native `NativePhysicalStopEvidence` ledger owns a strong handle to the
exact supervisor child registry and native C actor scheduler. It records a
one-use, immutable `Attestation` bound to all of:

- issuing supervisor ID and scheduler runtime domain
- logical child ID, failed physical actor slot and scheduler generation
- logical child incarnation and exact CONTROL turn generation
- host-issued monotonic in-process evidence sequence

A caller cannot obtain valid evidence merely by constructing a
`SupervisorChildRegistry::StopReceipt` with `host_invoked=true`: the
ledger rereads the authoritative registry's outstanding STOP reservation and
checks the actual scheduler `poll_done(actor)` barrier before issuance.
Scheduler slot **0 is valid**; domain and generation are required instead.

Reconciliation is **read-only with respect to the physical host action**:
`confirms_physical_done` checks the same registry-bound reservation, finds
an issued attestation, and independently rechecks the physical scheduler's
DONE state. `reconcile_uncertain_stop` then revalidates the original
reservation atomically before transitioning logical STOPPING to STOPPED.
An unknown STOP without issued host evidence stays quarantined. There is no
automatic physical STOP retry.

This ledger attests **physical actor completion**. It does not prove an
external API call, database transaction, or unrelated host sink side effect.
For such effects, separate authoritative, durable, idempotency-bound records
are still required. These in-memory receipts cannot survive host-process
restart and must never be advertised as durable/externally authenticated
receipts. Keep general guest RESTART gated.

## Executed native CI evidence

[Funded Actions run #38200268818](https://github.com/zed-pkg-test/java-app/actions/runs/38200268818)
**PASSED** with **18 exact-source Git blob checks** and GCC
`-fsanitize=address,undefined` for the real native C actor scheduler,
`Parser.cpp`, `OwnershipChecker.cpp`, native bridge and test programs.

- A restricted compiled `.ores` `on_panic(f: Failure)` /
  `self.stop(f.child)` program received a real typed scheduler PANIC.
- Successful STOP was acknowledged once.
- An uncertain STOP without physical host evidence remained STOPPING.
- A separately issued DONE attestation allowed a LOST host ACK to be
  reconciled without invoking the host callback a second time.
- Forged logical child, CONTROL turn and physical generation, and duplicate
  physical receipts were rejected.
- The independent regression deliberately marked a **still-running** native
  actor FINALIZED in the trusted registry. The ledger rejected attestation
  until the actual scheduler reached DONE, then permitted exactly one.
- Native scheduler slot 0 was exercised and accepted as a valid actor address.

The standalone regression is registered as
`native_physical_stop_evidence` in CMake/CTest. The full LLVM CMake/CTest
suite has **not** been executed by this funded source-subset verification.

## Remaining release gates

1. Host-effect receipts with durable, crash-recoverable identity, idempotency,
   and authenticated external side-effect completion, distinct from physical
   scheduler DONE.
2. Complete, causally ordered **actual runtime** schema-v2 event emission and
   verification, not reconstructed/synthetic fixture traces.
3. Java/GraalVM `ActorRuntime` physical lifecycle parity and full Java 25
   Maven + Graal Native Image suites.
4. Full LLVM native compiler CMake/CTest and concurrency/fault-injection
   test matrix at exact PR head.
5. Cross-backend compiled unrestricted source `on_panic`, RESTART, await
   and escalation conformance with TLC/implementation trace refinement.

All three supervisor PRs stay draft until their respective gates are green.
