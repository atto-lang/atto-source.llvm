# AttoLang LLVM linked-source ownership before filesystem probes

**2026-11-04 status: AUTHORED / NOT_EXECUTED**. Stacked directly on LLVM [#1407](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1407) (itself stacked on [#1378](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1378)). This is a **same private repository** branch; no cross-organization transfer.

## Bug class and fix

Prior to this change, the LLVM linked loader's relative-import branch called `resolve_native_source_suffix` before calling `NativeLogicalSourceAdmission` / `no_symlink_components`. For legacy `.ores` entries, suffix resolution checks `exists_marker` on both `.atto` and `.ores` candidates. Therefore a path such as `../../outside` could be probed **outside the admitted project root before ownership denial**. Canonical `.atto` imports also did unnecessary reads of the legacy sibling's existence even though the policy never chooses it.

This patch:
1. Adds **no-I/O** `checked_relative_source_path`, rejecting paths outside the root (and the root directory itself) *before* suffix discovery.
2. Calls `no_symlink_components` on the **unsuffixed** import candidate before suffix discovery. This enforces the no-I/O lexical root check **and** detects a symlinked ancestor directory before either `.atto` or `.ores` candidate is inspected. It revalidates the selected suffixed path before canonicalization and descriptor identity checks.
3. For canonical `.atto` importers, returns the expected modern suffix before inspecting existence of an irrelevant legacy sibling.
4. Adds native host C++ lexical checks (existing, absent, root itself, and a symlinked ancestor) plus three **real native compiler negative** fixtures (outside existing, outside absent, and in-root symlinked ancestor). Each requires nonzero compiler status, empty stdout (no LLVM IR), and the appropriate preprobe ownership/symlink diagnostic.
   
## Bounded claims

- Native repo CTest/linked compiler negatives are **authored, NOT EXECUTED**. An **isolated** reproduction of the root/symlink-check implementation passed six checks under GCC and Clang at C++17 -O1 with ASan/UBSan. This is not a native repo build. The source-owned `llvm-smoke` rerun [41886621399](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/41886621399) failed with **zero executed steps**.
- Path-based `fs::canonical` and `stat` still need a descriptor-relative, race-safe authority chain against a malicious concurrent filesystem writer. This PR does not claim to close that separate TOCTOU risk.
- This change does not implement general typed `Syntax<Expr>` macro expansion, same-code generation across Java and LLVM, or LLVM/JVM/Wasm/Forge/.NET parity.
- Keep existing strict source-org runner admission and private-source export governance; a source-free runner canary does not authorize copying private source to another organization.

## Follow-up validation

Run the production CMake build and CTest (including `native_logical_source_unit_identity`, `native_source_module_graph_execution` and imported-macro guest CTests) on the immutable head and record actual step arrays, GCC/Clang, ASan/UBSan, positive compiler→LLVM IR→Clang→native exit 42 and negative zero-IR receipts. Require independent security review and preserve the draft status until those gates are satisfied.
