# LLVM imported public macro M1 — Java #2492 source-linker parity

**Status:** source implementation and tests authored. **NOT_EXECUTED** against the
actual LLVM source/compiler head until GitHub Actions allocates a runner with
nonempty step records. This is a **stacked draft** on LLVM #1399 (itself stacked
on source-identity #1380). No private source crossed organization boundaries.

## Implemented bounded guest contract

A physical source at `./import-producer.atto` may export only closed,
statically-checked public macro declarations of these two forms:

- `define pub macro literal(Const<int> value) -> Syntax<Expr> { return quote(value); }`
- `define pub macro identity(Syntax<Expr> value) -> Syntax<Expr> { return value; }`

The consumer imports `import * as lib from "./import-producer.atto";` and
expands `lib.literal!(const 40+2)` in the *production parser*, producing
a single `Expression::Kind::Integer` node valued 42. `Syntax<Expr>`
identity forwards a single already-parsed AST node.

The compile-time-only flow is:
1. Lexer-backed scanner reads leading star-import headers (no string regex or
   textual macro substitution), rejecting mixed import forms and duplicates.
2. The trusted `ModuleLoader` permits only explicit, same-directory `./*.atto`
   producer paths. Root ownership and no-alias checks **precede** opening the
   physical source.
3. `SourceInput` captures the expected physical file identity, reads within
   remaining 8 MiB budget, checks the observed descriptor identity, and the
   root ID registry re-admits that exact owner.
4. The same typed macro declaration parser validates the *entire* producer:
   only public typed macro signatures, no executable functions, nested imports
   or generated declarations.
5. LLVM SHA-256 hashes the verified producer bytes. Only the loader creates a
   trusted binding `{alias, path, logical_owner, source_sha256, exports}`.
6. The consumer matches its exact alias/path against that binding, adds
   qualified typed macro signatures to the per-parser registry, and expands
   into the ordinary checked expression AST **before** code generation.
7. `Program.compile_time_macro_imports` preserves the logical owner and digest
   through the linked AST. These records are compiler-only; no runtime macro
   callable or source reader is emitted into guest IR.

Existing v1 limits still apply: at most 8 aggregate declarations, 128
expansions, 512 visited const AST nodes and depth 64. No host-eval or
runtime fallback. Full Java macro language and general import/export
capabilities are deliberately NOT claimed.

## Authored test gates

- `native_macro_aot_admission_no_runtime_fallback`: production parser checks
  imported interface, one literal-42 AST, compiler-only owner/digest receipt,
  and rejection when no trusted binding is supplied.
- `native_macro_imported_literal_linked_guest_aot_42` and
  `native_macro_imported_identity_linked_guest_aot_42`: **real**
  `oreslang-llvmc -> emitted IR -> clang -> native executable` gates.
  Their executable receipts include hashes of the consumer, producer, compiler,
  generated IR and executable; both must exit 42 and have no unexpected output.
- Six checked-in compile-denial fixtures: private export, missing macro name,
  path traversal, explicit legacy producer, duplicate import alias and
  compile-time integer overflow. Every test requires nonzero compiler exit,
  a specific diagnostic, and **zero** LLVM IR stdout.
- `native_macro_imported_filesystem_security`: real compiler must reject
  symlink/hardlink aliases, missing producer and FIFO input, without LLVM IR.
- All tests are registered in CMake/CTest and required by the source-owned,
  exact-head `.github/workflows/atto-import-resolution.yml`. Source-head/tree
  checks occur before and after tests.

## Remaining differences / merge blockers

1. **Unexecuted** full compiler, CTest, Clang, ASan/UBSan and physical guest 42.
   Source-org jobs with `steps=[]` do NOT validate any implementation.
2. Only same-directory explicit macro producer paths are admitted, not
   `../`, `+/`, `pkg:`, arbitrary rooted imports, or mixed regular-module
   and macro imports. Reconcile LLVM #1336 canonical `.atto` policy first.
3. No general macro expansion hygiene, generated identifiers or declarations,
   foreign/remote macro authority, cross-package public macro export table,
   or incremental cache-invalidating artifact contract beyond source digest.
4. Descriptor identity rechecks and root lexical preflight are not atomic
   snapshots against a sufficiently strong concurrent filesystem attacker.
   Integrate fd-walk `O_NOFOLLOW` and stable descriptor pinning from the
   independently reviewed native source-input work before a security release.
5. Native M1 currently restricts logical filenames to ASCII. Java safe
   Unicode identity acceptance and complete canonical TOML root policy are
   separate parity work.
6. The producer-only grammar intentionally does not compile ordinary source
   declarations in a public macro producer. This must widen through a checked
   module export interface, not textual substitution.

## Required evidence promotion

- Independently review the exact branch diff, especially import ownership,
  generated typed macro bindings and C++17 build compatibility.
- Run compiler-backed source-owned Actions with actual steps, exact HEAD/tree
  and all new CTest gates including GCC/Clang -Werror and sanitizers.
- Reconcile #1336/#1365 source-entry semantics, then run Java and LLVM
  *byte-identical* imported macro positive/negative source fixtures.
- Keep all ancestor PRs draft and unmerged until their respective gates pass;
  do not mirror private Git trees across organizations without the independent
  governance approval.

References:
- Java issue: https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/2492
- LLVM source IDs #1380: https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1380
- LLVM scalar macros #1399: https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1399
- LLVM canonical imports #1336: https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1336
