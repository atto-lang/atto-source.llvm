# Core assertions

> **LLVM status:** This is a source/API contract, not an executable native
> standard library. The example below currently runs on Java/GraalVM only.
> Canonical imports and qualified Assert member calls now parse into a
> sealed native AST with explicit alias binding and arity checks. Source-level
> bool return metadata is staged, but LLVM code generation still rejects the
> unlinked standard-library ABI and assertion/trap lowering.

Import the source-owned module with `import module Assert from "std/assert";`.

| Predicate | Meaning | Failure-producing counterpart |
| --- | --- | --- |
| `equal(Any a, Any b)` | Language `eq` equality | `equal_throws(a, b)` |
| `not_equal(Any a, Any b)` | Language `neq` inequality | `not_equal_throws(a, b)` |
| `same(Any a, Any b)` | Language `is` identity | `same_throws(a, b)` |
| `true(bool actual)` | Value is true | `true_throws(actual)` |
| `false(bool actual)` | Value is false | `false_throws(actual)` |

Predicates return bool without failing. Counterparts return true on success and
produce an ordinary runtime failure on mismatch. `Assert.fail(String message)`
unconditionally produces an ordinary failure with the supplied message.
Equality follows the language operators; this module does not introduce recursive
collection comparisons or approximate numeric comparisons.

```ores
import module Assert from "std/assert";

trap fnc check_value(int actual): bool {
  return Assert.equal_throws(actual, 42);
}

pub routine main(): void {
  val Option<bool> good = check_value(42); // Some(true)
  val Option<bool> bad = check_value(7);   // None
  stdio.println(good.is_some());
  stdio.println(bad.is_none());
  return;
}
```

`trap` is currently a callable modifier, not a call-site operator. It wraps normal
returns in Some and ordinary runtime failures in None, without retaining error
detail. Untrapped assertion failures retain the message and can be caught by
ordinary try/catch. Panic, cancellation, process termination, and capability or
permission denials must bypass trap: they are not assertion mismatches.

The source module uses `rt fail(String message): void`, a general runtime failure
operation. The host only transports that failure; predicates and messages live in
Oreslang. This operation deliberately does not enable the reserved `throw`,
`raise`, `panic`, `recover`, or proposed `throws` syntax or typed error effects.
`true` and `false` may name declared callables and qualified members; bare boolean
literals retain their existing meaning. Call these functions through `Assert`.

The LLVM repository carries the same `stdlib/assert.ores` source, but its current
compiler cannot execute it: stdlib imports, Any/bool APIs and ordinary failure/trap
lowering still require implementation. This is not native execution parity.

## Native implementation milestones

The LLVM frontend now distinguishes `Assert.equal(1, 2)` from class-field reads
and ordinary function calls. It rejects duplicate or noncanonical imports,
unknown members, wrong arities, alias shadowing, and unscoped borrows.
These checks do **not** prove `Any` or `bool` native execution: compiler codegen
continues to reject all imported `std/assert` programs before LLVM IR emission.

The native `EffectUnwind` helper has been hardened with 36 pairwise effect
precedence scenarios. An already-pending terminal effect remains primary;
ordinary cleanup throws retain the most recent ordinary failure and preserve
previous failures as secondary diagnostics. Only ordinary `Throw` is marked
eligible for future `trap` capture. This helper is not yet emitted by the
Oreslang compiler. Actual guest-to-guest parity remains a release gate.
