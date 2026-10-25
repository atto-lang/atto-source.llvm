# Oreslang five-PR lifecycle merge — semantic reconciliation (HOLD)

**Five requested PRs:** LLVM [#928](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/928), [#931](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/931), [#948](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/948), [#987](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/987), and Java [#2015](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2015).

**This reconciliation is not a merge receipt.** None of the five should be
reported as merged without GitHub merged status, independent review, exact-head
CI execution and a post-merge receipt. GitHub's green `mergeable` flag alone
is insufficient, and draft status must not be stripped just to bypass a gate.

## Upstream prerequisites

- LLVM `main` already contains #939/#942 mailbox, Task and physical-loan
  integration. Treat those latest runtime files as authority; never overlay
  an old #931 scheduler/actor-group tree wholesale.
- #931 is stacked on #928. #987 is stacked on #931. #948 is a separate
  non-consuming lifecycle-observer branch based on `main` and has overlapping
  parser, contract and test expectations. Reconcile conceptually, not by
  choosing one side of an auto-generated CMake merge marker.
- Java #2015 is stacked on #2009, itself stacked on #2008, then #1949.
  Merging #2015 directly to Java `main` without those reviewed prerequisites
  would silently import that entire NIO/actor stack. Do not count a merge into
  a feature base as a release/mainline merge.

## Implemented semantic conflict resolution on the #987 integration branch

1. **READY:** direct `await actor`, explicit `get_ready()` and an immutable
   producer-provenanced `Future<void>` are repeatable observations of one
   lifecycle barrier. Each valid source method must declare typed
   `throws ActorTerminated`. One root CONTROL continuation is shared; two
   syntactic awaits do **not** imply two native bridge calls.
2. **DONE:** `get_done()` observes physically quiescent finalization, never
   readiness. Repeated DONE is the Java contract; the restricted LLVM guest
   path must remain red/fail-closed until a correct implementation exists.
3. **Capability:** reject forged/mutable/escaping tokens, stale ActorRef
   incarnations, invalid source effects and unproven Future producers.
4. **Runtime preservation:** #931's native scalar Future, VM CONTROL and
   failure-injection work are kept. #942's current mailbox, wake-escrow,
   resource-loan and single-flight behavior must be preserved at mainline
   reconciliation; no scheduler rewrite is justified by a front-end change.
5. **CMake/tests:** union registrations without duplicate targets; tests that
   previously *rejected* repeated READY observation become source-to-IR/Clang
   positives. Negative syntax/effect tests stay rejecting. The #987 probe now
   requires **one** coalesced bridge call per barrier rather than an incorrect
   two, excluding declarations/comments. Actual source guest execution and
   differential Java/LLVM traces are mandatory before claiming parity.
6. **`send_async`:** a wake-escrow primitive without atomic mailbox
   ACK -> credit TRANSFER -> wake publication does not satisfy L06. Preserve
   the negative gate until the complete bounded protocol executes in source.

The #948 parser/header/HasFuture contract/native contract test, matching source
fixtures and source-e2e/Rust AOT expectations were integrated into #987's
branch without replacing #931's runtime files. These are **unverified
source-bearing changes**, not a successful compiler run.

The canonical L01–L06 manifest is byte-identical across both repositories
(blob `41883b97563a3f06796f55c8c95676f18c113daa`).

## Merge gates and receipts

- Both LLVM #928 and #948 reported not mergeable with current `main` during
  audit; #931, #987 and Java #2015 are draft/stacked.
- GitHub review listings did not return independent approving reviews.
- Sampled exact-head source Actions jobs returned failed with zero executed
  steps; these are **NOT_EXECUTED**, not passed tests.
- Java must run source JUnit/GraalVM JDK25 tests and physical DONE races.
- LLVM must run source compiler -> emitted IR -> Clang guest execution, all
  targeted/full CTest, GCC/Clang strict builds, ASan/UBSan and applicable TSan.
- L06 must prove real mailbox slot ACK/wake exactly once under STOP/cancel/ABA,
  not merely independent host modules.
- Record post-merge base/tree, stepful results, test logs and both runtime traces.

Cross-organization export of private Oreslang sources remains **BLOCKED**.
The LLVM destination allowlist is empty, source and carrier `main` lack
verified protection, and platform-conformance incidents #22/#77/#127 remain
open. A successful source-free funded carrier is not transfer authorization.
