# Logical word operators: native parity gate (2026-10-17)

Java/GraalVM defines reserved, case-sensitive infix `and` and `or` as aliases for `&&` and `||`. They operate only on `bool`, short-circuit left to right and share precedence with their symbolic counterparts. Core Boolean intrinsics support `And(bool, bool, ...)`, `Or(bool, bool, ...)` with two or more scalar arguments; `Xor(bool, bool)` requires exactly two scalar arguments. The existing one-argument `Xor(Array<bool>)` parity reducer remains an explicitly distinct compatibility overload.

The LLVM signed-i64 source frontend does **not** have typed bool lowering, full Boolean expressions, or BooleanOps intrinsics. Reserve `and` and `or` now to prevent them from being admitted as ordinary identifiers on native while Java treats them as operators. Do not accept source expressions containing these operators until LLVM supports Boolean IR, short-circuit CFG edges, effect/ownership sequencing, and scalar/intrinsic call dispatch.

Parity exit criteria:
1. Typed native Boolean constants and expressions are representable end to end.
2. Mixed word/symbol spelling produces the same result and short-circuit behavior.
3. `Xor` exactly-two-scalar-argument checks agree between compile-time and runtime; existing list reductions have the established identities and parity behavior.
4. Native diagnostics reject invalid operands, extra args, and abandoned right-hand effects without falling back to signed-i64 semantics.
5. Run the same exact-tree conformance fixtures for Java/Graal JIT, native-image/AOT and LLVM. Zero-step Actions failures are not test results.

Cross-reference: `ores-truffle-oreslang/oreslang-source.java/docs/BOOLEAN_LOGICAL_CONTRACT.md`.

This change is a **fail-closed parser hardening step**, not a claim that LLVM already executes this Boolean syntax.
