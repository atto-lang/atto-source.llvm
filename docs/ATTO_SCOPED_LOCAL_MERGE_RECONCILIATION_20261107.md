# Scoped-local semantic merge reconciliation — 2026-11-07

**Status: in progress, draft, source-only.** This is an audit of what must
survive rebasing LLVM PR #1389 onto the evolving default branch; it is NOT
a successful 3-way Git merge or a compiler parity receipt.

## Anchored comparison

- Observed `main`: `ea5cdf914f2e4cdb32876349ef8e3f062ca3cc5b`.
- Original PR merge base: `040058c1750a049be43949c41f61a87d028e6ddc`.
- PR head at initial conflict inspection: `52e635104fec9209a1353ff7483d3d6f9d7c9e89`.
- Main was 910 commits ahead of merge base; PR was 255 ahead at that read.
- GitHub returned a 300-file main diff (potentially truncated); therefore
  this is a minimum overlap inventory, not proof that other paths are clean.
- At least 11 overlapping paths were identified. Two had identical blobs:
  `NativeExecutionAuthority.h` and merged `ScopedLocalStorageV1.hpp`.
- Nine paths require semantic review and a real 3-way merge:

| Conflict family | Paths | Do not lose |
|---|---|---|
| Compiler | `frontend/Parser.cpp`, `frontend/Parser.hpp`, `backend/LlvmCodegen.cpp` | Main compiler evolution plus branch `.atto` TaskLocal lowering/negative admission |
| Native scheduler | `runtime/NativeActorScheduler.c`, `runtime/NativeActorScheduler.h` | Current main lifecycle/scheduler contracts and draft original DATA-mail, STOP notices, actor-generation fencing |
| Request ownership | `runtime/RequestContextLocalV1.hpp` | Canonical cancellation and ownership plus draft allocation-free commit, quota, quiescent leases and revocation |
| DATA/WAKE bridges | `runtime/NativeRequestDataMailGateV1.hpp`, `runtime/NativeRequestWakeGateV1.hpp` | Exact physical DATA/WAKE provenance, scheduler-aware final commits and deny-on-cancel |
| Build | `CMakeLists.txt` | All main CTests plus draft host checks, adversarial negatives and coexistence gates |

## Semantic decision already committed

**Merged #1391 is canonical for the host-only ActorLocal/TaskLocal/
ThreadLocal substrate.** Its `ScopedLocalStorageV1.hpp` is copied into
the PR with the exact same Git blob SHA
`9b6fa46743091dd4bc8dd90da1f6641633b85fde`, not reimplemented.
`LocalContextDomainsV1.hpp` remains a characterization substrate with
explicit `LegacyActorLocalKeyV1`, `LegacyTaskLocalKeyV1` and
`LegacyThreadLocalKeyV1` aliases. This avoids clashes with canonical
`ActorLocalKeyV1`, `TaskLocalKeyV1` and `ThreadLocalKeyV1` when both
headers are included. Updated existing test fixtures and scheduler bridge
to use the legacy aliases. A new 17-case co-inclusion CTest validates that
both type families coexist and never alias the same owner keys or stores.
Neither host family is a guest-authorized AttoLang capability.

## Derived RequestLocal lifecycle contract

`RequestContextHandoffTableV1` now offers a required-registry construction
path. The recipient context is created via
`NativeRequestLifecycleRegistryV1::open_registered` and MUST be admitted
for that exact actor generation before the context is returned to B.
An unattached/stopping recipient fails closed, retaining no exposed scope.
The backward-compatible unregistered host path now requires the explicit
`UnregisteredHostOnly` opt-in tag and MUST NOT be used by guest lowering.
Copies still cross mailboxes as typed selected DATA, never closures.

A 30-case native scheduler test exercises valid derived-context STOP cleanup,
unattached-recipient rejection, one-shot ticket semantics, parent-cancellation
propagation, and the survival of B's independent copy when A stops. The
optimized workflow includes a negative mutation that replaces
`open_registered` with raw `RequestContextLocalV1::open`; that mutation
must fail the unattached-recipient assertion. All are authored host tests;
no native/sanitizer or compiled `.atto` receipt has been established.

## Unresolved hard gates

1. Preserve every main-side compiler/scheduler evolution in a genuine
   resolved merge; do NOT replace main's whole files with PR copies.
2. **Authored but not executed:** recipient STOP now uses one bounded
   CONTROL-plane lifecycle cleanup hook to revoke all unredeemed envelopes
   for its exact actor generation. A sender+recipient scheduler gate rejects
   new publications if either STOP won first, and a weak-owned table State
   prevents callback use-after-free. Full STOP-vs-seal, cancellation-vs-sweep,
   stale-slot ABA and destructor/lock-order runtime proof is still required.
3. Require issuer-authenticated tenant/isolate/request identity and
   compile-time ownership/Sendable descriptors before exposing guest ABI.
4. Restore source-free runner admission and obtain exact-head GCC/Clang,
   ASan, UBSan, TSan, native CTest and compiler-backed `.atto` receipts.
   `steps=[]`/`steps=null` proves no test was executed.
5. Keep PR #1389 draft until conflicts are resolved, independent review
   counts under GitHub rules and all mandatory CI/security gates pass.

## Handoff STOP and ticket-table hardening (later 2026-11-07 batch)

- Scheduler adds `commit_if_both_live_v1` for a trusted-host,
  allocation-free two-incarnation commit under one STOP lifecycle mutex.
  A recipient that stopped before a sender's final seal may never gain
  a new envelope, even if the sender is still running.
