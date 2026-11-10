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
      -> Unknown: quarantine, reconcile until definite provider outcome
      -> Cancel/device loss/actor teardown: halt admission but retain in-flight buffers
      -> Terminal: explicitly retry physical provider release
      -> Released: recycle bounded registry slot

A thrown enqueue or malformed receipt means Unknown, **not** proof of no submission. A forged event, another actor generation, or an earlier turn cannot publish state. Device loss affects matching tenant/device/provider generation; other tenants' actors can complete independently. Timeouts do not imply device quiescence.

The registry retains the V0 host ledger, provider and shared supervisor CONTROL lane when the actor-facing wrapper disappears. Admission is capacity bounded, so no overcommit occurs when device loss prevents physical release.

**Remaining lifecycle gap:** This protects actor-wrapper destruction while its supervisor-owned registry remains alive. V0 still calls std::terminate on unresolved ledger destruction, so deletion of the *entire registry* with outstanding entries is not an isolated recovery path. A durable or process-root quarantine owner that survives supervisor restart must be implemented and tested before production enablement. The registry cannot be safely discarded on a timer.

## Verified native host-only evidence

Private token-free exact Git-blob carrier: https://github.com/scintilla-run/scintilla-run-e2e/actions/runs/42920512073

- GCC with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer: executed and passed
- Clang with strict warnings: executed and passed
- 6,268 host checks on each compiler; 1,024 logical actors, 4,096 mock-provider turns
- Event forgery/replay, malformed receipts, definitive enqueue abort, ambiguous exceptions, cancellation during enqueue, cohort loss, cross-tenant isolation, capacity exhaustion and graceful retirement
- No real GPU, GPU driver event, compiled AttoLang guest, physical GPU kernel, or five-backend parity verified

## Next production gates

1. Implement trusted physical provider adapter with native GPU queue, allocation generations and exact event authority.
2. Make quarantines resilient to supervisor-of-supervisor restarts, with explicit release and device-generation recovery.
3. Complete Java and LLVM GPU actor typed body MIR, effects, borrow rules and guest lowering.
4. Execute real device run with hardware/driver identity, artifact hashes, successful device event, independent CPU oracle and supervision validation.
5. Independently verify unsupported backends fail closed.
