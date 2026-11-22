## Actor-owner identity correction (October 25)

`NativeSourceCleanupBinding` now authenticates `NativeSourceCleanupTicket.actor_id`
against `OresNativeAllocationDomainRef.owner` (the actor allocation owner/slot),
not against `allocation.domain` (the scheduler/runtime allocation domain).
It also requires the claimed execution's `runtime_domain` to equal the
allocation domain.

This closes two inverse failures: legitimate actors whose scheduler domain is
numerically different from their actor id no longer fail cleanup admission, and
a continuation owned by actor B can no longer authenticate actor A merely
because its scheduler domain happens to equal A's numeric actor id. The
continuation handle, allocation generation, code generation and source token
remain independently checked. This is still host-side substrate; it does not
remove the existing requirement for scheduler-owned carrier admission and
source lowering.

## Positive cleanup transition access hardened (October 23)

Normal production translation units can no longer call
`NativeSourceCleanupGate::try_claim_cleanup`, `yield_cleanup` or
`complete_cleanup` directly: these methods are private, with access
restricted to the `NativeSourceCleanupBinding` implementation that validates
a live, claimed continuation record. Existing focused gate/retirement tests
use a build-target-only `ORESLANG_NATIVE_SOURCE_CLEANUP_TEST_PEER` friend
(never defined for the production runtime or binding CTest).

This is **C++ API exposure hardening**, not a complete authority boundary.
A trusted host still constructs bindings, and the store snapshot-to-CAS
transition does not yet own an indivisible actor carrier lease. Normal
completion and fail-closed cancellation/reporting remain host-level APIs.
Do not allow guest code or untrusted FFI to define the test-peer macro,
construct host bindings or manipulate the native continuation store. Actual
scheduler-owned exclusive admission and source-to-native lowering remain
mandatory release gates.

## Normal-completion admission hardening (October 23)

The native cleanup gate's normal positive completion method is now private,
available to production only through
`NativeSourceCleanupBinding::complete_normally`. That binding rejects any
unclaimed, stale-generation or foreign actor continuation before the
pending-to-completed CAS. The test-only peer retains gate transition access
for adversarial state-machine tests.

**Executed locally against exact committed gate/binding/test Git blobs:**
Clang 17, GCC 14, and Clang 17 ASan+UBSan passed the 1,600-race-pair gate
test; a separate mocked-C-store binding smoke passed under the same
configurations. A production-mode negative compile confirmed direct
`gate.complete_normally(ticket)` fails due to private member access.

**Not yet proven:** real C continuation-store CTest, atomic admission with
the scheduler's physical single-writer lease, unforgeable native runtime
authority, source-to-native compiler lowering or GraalVM parity. The host
must still prove lexical frame and all FFI/loan teardown before calling
`complete_normally`; binding validation alone cannot prove those events.

# Native source-cleanup receipt v1 (host prerequisite; not guest lowering)