- Lifecycle registry now admits at most 64 CONTROL-plane cleanup callbacks
  before any attached actor can STOP. Its STOP observer remains a single
  lock-free atomic store; handlers execute only on CONTROL, outside registry
  and scheduler mutexes.
- Handoff slots, generations and pending count live in a shared State.
  Registry holds a weak reference, never a raw handoff table pointer.
  A table may be destroyed before a delayed actor STOP without callback UAF.
  STOP hooks detach matching pending envelopes under the State mutex, then
  destroy selected values **outside** that mutex.
- Cancellation `sweep()` snapshots envelope {slot, generation, weak parent},
  checks parent cancellation outside the table lock, then revalidates the
  generation before detaching. This avoids the previous cancellation-graph
  versus table-mutex inversion.
- The handoff suite now authors **70 named host assertions** (40 more than
  the previous 30); a new native C ABI suite authors **16**. Including the
  prior 219 host checks and 17 merged-header coexistence checks, the
  cumulative authored count is **322**. Both STOP-notice removal and
  sender-only ticket admission mutations are configured to fail named cases.
  These are source-level expectations, **not executed CI or guest proof**.
- Runtime semantics remain under validation; cancellation and STOP are
  separate ownership events. Source STOP does not automatically cancel a
  shared request parent or revoke B's already-redeemed independent copy.
  Pending *recipient* tickets are the cleanup boundary addressed here.

## Negative-merge rule

If an overlapping main file cannot be reconciled with preserved type,
ownership, cancellation, generation, scheduler and guest invariants, leave
the PR unmerged. Semantic resolution is mandatory; `ours`/`theirs` blanket
selection is not acceptable evidence.


## Ticket table cleanup-hook lifetime and quota audit (2026-11-07)

- Handoff tables now deregister their STOP cleanup hook in the destructor.
  A shared registration token is held both by the table and the registry;
  a worker snapshot can finish its callback safely while the table's weak
  State reference prevents use-after-free. Destruction on a scheduler carrier
  is not admitted for this registry-connected host resource.
- Active cleanup hook quota is 64 per scheduler registry. Releasing a table
  frees one quota slot. Tests cover 96 create/destroy cycles, exhausting all
  64 live registrations, rejecting registration 65, and accepting a fresh
  registration once one table is destroyed.
- The handoff test suite now authors **80 named native-host assertions**
  (50 more than the initial registered-handoff 30). An independent C suite
  authors **16** two-incarnation STOP gate assertions. Previously authored
  suites: 219 lifecycle/STOP/scoped + 17 canonical coexistence, for a total
  of **332 named host assertions** (219 + 17 + 80 + 16). These counts are
  SOURCE-ONLY; neither the host suite nor any negative mutation has a
  verified runner-assigned execution receipt at this head.
- Optimized-workflow negative witnesses must reject unregistered derived
  contexts, disabled automatic pending ticket revocation, and omission
  of the recipient side of dual-actor STOP admission. The cancelled-parent
  sweep checks generation identity outside cancellation/table mutex nesting.
- Remaining: real native GCC/Clang/ASan/UBSan/TSan execution, full semantic
  merge with current main, compiler-issued guest data descriptors and
  tenant/isolate authentication; actor hot-upgrade/restart/tenant reclaim
  integration remains unproven.


## Late 2026-11-07 attachment and main-based scheduler integration

- Audit found that registry cleanup-hook registration failed closed for ALL
  new handoff tables after ANY unrelated attached actor STOP. The hook
  admission now permits fresh empty tables to serve surviving actors. Its
  bounded dual-generation scheduler commit still denies stopped recipients.
- A more serious gap allowed a lifecycle-aware handoff to create a pending
  ticket for a recipient never attached to the STOP registry: redemption
  failed later, but such a ticket had no recipient cleanup observer.
  Admission now requires a lock-free, immutable, generation-keyed attachment
  snapshot BEFORE envelope copying or ticket quota charge. The final
  scheduler dual-live gate still serializes actual publication against STOP.
  Snapshot nodes are allocated/published by CONTROL and retained until
  registry teardown; carriers never acquire the registry mutex for lookup.
- The handoff suite now has **100** named host assertions: 10 surviving
  actor admission cases and 10 lock-free recipient-attachment cases were
  added. Negative mutations separately remove (a) recipient attachment
  admission (case 13), (b) derived registry enrollment (case 09),
  (c) STOP cleanup hook, and (d) recipient half of dual-actor publication.
  This makes **352 authored host assertions** on draft #1389
  (219 preexisting + 17 coexistence + 100 handoff + 16 dual-live C).
  All are authored only, NOT native/sanitizer/guest execution receipts.
- Extracted a minimal main-based scheduler integration draft:
  **LLVM PR #1583**, branch
  `integration/dual-actor-stop-commit-20261107`, based on
  `main` commit `0ee037ca3e5dd9ac6ac1e9cc3d40f19ee6326a9c`.
  Four files only: scheduler C/H, the byte-identical 16-case C fixture,
  and an exact-head source-free admission + GCC/Clang native/sanitizer
  workflow with a deliberate lifecycle-bypass negative mutation.
  It must NOT be merged before independent review and real stepful tests.
- PR #1583 Actions run #42437688734 had a runner-admission failure:
  the admission job completed FAILURE with runner_id=null and steps=null;
  dependent native job SKIPPED. This is not an execution failure. The
  integration PR is DRAFT and does not claim full #1389 parity.
- Semantic conflict resolution for the remaining compiler, RequestLocal,
  native scheduler STOP notices and CMake paths is still outstanding.
  Do not blanket-choose the older scheduler source on top of current main.
