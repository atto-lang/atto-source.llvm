# Native annotation / macro AOT admission contract (draft)

## Non-negotiable invariant

Every supported Oreslang attribute, annotation and macro must be processed before
native code generation. **Unsupported semantics fail before LLVM IR is emitted.**
An attribute must never be silently dropped because one backend does not implement it.
A native executable must never require runtime source parsing, `eval`, dynamic macro
execution, JIT compilation, or reflective code synthesis.

## Current LLVM implementation status

LLVM's experimental source compiler does **not** currently implement the Java
annotation AST, `@FromJson`, derivation, attribute macros, `macro_rules!`, or typed
procedural macros. The native lexer/parser rejects those source forms; this is
intentional, not evidence of parity. `tests/annotation_macro_aot_admission_matrix.py`
asserts that representative annotations/macros are rejected with nonzero status,
no LLVM IR on stdout, **no unexpected generated files** in an isolated
working directory, and a diagnostic under both host and wasm32 targets. It
also checks that attribute-shaped source comments remain inert and that
ordinary unannotated typed functions/calls still emit valid `main` and helper
functions (including the certified wasm32 target triple).

## Required future implementation (shared contract)

1. Parse attribute and annotation declarations into retained typed metadata, with
   identity, target, arguments, repeated/inherited policy, and retention.
2. Expand hygienic declarative, derive, and procedural macros *at build time only*.
   A `rebuildable` macro means a separate compilation generation, not native JIT.
3. Enforce known target support, recognized attributes, constant argument types,
   repeatability, dependency and expansion limits, source provenance and hygiene.
4. Re-run type, borrow, `using`, `critical`, suspension, actor state isolation,
   message sendability, and ABI checks **after** full macro expansion.
5. Emit a canonical expansion/ABI manifest and compare it against the Java backend
   with JSON Schema/TypeSpec conformance vectors. Generated code must be compiled
   into a standalone guest-native application; interpreter-native packaging alone
   is not equivalent to guest AOT.
6. Unsupported target-specific semantics cannot be treated as no-op optimization
   hints unless the annotation definition explicitly permits that behavior.
7. Prove negative cases cannot emit partially usable IR, including nested macros,
   malformed/unknown annotations, actor-field injection and unproved ownership.

## Future test promotion

For each newly supported attribute, replace its negative fixture with positive
native execution and equivalence checks on both backends. Do not delete a
negative fixture until a corresponding positive compile-and-run gate exists.

Related Java/GraalVM integration: [AOT macro and annotation PR #771](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/771).
Artifact admission contract: [PR #777](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/777).
All current-source CI gates remain blocked until real runner jobs execute.
