# GPU actor provider bridge v1 — host authority and bounded quarantine

**Status: executable native host-control prototype, not real GPU actor execution.**

This patch stacks on draft #1650's GpuActorHostTurnBridgeV0. It does not enable GPU actor syntax, implement typed body MIR, allocate GPU memory or run a kernel. Mock-provider success must never be presented as physical GPU evidence.

## Ownership boundary

ProviderPortV1 is a **trusted native backend interface**. It issues an opaque, non-copyable event object bound to provider instance identity, actor identifier/generation, tenant, device/generation, queue/generation, allocation generation, exact actor turn and provider-local serial. GpuActorProviderBridgeV1 does not expose public completion booleans, event injection or buffer release acknowledgments. The registry checks locally minted event provenance before calling internal V0 host methods, rejects foreign events and stale-turn replay, and retains the trusted provider while events remain unresolved.

**This is local host capability validation, not independent physical device-event authentication.** A malicious or defective native provider can still mint an event or falsely assert hardware completion. A hardware adapter must verify its actual native GPU event, queue, and buffer identities before reporting completion. The original V0 Boolean methods also remain available in native code; production code review must prevent V0 bypasses and deny all guest access to these privileged entrypoints.

## Submission and recovery state machine

    Ready
      -> Prepared with checked staged state and exclusive actor turn
      -> Begin enqueue window
      -> Submitted(event) | DefinitelyNotSubmitted | Unknown
      -> Submitted: pending until a matching provider-reported terminal event
         -> success: publish staged state exactly once
         -> failure: supervisor CONTROL without state commit
      -> Unknown: quarantine, single-owner reconciliation until definite outcome
      -> Cancel/device loss/actor teardown: halt admission but retain in-flight buffers
      -> Terminal -> Releasing: exclusive provider release; no overlapping retries
      -> Released: recycle bounded registry slot

Graceful retirement first reserves a Retiring phase before dropping the native lock to request provider release. Concurrent actor submission is rejected; cancellation racing with retirement consumes the original provider release result (no second release). Terminal cleanup likewise reserves Releasing before dropping its lock; two recovery pollers cannot release the same physical allocation twice. Unknown-enqueue reconciliation uses a Reconciling phase to prevent simultaneous provider reconciliation calls.

A thrown enqueue or malformed receipt means Unknown, **not** proof of no submission. A forged event, another actor generation, or an earlier turn cannot publish state. Device loss affects matching tenant/device/provider generation; other tenants' actors can complete independently. Timeouts do not imply device quiescence.

The registry retains the V0 host ledger, provider and shared supervisor CONTROL lane when the actor-facing wrapper disappears. A native process-root anchor also pins any nonempty registry across loss of its immediate supervisor handle. The root is bounded to 256 registries; each registry has its own capacity gate. Native recovery calls progress_process_root() and automatically unpins empty registries. No arbitrary guest may access the root.

**Remaining lifecycle gap:** The process-root anchor prevents a normal in-process supervisor-handle drop from destroying unresolved V0 ledgers. However it is NOT process-crash durable and cannot prove an actual GPU device quiesced after driver loss. A permanently unresponsive driver can retain physical resources until trusted recovery; global root capacity bounds the number of separate retained registries but does not implement durable recovery or reclaim device memory. This is not yet an isolated whole-process or supervisor-tree restart contract.

## Verified native host-only evidence

Private token-free exact Git-blob carrier: https://github.com/scintilla-run/scintilla-run-e2e/actions/runs/42925139986

- GCC with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer: executed and passed
- Clang with strict warnings: executed and passed
- 6,329 host checks on each compiler; 1,024 logical actors, 4,096 mock-provider turns; verified process-root retention and recovery after supervisor-handle loss
- Event forgery/replay, malformed initial/reconciled receipts, definitive enqueue abort, ambiguous exceptions, cancellation during enqueue, cohort loss, cross-tenant isolation, capacity exhaustion, graceful retirement, overlapping release/reconciliation threads, release exceptions, and failed-admission root cleanup
- No real GPU, GPU driver event, compiled AttoLang guest, physical GPU kernel, or five-backend parity verified

## Next production gates

1. Implement trusted physical provider adapter with native GPU queue, allocation generations and exact event authority.
2. Add durable crash recovery and supervisor-of-supervisor registry handoff beyond this process-root-only retention, with explicit release and device-generation recovery.
3. Complete Java and LLVM GPU actor typed body MIR, effects, borrow rules and guest lowering.
4. Execute real device run with hardware/driver identity, artifact hashes, successful device event, independent CPU oracle and supervision validation.
5. Independently verify unsupported backends fail closed.

## 2026-11-10 independent lifecycle audit

The audit identified and corrected: a duplicate terminal provider release race, cancellation racing with graceful release, simultaneous ambiguous-enqueue reconciliation, provider-error recovery without quarantine escalation, and an exception-safety exposure when process-root retention preceded registry placeholder allocation. Both strict native compilers executed 6,329 host assertions on pinned source.

**Unresolved trust boundaries:** native provider callbacks remain privileged and can forge completion internally; V0 still exposes native Boolean completion paths; no physical device event or provider driver has been attested; process-root ownership is not crash durable and unresolved work can consume its bounded capacity; host actor tests do not prove the full compiler MIR or actor scheduler integration. This PR must remain draft, and none of the five backend GPU-guest statuses may be promoted.
