# AttoLang named generics + named value calls: LLVM native scalar slice

Status: **draft implementation and compiler-backed CTest targets**, not a verified
cross-backend guest parity receipt. Java and the shared conformance repository
have broader syntax/semantic targets. This branch also admits `.atto`
direct-entry source paths and extensionless relative/project-root imports
resolved by the importing source's suffix. Explicit `.ores` and `.atto`
paths remain valid during migration. This is implementation plus pending
executable validation, not a stepful parity receipt.

## Working subset targeted by this patch

```atto
fnc encode<A, B=int, C=B>(A first, B second): int {
  return first * 10 + second;
}
pub fnc main(): int {
  return encode<C=int, A=int>(second: 2, first: 4);
}
```

There are **two separate binding maps**. `<C=int, A=int>` chooses
compile-time generic slots by case-sensitive name. `(second: 2, first: 4)`
maps source-evaluated values to declaration ABI slots. Neither map may
select an overload by label or runtime type. Actuals are evaluated in source
order and then reordered to formal ABI order.

This branch adds a strict scalar type check for every generic formal:
all admitted generic concrete values are `int`/LLVM i64 in this first
native slice. The previous `parameter_is_scalar` check skipped
`parameter_generic_types`, risking a boolean (i1) or string-typed actual
being passed to an i64 formal. A second LLVM emitter guard rejects any
mismatch between evaluated LLVM actual types and declared callee formal
types before emitting `call`. These checks use the already-bound formal
slot, never the original source index.

## Explicit support limits

| Construct | LLVM native status / gate |
| --- | --- |
| `fnc f<T>(T a)`, `routine f<T>(T a)` | Scalar i64 candidate |
| `f<T=int>(a: 42)` | Combined generic/value binder candidate |
| Named generic order, default from earlier slot, `_` inference | Scalar i64 only |
| Named runtime values in arbitrary source order | Compiler-checked exact required slots |
| Multiple same-name/different-arity functions | Select by name/arity, not labels |
| Generic method declarations and class type instantiation | Fail closed; nominal ABI not specialized |
| Named Array/uncertified member calls | Fail closed on parent LLVM #1228 |
| `T?` optional runtime argument omission, `@Default` | Fail closed; no presence/Option ABI |
| Non-i64 generic actuals: bool/string/classes | Fail closed |
| Move/borrow/actor/suspension generic crossing | Existing strict owner/effect gates |
| Generic `Any`/`Unknown` fallback and implicit conversion | Not admitted |
| LLVM `.atto` source admission | Direct native guest CTest added; not yet executed |
| `.atto` relative and `+/` module imports | Compiler graph + root-security suites added; not yet executed |
| Legacy `.ores` imports | Preserved; extensionless default chooses importing source suffix |

## Source dialect and symlink identity hardening

The LLVM loader canonicalizes filesystem paths to enforce physical unit
identity. It now also rejects a mismatch between the requested
`.atto`/`.ores` source suffix and the physical target suffix, so a
symlink cannot silently switch the extensionless resolution rule in the
entry file, a relative import, or a `+/` package-root import.
Same-suffix symlinks remain permitted, and explicitly importing the
physical `.ores` file from `.atto` (or vice versa) remains supported.

Native entry filenames must also be explicitly `.atto` or `.ores`: the
loader rejects extensionless, unrelated, uppercase-case-mismatched and
double-extension filenames before parsing, with zero emitted LLVM IR.
Four new negative cases are in `native_source_module_graph_execution`.

The `native_source_module_graph_execution` suite has seven additional
regression scenarios: both suffix directions for entry, relative and
project-root imports must reject with no LLVM IR; a same-suffix `.atto`
symlink must compile, link, and execute a 42-exit native guest. The
independent `native_project_root_import_security` header-level test also
checks both source/target cross-suffix symlink directions and one positive
same-suffix `.atto` import (five new assertions). The module-graph suite
also rejects **four hard-link cross-dialect aliases**: both import orders
across a shared `.atto`/`.ores` inode, and both self-import alias directions.
A file's import dialect must never depend on visitation order. The
standalone project-root resolver also rejects invalid and extensionless
importing-source filenames directly (four C++ negative assertions), even
when called without the module loader. These are **registered, not yet
executed** until an authorized runner is admitted.

## TOML project-root manifest admission

The small native `.ores.pkg.toml` reader must not silently ignore TOML
`[project.root]`, quoted/dotted variants, `[[project.root]]`, or
`[[project]]`. A `project.root` setting would otherwise appear accepted
while `+/` imports continue resolving from the manifest directory,
potentially crossing a caller's intended package boundary. Such declarations
now fail closed pending a complete TOML parser. Dotted project descendants
(`[project.root.extra]`, `[project.logging]`) and Unicode-escaped TOML
key spellings are also rejected, so escaping cannot disguise a project-root
setting. Dotted `project.root.path =`, `project . root . path =`, and
`[project]` followed by `root . path =` are rejected too: TOML permits
both arbitrary whitespace around dots and nested key assignments.
Duplicate `[project]` tables are rejected; unrelated `[build.*]` and
`[build] root . config=...` metadata remain allowed.

