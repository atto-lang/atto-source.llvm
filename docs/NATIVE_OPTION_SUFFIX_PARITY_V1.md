# Native Option type suffix parity — scalar ABI slice (2026-11-20)

**Implementation status:** source and material tests authored on the current PR branch. **Execution status: NOT_EXECUTED** until the exact-head workflow actually runs. This is not a claim of complete Java/GraalVM ↔ LLVM feature parity.

## Canonical language boundary

AttoLang Java already parses a postfix type `T?` as `Option<T>`; the postfix binds tighter than a union. This patch maps only **native scalar** `int?`, `bool?`, `String?` and `string?` onto the same `NativeSumType::Option` descriptor and checked tagged payload semantics as corresponding explicit `Option<T>` types. The existing parser representation (not a new ABI or a raw nullable pointer) is used for supported local declarations, parameter/return type annotations and scalar class field declarations.

The following are **not** silently treated as equivalent:

- `T name?` or `name?: T`: omittable parameter admission with defaults/presence ABI is separate and remains unsupported in the native compiler.
- `T??`: repeated suffix is invalid; nested optional values must be explicitly typed and supported by the target ABI.
- `void?` and `null?`: invalid value types.
- `Class?`, `Option<T>?`, `Result<T,E>?`, alias`?` or resource/compound payloads: **E_OPTION_TYPE_ABI_UNSUPPORTED** until a verified owned native tagged-object representation exists.
- Typed unions, field generic compositions, full class member lifetimes and dynamically omitted call arguments remain out of scope. Unsupported syntax must emit a diagnostic and **no partial LLVM IR**.

The `?` suffix must not affect expression ternaries, Result/Option value propagation, or `?` after a parameter name. No implicit `T -> Some(T)` conversion is introduced. No public `owned` qualifier is added.

## Material test contract

The source fixtures in `tests/fixtures/option-suffix-v1/` are compiled with the **actual** `oreslang-llvmc` C++ compiler into LLVM IR, linked using Clang `-O0` and `-O2`, and executed as native binaries. The three positive fixtures (explicit `Option<int>`, `int? = Some(42)`, `int? = None`) must each exit **42** with empty stdout/stderr, yielding 6 executed guests. Seven negative fixtures must exit nonzero, provide a diagnostic, and emit no IR. The gate is registered as `atto_option_suffix_guest_v1` in CTest and `.github/workflows/native-option-suffix-v1.yml`; this workflow pins its actual Git head/tree and checks source fixture blob identities.

**Required before promotion:** actual successful CMake build and six native guest executions; seven compiler rejections; exact-head/tree identity; no zero-step runner receipt; independent source, ownership and security review; matched Java positive and negative guest oracles in `atto-platform-conformance`. A parser-only pass, host test or a source PR description is insufficient.

## Overlap audit / migration

- Java legacy [#3157](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/3157) implements the complete `T?` parser grammar; this native PR intentionally only admits the checked scalar ABI.
- LLVM legacy `Parser.cpp` recognizes `T?` in selected positions but explicitly rejects it pending optional ABI. This patch reuses the existing tagged Option subset rather than treating `T?` as omission or erasing it to an int.
- Atto LLVM [#61](https://github.com/atto-lang/atto-source.llvm/pull/61) addresses Channel<int> lexical denial, [#75](https://github.com/atto-lang/atto-source.llvm/pull/75) addresses trap-to-Result. Both are semantically independent; none should be merged into this PR.
- Atto spec [#143](https://github.com/atto-lang/atto-spec/pull/143) addresses `^`/ `^^` semantics; this patch does not touch bitwise or logical operator parsing. Current native logical-XOR typing remains a separate migration issue.

**Next steps:** expand scalar positive/negative type contexts only when real compiler evidence passes. Implement complex Option payloads together with ownership, source actor isolation, tagging, and safe lifecycle. Do not declare language-wide parity from this scalar subset.
