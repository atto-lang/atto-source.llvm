# Native LLVM parity gate: `any`, `unknown`, ternary, and runtime guards

Status: staged admission boundary. This document does **not** claim positive native execution parity.

Canonical source contract is defined by `oreslang-common-source/docs/TERNARY_ANY_UNKNOWN_RUNTIME_GUARDS_V1.md`.

## Reserved surface

The native frontend reserves lowercase `any` and `unknown` as language type keywords and lexes `?` as a source symbol. Current native main is deliberately restricted to signed-i64 scalar expressions, so it must reject these forms before emitting LLVM IR:

```ores
pub fnc normalize(any value): int {
    return Integer.is_int(value) ? value : 0;
}
```

Rejecting is preferable to representing `any` as an untagged i64, because that would make `Integer.is_int` trivially true and would erase ownership/capability provenance.

## Positive native lowering prerequisites

Positive parity requires all of the following in the same compiler path:

1. A canonical opaque value ABI with at least a non-forgeable runtime type tag/descriptor plus payload representation.
2. A real `bool` expression/result path usable as a conditional predicate. The existing typed-bool work must be integrated rather than bypassed.
3. `Integer.is_int` lowering to a trusted tag/descriptor comparison, never an ordinary user-resolvable method call.
4. A conditional-expression AST with right-associative parsing, exactly-once condition evaluation, exactly-one-branch execution, and a typed branch merge.
5. Flow-sensitive narrowing of stable bindings in the true branch.
6. Ownership/drop rules for opaque payloads. Capability-bearing, borrowed, actor, channel, future, native-I/O, and other affine values must not be hidden in a transportable dynamic box.
7. ABI/version identity shared with other AOT backends. Unknown/reserved tags must fail closed.
8. O0/O2 compiled guest execution covering both an integer value and at least one genuinely non-integer value. An i64-only test cannot prove dynamic discrimination.
9. Negative no-IR tests for spoofed `Integer`, unsupported descriptor targets, malformed/unknown tags, branch ownership conflicts, and opaque transport.

## Current gate

Until those prerequisites are implemented and executed, the native compiler rejects opaque top types and `?:` with dedicated diagnostics and no LLVM IR. This keeps source compatibility explicit without overstating backend parity.