Added **22 negative compiler scenarios** to `native_source_module_graph_execution`
requiring no LLVM IR plus one positive `.atto` native compile/link/guest
case exercising unrelated dotted build metadata. The standalone
`native_project_root_import_security` suite gains **23 cases**
(22 negative and one unrelated-root positive). These are authored tests,
not backend execution evidence.

## Generic-call token lookahead and separate binding namespaces

The native parser used to inspect only **64** tokens when deciding
whether `<...>(...)` was a generic call. A valid call with 24 explicitly
named type arguments needs about 96 tokens, so the parser would instead
treat `<` as a comparison operator. The bounded lookahead is increased
to **4,096 tokens**. The new `.atto` fixture checks recognition, full
native compilation, Clang linking, and exit 42; the cap remains finite.

A second `.atto` fixture uses `A` and `B` in **both** the generic type
parameter map and the runtime argument map. Its declaration and call
reverse the value labels independently of the generic labels. The
result must be 42, proving that labels are not conflated across the two
maps. Neither fixture has stepful execution evidence yet.

## Executable evidence gates

The focused workflow `.github/workflows/named-call-exact-head.yml`
checks the exact Git commit/tree, builds `oreslang-llvmc` and its Rust
test harness, requires all named-call CTests in the inventory, and runs
the same selected suite. Its newly authored **22** CTests comprise:

- **5 positive native linked guest executions**: combined named generics
  + values, a direct-only routine, explicit inference hole, positional
  prefix, and reordered source-effect case (expected exits 42/42/42/42/12).
- **4 additional positive `.atto` linked guests**: the original combined
  generic/value ABI, lowercase `<foo=int,bar=foo>` type parameter names,
  a **24-slot named generic call** crossing the former 64-token parser
  lookahead budget, and a call in which `A`/`B` name both compile-time
  generic slots and runtime value slots. All four expect exit 42.
- **1 real LLVM IR-order assertion**, checking `read_second` precedes
  `read_first` after named binding.
- **1 combinatorial real native test** that generates and compiles all 36
  `3! × 3!` named type-argument and runtime value-argument permutations as
  canonical `.atto` source files. Every emitted IR module is linked and run
  under both Clang `-O0` and `-O2` (72 executions); each must return exactly
  42, rather than relying on a sum-only oracle.
- **11 negative real-compiler source checks** requiring rejection/diagnostic
  before LLVM IR: bool/string type erasure, reordered bool actual,
  duplicated/unknown/ordered generic slots, value-slot collision,
  unknown value label, uninferable hole, and two case-sensitive `.atto`
  generic-type/runtime-value label failures.

These 22 dedicated CTests together require **81 native guest executions**
(5 legacy-source guests, 4 AttoLang-source guests, 72 generated permutation
guests), 1 IR-order proof and 11 source rejection cases. The workflow also
requires the existing `native_source_module_graph_execution` and
`native_project_root_import_security` CTests, which now cover `.atto`
relative imports, project-root imports, mixed explicit extension migration,
negative no-fallback cases, nested package fences and symlink escape.
The workflow gates **37 CTest names** in total (13 existing named-call/
member tests, including reordered guest ABI, mixed named calls, overload
selection by arity, and mutable-borrow conflict/binding; 22 new scalar
generic/value cases; and 2 import/security suites). All are pending real
execution. A test listing, authored test source, zero-step job, or
host binder oracle does not establish execution. For promotion require
exact-head source/tree attestation, nonzero executed job steps,
`ctest --output-on-failure` pass counts, LLVM IR emission and Clang
link + guest return receipts, negative diagnostics without emitted IR,
and independent review. Repeat with Release/Debug and sanitizers where
applicable. Keep private source in the owning organization until separately
authorized transfer policy is reviewed and approved.

## Cross-backend progression

Shared conformance `oreslang-platform-conformance` PR #345 **merged** and
expanded the source-target-only corpus to 40 `.atto` cases (18 accept/22
reject). Its backend result fields are intentionally `pending`. Java #2507 covers
generic declaration/source binding; Java #2508 is a review of
identity-bound named call optimizer proofs stacked on Java #2086.
LLVM #1228 is a prerequisite for member-call fail-closed parsing.

The next native milestones are verified (stepful) `.atto` guest/source
admission and import tests, stable compile-time generic specialization
identity beyond i64,
proper generic method/constructor ABI, optional presence metadata and
call ownership/effect checks across both labeled maps. These must
not be promoted by this scalar-only patch.