Java/Graal PR [#1887](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1887)
separates a cancellation request from an authenticated, physically completed
SourceTask cleanup. Native LLVM must preserve the same distinction.

## New bounded host substrate

`NativeSourceCleanupGate` is an allocation-free atomic state machine. A
`NativeSourceCleanupTicket` binds actor ID, actor incarnation, and continuation
ID to prevent stale or cross-incarnation control operations.

| State | Legal transitions | Physical actor reclaim? |
| --- | --- | --- |
| pending | complete_normally or request_cancel | no |
| cancel_requested | try_claim_cleanup or fail_cleanup | no |
| cleanup_running | yield_cleanup, complete_cleanup or fail_cleanup | no |
| cleanup_yielded | try_claim_cleanup or fail_cleanup | no |
| completed | none | yes, subject to remaining registry/finalizer barriers |
| cleanup_incomplete | none | **never** |

`request_cancel` cannot publish completed DONE. A cleanup turn may yield
to `await` and then be claimed again by the scheduler. Scheduler shutdown,
exhaustion, missing cleanup carrier or fatal cleanup raises
`cleanup_incomplete`; completing it retroactively is forbidden. The caller
must not invoke `complete_cleanup` until the compiler/runtime has actually
run LIFO defers/finally, closed iterators exactly once, retired lexical
mutex guards/loans and reconciled FFI/I/O pins. **These are obligations, not
yet independently verified by this state machine.**

`NativeActorRetirementBarrier` accepts an *optional* shared cleanup gate.
With one installed, resource closers and `get_done()` are withheld until
the gate reports completion AND the existing group execution finalizer,
loans, callbacks and resource registry are safe. An incomplete gate
reports `Poll::cleanup_incomplete` without publishing DONE or invoking
any allocation closer. The barrier intentionally retains the registry;
its fail-closed emergency-retention destructor is not a substitute for
supervised, bounded quarantine and isolate teardown.

## Native tests added

- Exactly one winner for concurrent normal-completion versus cancellation
  (800 adversarial pairings per CTest invocation).
- Stale actor/continuation ticket rejection and duplicate claim denial.
- Three successive cleanup yield/reclaim cycles before physical completion.
- Another 800 cleanup-failure-versus-completion adversarial pairings ensure\n  exactly one terminal result; failure cannot be rewritten as success.\n- Fatal/incomplete cleanup can never be converted into a completed receipt.
- Actual actor resource-registry and ActorGroup `get_done()` integration
  refuses reclamation on cancel request or yielded cleanup and admits it
  after an explicit completed receipt.
- Incomplete cleanup forbids native arena release and falsely successful DONE.

## Focused executable evidence (not a full backend build)

On the October 22 feature head following the semantic `main` merge, the
**exact Git blobs** for `NativeSourceCleanupGate.hpp`
(`7d70ac63c0bc883f7301414c84e4fa0fd6659da7`) and
`native_source_cleanup_gate_test.cpp`
(`0df2f5616169afd3594ae78e635f9e9043e77b25`) were materialized
locally and independently verified with `git hash-object`. Compilation
and executable tests passed with Clang 17 / GCC 14 (`-std=c++17 -Werror`);
Clang 17 ASan+UBSan passed with leak detection and halt-on-error.
The harness exercises **1,600 racing pairs per run**, plus stale-ticket,
duplicate-claim, failure-terminal and multi-turn yield invariants.

These receipts do **not** validate full repository CMake, integrated
`NativeActorRetirementBarrier`, generated LLVM IR, executed `.ores`
programs or GraalVM JDK25. The source GitHub Actions runs still show
`runner_id=0` and `steps=[]`; source-org administrator admission is
a prerequisite for full exact-head tests.

## Store-backed continuation admission (October 23)

`NativeSourceCleanupBinding` now checks a **live, CLAIMED** generation-fenced
`NativeContinuationStore` record before allowing its wrapper to advance the
cleanup gate. The binding verifies the packed slot+generation handle, actor
runtime domain, actor arena owner and incarnation, code generation and source
token. A dedicated `native_source_cleanup_continuation_binding` CTest
exercises the **real C continuation store**, rejection before resume-claim,
wrong actor/incarnation, wrong source generation/token, foreign store handles,
and revoked or released continuation handles.

This is an **additional verification wrapper**, not yet non-bypassable
scheduler authority: `NativeSourceCleanupGate` still exposes its host
transition methods directly, and store `get` plus a subsequent gate CAS is not
atomic with relinquishing the physical actor carrier lease. The runtime must
route all guest cleanup continuation admission through the trusted actor
CONTROL scheduler, remove direct/foreign paths, pin the continuation-store
lifetime and prove same-actor single-flight across each yield/re-entry.
Unsupported lowering and guest `defer`/`finally` remain fail-closed.

The exact committed binding header blob
`ae9f36d2d4783cc330f620ec65c7cb89386be493` was reproduced locally,
verified by `git hash-object`, and compiled/executed in a **mock-C-store**
smoke with Clang 17, GCC 14 and Clang ASan+UBSan. This is syntax and adapter
logic evidence; the new CTest linked against real C runtime sources has
**not** executed because source GitHub Actions still do not assign runners.

Shared draft source-conformance fixtures live at
[platform-conformance PR #159](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/159)
and expressly do not claim real backend execution.

## Not yet implemented; hard merge gates

1. **LLVM frontend/compiler lowering:** Native source `Task`, `await`,
   `defer`, `finally`, `recover` and iterator close must be lowered into
   heap-owned continuation frames and explicit verified cleanup turns.
   Current unsupported source forms must continue to fail before emitting IR.
2. **Trusted continuation authority:** Connect receipts to actual
   `NativeContinuationStore` generations, `NativeActorScheduler` owner
   execution lanes, supervisor CONTROL stop handling and actor incarnation
   fencing; guest code must not call these host APIs directly.
3. **Physical admission after STOP:** Provide a bounded *cleanup-only* carrier
   path after normal actor admission shuts, with strict single-flight guest
   execution and no guest catch/trap interception of cancellation/panic.
4. **Quarantine/FFI:** Broker ACK, live borrow/loan, callbacks, child Tasks,
   native arena and native destruction must join before a positive receipt;
   isolate-level verified shutdown must handle cleanup failures without UAF.
5. **Executable differential tests:** Compile actual positive/negative
   `.ores` fixtures on Java/Graal and LLVM with identical task/fault behavior,
   including cancellation-before-start, cancellation at await, cleanup
   re-suspension, close-once, panic dominance and actor-stop races.
6. **Exact-head execution:** Full native CMake/CTest, GCC/Clang,
   ASan/UBSan/TSan (where supported), compiled guest LLVM IR/object/native
   binaries and GraalVM JDK25 + Native Image on permitted runners. Zero-step
   Actions failures are runner evidence only, not test evidence.

No private-source cross-org Git mirror is permitted until reviewed policy
and destination authorization allow it. This change stays DRAFT.
