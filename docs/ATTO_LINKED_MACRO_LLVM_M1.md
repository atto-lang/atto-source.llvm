# AttoLang LLVM M1 — linked source macro parity integration

**Evidence level: source-implemented candidate; native compiler build, executable guest and
cross-backend parity NOT_EXECUTED at this head.** This branch is stacked on
LLVM logical source identity draft #1378. It ports only the bounded local
typed-AST expander of LLVM draft #1026 into the newer parser and integrates
the compatible canonical source import behavior proposed by LLVM #1336.
Neither older draft branch is merged by this work.

## Executable guest target

`producer.atto`:

```atto
define pub macro literal(Const<int> value) -> Syntax<Expr> {
    return quote(value);
}
```

`consumer.atto`:

```atto
import * as lib from "./producer.atto";

pub fnc main(): int {
    return lib.literal!(const 40 + 2);
}
```

The native linker accepts the module import only when:
1. Its logical project-owned source ID is admitted by the exact
   build-local registry from #1378.
2. The target is a standalone `.atto` macro-only unit (no compiled
   runtime exports, package imports, actors, generators, or nested modules).
3. The referenced macro is an exact **public** checked
   `Const<int> -> Syntax<Expr>` quote declaration.
4. Its supplied typed AST is a closed i64 literal expression using only
   negation, addition, subtraction or multiplication, with checked
   overflow and bounded 64-depth/512-node traversal.
5. The linked per-build imported expansion count is at most 128.

The parser emits a compiler-only pending call AST for an imported
`lib.literal!(const ...)`. The linker resolves the trusted alias to its
source Unit, checks that Unit's declaration and identity, then replaces
the pending expression with a freshly constructed `Integer` AST.
No source-string interpolation, runtime macro interpreter, runtime
import wildcard, or second executable stage is authorized. The
complete linked `Program` does **not** receive the macro-declaration
metadata. Local macro forms from #1026 use their prior independent,
bounded parser expansion.

Unsupported external/public macro exports, imported Syntax<Expr>
identity and quotation templates, dynamic constants, public declaration
generators, unknown/private macros, malformed signatures and forgery
all fail before native IR. A normal `import module ...` is **not**
permission to invoke a source macro.

## Canonical package imports

- A `.atto` importer resolves an extensionless `./lib`,
  `../lib` or `+/lib` to `lib.atto` only. Legacy `lib.ores` is
  available only by explicit suffix.
- A regular `.atto.pkg.toml` takes precedence over a same-directory
  `.ores.pkg.toml`. Canonical manifest symlinks and invalid canonical
  manifests do not trigger legacy fallback.
- The native TOML subset accepts `schema_version = "1"` and
  literal `[project] root = "."`; arbitrary root relocation, duplicate
  `[project]` tables, multiline TOML and unsupported roots reject.
- Legacy `.ores` importers retain the prior twin-ambiguity rule pending
  separately ratified shared conformance.
- This resolver is not a directory-handle-pinned, race-proof hostile-tree
  reader. Concurrent symlink swap/restore remains a separate hardening
  and exact-head execution gate.

## Committed test assets, not executed receipts

- `tests/native_macro_aot_admission_test.cpp` and the **original**
  scalar macro regression fixtures from #1026 (copied by exact blob SHA,
  without replacing newer `Parser.cpp` with the older branch version).
- Registered CTests for local typed-AST macro parser, actual
  `oreslang-llvmc -> LLVM IR -> Clang -> native executable exit 42`,
  and compiler-only zero-IR rejections.
- `native_source_module_graph_execution` now includes the true two-file
  public macro guest 42 with its source files removed before execution,
  plus separate private-export, forged-import, dynamic-constant, overflow,
  unsupported-signature, runtime-star, and import-precedence negatives.
- `native_project_root_import_security` includes canonical project
  manifest, no implicit legacy fallback, malformed canonical manifest and
  nested package security tests.

**Required promotion:** exact-head verified stepful GCC/Clang and CTest,
full native regression, Linux/x86 and ASan/UBSan as supported, manifest
and source race tests, executed native exit 42 and guest-IR macro erasure,
independent review, plus Java linked-guest differential. A source-only
file/AST audit or GitHub Actions run with `steps=null` is NOT_EXECUTED.

## Deliberate parity limits

This M1 is *one* linked `Const<int>` literal-quote form. It is not general
imported `Syntax<Expr>`, hygiene/capture safety for generated names,
arbitrary AST quotation, cross-module macro re-exports, external-package
macro trust, or complete Java/Graal/LLVM/Wasm/Cranelift/.NET parity.
Java #2485 implements a public-export artifact identity envelope, which
does not itself prove linked imported macro execution. Cross-toolchain
parity must continue to use exact compiled guest receipts, not source
descriptions.

No private source was mirrored outside `ores-truffle-oreslang` by this PR.
