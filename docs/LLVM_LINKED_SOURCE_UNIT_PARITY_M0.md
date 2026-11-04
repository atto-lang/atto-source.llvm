# LLVM host-to-logical source-unit identity — Java #2492 parity M0

This is a source-local native compiler admission slice, not imported compile-time macro equivalence. No private source was mirrored to another organization.

## Implemented in this draft

- HostLogicalSourceUnitIds is constructed per linked build, and returns root-scoped identities such as project/app/consumer.atto and external/1/producer.atto.
- Only the nearest package root or package-less entry directory is owned implicitly. External source roots require explicit configuration. The production LLVM loader supplies no ambient external roots.
- Canonical .atto.pkg.toml is preferred over .ores.pkg.toml at the same directory. Malformed, unreadable or symlinked markers fail closed. The canonical reader supports only [project] root = "." at M0, not arbitrary TOML.
- Linked import closure admission verifies a physical path against a descriptor identity before reusing a dependency; a per-build registry rejects root escapes, hardlinks, duplicate logical aliases and in-root symlink redirects.
- Known import cycle/back-edges do not consume the new-source byte quota.
- Logical IDs are bounded to 4096 characters with a deliberately conservative ASCII-only filename policy. This safely rejects non-ASCII names that Java currently allows: remaining Unicode acceptance parity must be implemented explicitly, not assumed.
- Physical source paths remain available to the trusted reader and diagnostics; the loader records the root-scoped logical key separately.

## Distinct test gates — NOT YET EXECUTED at the private compiler head

1. native_host_logical_source_ids_security: a real C++ host component test for root identities, external ordinals, malformed IDs, hardlinks, symlinks, root denial and identity changes.
2. native_project_root_import_security: native resolver rejection of symlinks whose targets stay inside the root.
3. native_host_logical_source_ids_linked_guest: runs the REAL oreslang-llvmc on two physical .atto modules, checks IR output, Clang-links it, executes a native program returning 42, and verifies negative cases return no IR.
4. The focused atto-import-resolution GitHub workflow pins PR HEAD/tree and requires GCC/Clang, ASan/UBSan, actual CMake/CTest and native guest execution. Zero job steps always mean NOT_EXECUTED.

## Unfinished parity

- Java's imported macro syntax import * as lib from "./producer.atto" and lib.literal!(const 40 + 2) is not yet lowered by LLVM. LLVM macro PR #1356 provides a separate same-translation-unit scalar expander. This PR's native guest 42 proves ordinary module linking only if it actually runs.
- Native AST macro provenance, public export artifact owner/digest, macro expansion erasure, incremental cache IDs and source maps still require M1 integration. No runtime macro fallback is permitted.
- Full Unicode General_Category agreement, external-root ordinary runtime declarations, canonical package TOML parsing and secure atomically pinned file reads remain unverified.
- Exact-head source-owned or independently approved carrier jobs must really run; source-free alternate-runner canaries are not compiler evidence. Independently review and retain DRAFT until done.

References: Java issue https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/2492 ; Java draft https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2485 ; LLVM macro draft https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1356 ; LLVM canonical-import draft https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1336 .

## Merge order

1. Reconcile root/manifest compatibility with LLVM #1336 and #1365 on exact HEAD.
2. Execute GCC/Clang host checks and sanitizers, then the real native compiled linked guest and negative no-IR cases, and independently review the result.
3. Integrate with LLVM macro PR #1356. Bind imported public macro declarations to exact logical unit, checked artifact owner and digest, statically expand to a literal AST node and prove executable return 42.
4. Preserve private source governance. Do not move source across organizations on available-minutes evidence alone.
