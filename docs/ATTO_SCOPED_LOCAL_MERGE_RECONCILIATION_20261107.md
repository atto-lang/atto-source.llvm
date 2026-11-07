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
2. Automatic rejection/revocation of **unredeemed** handoff tickets on
   recipient STOP still requires lifecycle-managed ticket-table cleanup.
   Derived contexts are now registered, but pending ticket envelopes remain
   a distinct bounded-lifetime obligation.
3. Require issuer-authenticated tenant/isolate/request identity and
   compile-time ownership/Sendable descriptors before exposing guest ABI.
4. Restore source-free runner admission and obtain exact-head GCC/Clang,
   ASan, UBSan, TSan, native CTest and compiler-backed `.atto` receipts.
   `steps=[]`/`steps=null` proves no test was executed.
5. Keep PR #1389 draft until conflicts are resolved, independent review
   counts under GitHub rules and all mandatory CI/security gates pass.

## Negative-merge rule

If an overlapping main file cannot be reconciled with preserved type,
ownership, cancellation, generation, scheduler and guest invariants, leave
the PR unmerged. Semantic resolution is mandatory; `ours`/`theirs` blanket
selection is not acceptable evidence.
