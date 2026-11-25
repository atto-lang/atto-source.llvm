# AttoLang LLVM: independent source-CFG oracle execution (November 7, 2026)

## Why this was added

The 50-source restoration compiler fixture matrix in parent [PR #1552](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1552) has a Python host-only mutation suite and a still-unexecuted LLVM/Clang guest matrix. Its workflow-shape Node checks are structural, not an independent check of the source oracles. This child adds a **separate actual Node 22 test suite** that parses the authentic 50-program manifest and independently reconstructs source-defined class state, constructor identity, branch polarity and result/diagnostic oracles.

The Node suite runs **exactly 50 real JavaScript tests** using `node --test`, never executes guest `.atto`, and never emits a compiler/guest success marker. It rejects duplicate case IDs, duplicate source SHA-256 digests, malformed categories, missing `rt take` operations, wrong initial values, wrong constructor, wrong branch, and mismatched return/diagnostic oracles.

## Independent local execution evidence and precise scope

- The Linux working container has **Node v22.16.0**, **Python 3.13.5**, **Clang 17**; it has **no LLVM development headers or llvm-config**, and cannot resolve GitHub addresses directly.
- The 50-case manifest was independently reconstructed from the exact GitHub source fixture generator, then verified **byte-for-byte by Git blob hash**: local `git hash-object tests/contracts/atto_ownership_source_cfg_50.json` = GitHub content SHA **`38e92fd34a25d6773021cb4adfb5750bf0e9598c`**.
- New `tests/ownership_source_cfg_node_oracles_50.test.mjs` was first executed locally under **actual Node v22.16.0**; the initial run exposed a premature test-registration counter assertion (50 tests pass, one top-level failure). That assertion was corrected and the repeated real Node invocation finished **50 tests / 50 pass / 0 fail / 0 skipped**. Local `git hash-object` for the corrected file = GitHub content SHA **`13aa0857aca53a38727694d423e5ef08bd8a005c`**. Therefore the real Node run used the *same source bytes* as the committed test.
- An independently reconstructed Python validator/test module exercised **29/29 Python 3.13 host mutation checks** against that **blob-identical manifest**. The Python modules themselves were reconstructed from GitHub code and do **not** match the exact GitHub blobs. This is a useful independent host reproduction, **not** a claim that exact-head repository Python tests passed.
- The source-only workflow shape gate now includes the separate Node test command and validates the distinct TAP receipts `# tests 50`, `# pass 50`, `# fail 0`. Its own tests increased from 50 to **53**, passing **53/53 in an isolated V8/Node shim**, with `verified=false`, `guestExecutionVerified=false`. These 53 are **NOT hosted Node execution**.

## How this must run in authorized CI

The canonical `.github/workflows/atto-critical-ownership-llvm-parity.yml` now invokes, in order:

1. Static workflow-shape suite: **53/53** under a real hosted Node runtime, exact-head checkout.
2. Independent host-only fixture oracle suite: **50/50** real hosted Node tests.
3. Original 17 focused C++ ownership CTests and twenty scoped compiler checks.
4. Python host mutation CTest: **29** actual executed Python tests.
5. Two independent 50-program real LLVM compiler/Clang guest matrices: **100 source programs** total, positive linked native exits, negative semantic diagnostics.

All mandatory gates require exact immutable source SHA/tree and no failed or skipped test case. The source-organization Actions jobs have repeatedly returned `steps=null` from runner-admission failure; the billing/payment or spending-limit eligibility blocker in [issue #159](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/159) needs the authorized administrator. No cross-organization private-source mirror or approval bypass.

**Nonclaims:** This does not prove LLVM compiler execution, native ownership CTests, typed SSA LoanId completeness, optimized instruction-generation fencing, actor suspension/cancellation correctness or five-backend parity. Keep child and parents **DRAFT** until exact-head stepful evidence and eligible independent review are present.
