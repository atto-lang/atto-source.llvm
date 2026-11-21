# LLVM scalar typed-AST macro + linked source identity integration (M0)

**Evidence classification:** source authored, production compiler and native guest NOT_EXECUTED until exact-HEAD stepful jobs run. This branch is stacked on LLVM source-identity draft #1380, not merged into main. No private compiler source was copied to an alternate organization.

## Source provenance and changes

- Source identity parent: same-repo LLVM #1380. Includes the per-build root-scoped identity registry, a verified pre-read source-root ownership check, descriptor/source identity checks and native module guest test.
- Typed macro parser: all 10 Parser.cpp diff hunks of LLVM #1356 were reapplied with exactly one matching old-context segment each. The single CMake hunk also matched uniquely. Unrelated parser and loader code is preserved.
- Fifteen other test/fixture/doc blobs were inserted by exact Git blob SHA from same-repo LLVM #1356 head c46f49b8e5c589c8054257d7b2c89f4bf644fd63. Four legacy .ores files are retained explicitly as compatibility fixtures; all new language fixtures use .atto.
- Supports the bounded same-translation-unit Const<int> literal quote, Syntax<Expr> identity and nested scalar 42 cases, with checked macro expansion before codegen. Existing source macros are not runtime functions.
- The two-module Java #2492 canonical imported-macro fixture is now pinned as import-producer.atto and import-consumer.atto.

## Current deliberate limitation: public imported macros remain rejected

Parser.cpp explicitly returns NATIVE_IMPORTED_MACRO_LINKER_REQUIRED upon import-star syntax, before compiling, with no runtime fallback. The real-compiler CTest native_macro_imported_public_unlinked_no_ir demands that diagnostic, nonzero exit and empty IR. This **must be replaced by a positive guest return-42 test**, not silently renamed green, only after a source-owned typed macro linker exists.

No public macro import or expansion parity is claimed: an exported macro requires (a) a validated logical producer unit ID, (b) declared public visibility and verified exact typed export AST, owner/module and content digest, (c) alias/namespace and import-resolution provenance, (d) closed bounded compile-time expansion in the consumer, and (e) no unexpanded runtime macro node or generated symbol after lowering. An artifact SHA is a cache identity, not import authority.

## Exact-head execution gates

The focused source-owned atto-import-resolution workflow now requires:
1. An exact checked-out source HEAD and printed Git tree, repeated after execution.
2. GCC and Clang Werror host root and logical ID unit programs; ASan+UBSan runs.
3. Real LLVM compiler CMake target plus macro admission C++ test.
4. CTest inventory requiring linked .atto native module guest, scalar macro linked guest 42 (literal, identity, nested), and imported-macro no-IR negative.
5. Real oreslang-llvmc -> emitted LLVM IR -> Clang -> native exit-42 for supported scalar macro cases, and negative no-IR outcomes.

If the GitHub job has no executed steps, classify NOT_EXECUTED regardless of failure, cancellation, or source-only canary outcomes.

## Mandatory next integration

Merge/reconcile source-owning package rules from LLVM #1336/#1365 and independent security review first. Then implement a true imported-macro source-to-source typed AST linker (not textual preprocessing), stable macro artifact ownership by logical unit ID, and reject absolute/unowned/forged import resolutions. Add runnable two-file import and native guest 42 positives, malformed Unicode/symlink/hardlink/cross-root negatives, and exact-head CI on protected source or explicitly approved source-bearing carrier. This draft is not ready for merge.
