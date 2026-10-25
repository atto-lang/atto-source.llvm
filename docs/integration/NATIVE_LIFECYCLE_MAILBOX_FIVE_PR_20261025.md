# Five-PR native lifecycle + mailbox integration — 2026-10-25

**Status: staging/draft; NOT merged into `main`, NO source-executed qualification.**

Requested integration preserves these exact upstream inputs:

| PR | Head | Scope |
|---|---|---|
| #951 | `0a4e11a5ca586bf22a00163850e4a8c62583bfad` | terminal resource-loan seal |
| #955 | `cdaedf6c6c1b7951c6eaac3c1a84d029c74c410d` | CLAIMED continuation pin and execution identity |
| #971 | `717b768477bfb92f8b5c3ceb804400063738e1ab` | mailbox completion stress and STOP/DONE |
| #973 | `304bc3c4be89084e67c9f17230a5f5cd32013f75` | independent 52/172 actor-heap receipt oracle |
| #976 | `0501c7219612a3a4d07028eb0c475a6ef007bc6a` | duplicate owner and GCC fault-injection regression |

**Integration base:** #979 at `513d0bf92ffaf7e5b1bf0e149bed17781009dd3f`,
which already composes #971, #973 and #976 over LLVM main `e5c9973d97199e26f13157dbffbaffc192bb9993`.
This avoids re-merging three test fixes and inadvertently losing their semantic
reconciliation.

## Conflict resolution

- #951's production `ActorGroupLifecycle.hpp` is an exact snapshot from its
  audited main-based head. The #979 base retained the identical original
  production-header blob, so this does not regress an intervening actor
  runtime implementation.
- #951's two race-test patches were replayed as complete matching unified
  hunks **on top of #979's corrected owner/fault-injection fixtures**, not by
  overwriting them with the older #951 tests. Five hunks across two files
  matched uniquely; the resulting tests preserve prior #976 regression
  assertions and add STOP/close/retain and owned-send race coverage.
- #955's eight-file continuation/compiler-independent gate delta was imported
  from exact source blobs only after confirming the #979 version matched the
  original source-main baseline for every overlapping file. This retains the
  current transactional READY/CONTROL behavior.
- No base `CMakeLists.txt`, standalone mailbox runner, heap receipt oracle,
  mailbox lifecycle/stress fixtures or compiler/runtime unrelated files were
  replaced. CMake retains #979's three additional mail/STOP lifecycle tests;
  the #955 continuation source test is already a registered native target.

## Required physical contract

1. STOP/close/finalization atomically seal **new** resource admission; existing
   pre-seal loans remain counted and may drain without clearing the sticky
   terminal bit. DONE settles only after CONTROL, USER, loans and all physical
   work quiesce.
2. Bound mailbox `offer_to` and `offer_owned_to` race against the STOP
   admission seal. Rejection must not move the payload; successful reservation
   transfers exactly once, and the physical ACK discharges the one loan.
3. A CLAIMED continuation's last execution pin must not be released before
   `finish_resume`; nested child retirement must not reclaim a CLAIMED parent.
4. `resume_finish` must authenticate generation-bound continuation/parent
   identity plus logical ExecutionId. Forged/recycled links cannot claim a
   different guest's completion.
5. The independent actor-heap receipt prefix oracle remains correct for
   sequences ending at **52** and **172**, without weakened ownership checks.

## Verification evidence boundary

This integration performed *source blob/provenance checks, exact hunk matching,
changed-file comparison, and eight static invariants*. It did **not** run
native GCC/Clang compilation, CTest, AOT source guest execution or sanitizer
tests on this exact integrated head. Earlier standalone results on #979 and
source-free canaries on other heads do not qualify this combined tree.
Require actual steps and logs, exact commit/tree receipts, GCC/Clang strict
normal plus ASan/UBSan and applicable TSan, the standalone 11-test mailbox
suite, real source compiler-to-Clang guest tests, and reviewer approval.

The private CI destination allowlist is still empty, relevant security
incidents remain open, and private source must **not** be copied to an alternate
organization without independently reviewed protected-source authorization.
Do not mark this or its five upstream draft PRs as mainline merged.
