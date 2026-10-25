# LLVM PR #1006 — semantic mainline reconciliation and deterministic child-DONE verification

**Scope:** review-only integration of [#998](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/998) with the current LLVM mainline, not a merge to main and not native-execution qualification.

## Exact Git provenance

* Current source main at integration: `b4db9428802fe98153780b403a55059817722eef`; tree `43bf47d3da26b563e8e65d6b81f042f6e96979f7`.
* Previous #998 integration head: `0cd8af4bf6314a2d507102d11bcd6490cef4c0d2`; tree `e72550949b6258f697680ffdddfe0d250bac67f8`.
* Common ancestor: `e5c9973d97199e26f13157dbffbaffc192bb9993`.
* Semantic two-parent integration: `d67333c5fc78b2a286062ae97889c5ca619c745f`, tree `da4d212eb0bf5530fd63f28ab3782d5c3e7f00ac`. Parents in that order: main, #998.

Relative to common ancestor, #998 changed 36 tracked paths; the concurrently advancing mainline changed 35; only **four** overlap:
* `CMakeLists.txt` — 2 hunks.
* `src/main/cpp/oreslang/frontend/Parser.cpp` — 9 hunks.
* `src/main/cpp/oreslang/frontend/Parser.hpp` — 3 hunks.
* `src/main/cpp/oreslang/types/OwnershipChecker.cpp` — 1 hunk.

All **15** source hunks matched exactly once in the current main file. More importantly, applying each complete unified patch to its common-ancestor file reproduced **exactly** the #998 source bytes, so the patch engine was not silently discarding context. The merged files retain mainline-specific additions to the Task/borrow/type parser, source CMake tests and ownership checker. The other **32 source-side changed files** were imported as their exact Git blob objects after proving no mainline divergence on those paths. No build/test outcome is inferred from a clean textual merge.

## Physical DONE fix and deterministic witness

The source actor bridge originally could publish `await actor.get_done()` after the parent VM's physical ACK while constructor-owned children were still executing. #998 introduced `source_actor_join_owned_children` to stop/join owned children before settling the root DONE future, reusing the helper for synchronous stop and clearing freed slots before final parent reclamation.

#1006 removes the last timing guess from the adversarial regression:

1. Compile `oreslang-source-actor-capability-runtime-test` with `ORESLANG_SOURCE_ACTOR_TESTING` **only for this native test target**, not production compilation.
2. Start the parent actor and hold its child handler in a guest turn behind an atomic release gate.
3. Request parent STOP; a separate trusted host pthread awaits parent DONE.
4. The source bootstrap's test-only atomic counter increments **immediately after the real parent `ores_native_vm_wait_done` returns**, before child join and root future completion.
5. Boundedly wait for that exact acknowledgment. Assert child is still unfinished and the parent root DONE continuation count remains zero; only then release the child.
6. Assert the child finished and the root callback ran precisely once after physical subtree quiescence.

This is synchronization against an actual runtime state transition, **not 100,000 arbitrary scheduler yields**. A regression publishing root DONE at parent VM acknowledgment would run the callback while the held child is unfinished and fail its assertion. The test remains a real C11/POSIX CTest, but has not executed on the present candidate head.

## Exact-head test plan and acceptance gates

`ci/test-plans/llvm-pr1006.json` is scoped to only PR #1006 and contains **16 commands** within the v3 publisher's 16-command and 1800-second-per-command bounds:

* CMake full GCC build, nine-test CTest inventory assertion, targeted CTest, compiled source-to-LLVM-IR-to-Clang E2E and full CTest;
* Clang build, targeted CTest and full CTest;
* Clang ASan/UBSan build and targeted CTests;
* Clang TSan build and targeted CTests.

The plan's 14/14 shape, CTest registration, source lowering, test-only hook isolation and DONE-order static checks passed. These checks prove that test commands and code paths were composed correctly; they **do not** establish that any compiler, executable or sanitizer has run. The plan is NOT in an approved carrier contract, does not authorize private-source export, and is NOT a CI receipt.

## No-go gates

1. Source-org Actions still fail before recording executed steps (LLVM compiler run `40228309288` failed with `steps=null` for an earlier head on this new PR). Restore eligible source runners or finish independently reviewed protected cross-org carrier authorization.
2. Source `main` and inspected candidate carrier `main` report `protected=false`. The private-source approved destination allowlist contains no permitted repository, and existing contracts do not admit #1006. Security incidents platform-conformance #22, #77, #127 are open. **Never** mirror the private source tree to a public or unapproved GitHub organization, including through GitHub API blob copying without token secrets.
3. Require compiled native C and C++ CTest execution with exact head and tree receipts; GCC+Clang normal and ASan/UBSan, TSan where supported; compiled guest positive and rejected-no-IR negative source cases; simultaneous STOP, NIO completion, generation fencing, nested child physical joins, cancellation and Java parity.
4. Obtain independent reviewer approval, preserve the latest mainline commit across any intervening changes, and only then consider landing the dependent stack. PR #1006 stays a draft until these are met.

Current-main semantic reconciliation is real and committed. *Runnable test definitions are not executed test evidence.*
