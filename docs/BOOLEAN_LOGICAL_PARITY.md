# Canonical Boolean source operators — native LLVM parity gate

The 2026-10-17 document claimed that the LLVM frontend had no typed Boolean lowering. That claim is **outdated**. The current compiler already has a checked `ScalarType::Bool`, typed logical-expression admission, LLVM `i1` comparisons/XOR and `&&`/`||` conditional branches with PHI results. See `docs/NATIVE_BOOL_TYPES.md` and `tests/native_bool_matrix.py`.

## Syntax candidate (PR: canonical Boolean infix parity)

- Case-sensitive `&&` / `AND`: short-circuit AND.
- Case-sensitive `^^` / `XOR`: eager Boolean XOR, evaluating both operands exactly once left to right.
- Case-sensitive `||` / `OR`: short-circuit OR.
- Precedence (tight to loose): comparisons/equality, AND, XOR, OR, ternary. Each infix level associates left-to-right.
- Operands require the distinct checked `bool` type; integer truthiness and bool/int coercion are not admitted.
- Uppercase words are reserved infix aliases. Lowercase legacy word operators are reserved but not silently enabled by this change.
- `Xor(a,b)` stays a separate existing native scalar built-in with eager operand evaluation. Other named Boolean intrinsic overloads remain a separate parity task.
- Single `^` (integer bitwise XOR) is distinct from `^^` and should follow its existing fail-closed/named Integer operation contract.

## Required executable proof

The `tests/native_bool_matrix.py` suite compiles real `.ores` source, verifies emitted LLVM IR, builds with Clang and executes the native guest executable. This PR extends it with six spellings over four truth-table rows, mixed precedence, short-circuit state mutation tests, eager XOR side effects, and type rejection of unreachable RHS operands.

Real GitHub Actions failures before job steps do **not** count as executable verification. Do not mark LLVM as passed until an independent approved runner executes the **exact PR-head Git tree** with nonzero tests, including sanitizer configurations, and captures compiler/guest provenance.

Java/Graal parity candidate: `ores-truffle-oreslang/oreslang-source.java#3019` (Java CI awaiting approved private carrier). Rust/Wasm parity evidence: `atto-lang/atto-source.rs#53`. Canonical language contract: `atto-lang/atto-spec#6`.
