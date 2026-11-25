# AttoLang LLVM source CFG guest-oracle hardening — v2

**November 7, 2026. Draft, non-promoting; no native/guest execution evidence.**

## Audited gap in parent #1550

The fifty compiler/guest candidates in `atto_ownership_source_cfg_50.json` originally used the same condition `if (N eq N)` for every both-branch restoration test. Consequently, all ten positive guest executions exercised the true branch and **none executed the false branch**, despite requiring both branches to restore.

The host-only fixture validator also accepted any valid exit integer from 1 to 255, so a source edit that changed the value delivered by a particular branch could leave a syntactically valid manifest without a source-derived output oracle. Actual Clang guest execution would eventually detect this, but until runner admission is restored the source-only checks could not even flag a plainly forged exit.

## Exact source and oracle repairs

- Keep all **50** distinct candidate `.atto` sources and all five existing semantic categories, ten per category. In the **ten positive both-branch restoration** fixtures, five now have a true predicate `N eq N` and five a false predicate `N eq (N+1)` to require a real Clang-linked guest to execute the `else` restoration path on five runs. Accordingly, the corresponding positive process-exit oracles are changed from `N` to `N+1`.
- Apply the same five/ five true-vs-false branch split to the **ten negative one-branch-only restoration** candidates; both states must be rejected because the missing predecessor remains possible.
- Harden `tests/ownership_source_cfg_compiler_matrix.py` so the host manifest validator reconstructs the declared class's index, initial scalar field value, allocation identity, branch polarity and expected guest exit from the actual source text and required category, rather than accepting any in-range exit value. Nonbranch categories must not smuggle a source `if` guard.
- Expand `tests/test_ownership_source_cfg_compiler_matrix.py` from 18 to **29 independent Python host-only negative/positive mutation tests**: false-branch oracle forgery, truth reversal, class/default/constructor origin substitution, duplicated predicates, out-of-range exits, missing/duplicate sources, wrong diagnostics, and disabled actual compiler/Clang execution gates.
- Rename the host-only CTest to `native_ownership_source_cfg_manifest_28_mutations`. Require exact host JUnit test identity, `Ran 29 tests`, and an independently parsed no-failure/no-skip CTest summary.
- Expand workflow source-shape mutation suite from 46 to **50 source-only mutation groups**; keep the original 17 focused native CTests, 20 scoped compiler cases, **two independent actual 50-program compiler matrices** and immutable SHA/tree checks. The added source-only tests reject missing source triggers, missing real compiler runner and reduced manifest-mutation evidence.

## Evidence and remaining gates

Fetched exact draft source's 50-case source-only workflow shape suite passed **50/50 in isolated V8 with a Node test/assertion shim**, reporting `checkedNativeCTestCount=17`, `verified=false`, `guestExecutionVerified=false`. This is **NOT** hosted Node 22. Manifest source rows and expected branch exits have been inspected and checked, but the **29 Python host tests** and all native/compiled guests are **NOT_EXECUTED** here. The local environment cannot fetch private/remote GitHub source through DNS, and it lacks native LLVM developer headers/llvm-config. GitHub Actions source-org jobs continue to fail before runner assignment with `steps=null`; provider billing/payment or spending-cap eligibility remains for authorized owner (#159 / conformance #315).

**Required release proof:** a same-organization source-free job with nonzero steps; exact-head hosted 50 Node integrity checks; 28 Python host mutation cases with exact receipt; 17 C++ ownership CTests with sanitizers; 20 scoped compiler checks; both real 50-program LLVM/Clang `.atto` matrices (100 source cases total). The first matrix covers typed places; the second directly exercises true and false restoration paths. Demand source SHA, Git tree, compiler identity, per-case diagnostic/Clang exit records and no skipped cases, plus eligible ORESoftware independent review. No cross-organization private-source mirror, no security bypass, no merge/guest verification promotion.

This patch does not implement whole-program SSA LoanId use completeness, optimized IR instruction dominance, optimizer generation fences, actor cancellation or five-backend compiler parity.
