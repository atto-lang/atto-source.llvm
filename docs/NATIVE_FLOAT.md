# Native floating point

`float` and `f64` are the same IEEE binary64 primitive, as on Java/Graal. Native
source now executes float programs end to end: scalars, class fields, function
and method values, `Array<float>`, `Option<float>`, `Result<float, E>`.

## Semantics

| Source | Behavior |
|---|---|
| `val float x = 1;`, float parameter, field, return or element given an int | Widens with `sitofp` |
| `1 + 2.5`, `x < 1`, `c ? 1 : 2.5` | The int operand widens; the result is float |
| `1 / 2` | Real division, `0.5`. `1 / 0` is `Infinity`; the result never assigns to `int` |
| `5.5 % 2`, `5 % 0.0` | IEEE remainder (`frem`). Links libm where `frem` is not folded |
| `int % int` | Integer remainder; still requires a nonzero literal divisor |
| `2 == 2.0` | Exact: `2^53 + 1` does not equal the double it rounds to |
| `1.0e400`, `1.0e-400`, `1_000.5` | Infinity, zero and `1000.5`, as Java's lexer rounds them |
| `<`, `==` with NaN or signed zero | IEEE `fcmp`. See the divergence below |
| `println(x)`, `write(x)` | `java.lang.Double.toString` text: `1.0E10`, `9.9E-4`, `-0.0`, `NaN` |

Nothing narrows implicitly. `val int x = 1.5` and `val int x = 1 / 2` are compile
errors on both backends.

Storage is the checked static type. Stack slots and call ABIs use LLVM `double`.
Record, array and sum slots are 8-byte cells that hold raw binary64 bits, and every
reader recovers the value from the checked static type, never from the bits.

## Source forms added with this work

Java forms the ML guests depend on: `for let i = 0; i < n; i++ do ... done`,
`for int i = ...`, `for const x of xs do`, `while c do`, `loop { }`, `break`,
`continue`, `if c then ... elif d then ... else ... fi`, a void body that falls off
its closing brace, `stdio.stdout.write`, printing any scalar, `rt fail(message)`,
`import fnc a, b from`, `import class A, B from`, `define module m ... end` followed
by file-level `main`, and `m.f()` naming a public member of that module.

A `let` slot holding an immutable leaf record may be rebound: `state = step(rt
borrow state)` copies the new record's fields into the slot once the right side
has fully evaluated. Classes with mutable fields or arrays stay non-replaceable.

## Limits, all fail closed

- `f32`, `decimal` and complex types are rejected. Java runs `f32` as a double, so
  a real binary32 lowering is a language decision, not a port.
- `rt fail` ends the process with status 1 after flushing admitted output. Java's
  `rt fail` is catchable and runs cleanup, so native rejects any program that has
  both `rt fail` and `defer` cleanup or a caught throw. A native `try` body cannot
  call a failing function; it admits only nonthrowing reads.
- Loops, records, strings and console output require the synchronous host main
  region. Programs with actors or suspension are rejected rather than run a
  non-yielding loop on a carrier.
- A record region holds 65,536 live records. Loops release each iteration's records,
  so training loops run indefinitely, unless the program uses `Option`, `Result` or
  record arrays, whose handles can outlive an iteration. Those programs still hit
  the limit, and exit 1 after continuing with zeroed reads.
- A string region holds 65,536 strings. Printing a lone scalar or a literal in a loop
  consumes none per iteration; building strings with `+` in a loop still does.
- Native console output requires stdout to be a pipe, socket or terminal. Redirecting
  stdout to a regular file exits 1 with no output, by the existing strict NIO rule.
- Function parameters of type `float` are by value only; `borrow float` is rejected.
- `Math` functions do not exist on Java either, so neither backend offers them.

## Known divergences from the Java backend

1. **NaN and signed-zero ordering.** Java's `compare` uses `Double.compare`, so
   `NaN > 1.0` and `-0.0 < 0.0` are `true`. Its `NaN == NaN` depends on object
   identity. LLVM follows IEEE. Java PR #1033, open, hardens toward IEEE, and the
   OresLLM guests assume it, so this is treated as a Java bug.
2. **An int in a float slot.** Java leaves the runtime `Long` in place:
   `val float w = 3; println(w)` prints `3`, and `true ? 1 : 2.5` prints `1`. LLVM
   prints `3.0` and `1.0`. Arithmetic that mixes in a real double agrees on both.
3. **Very large exact comparison.** Java compares `int == float` through
   `Double.toString`, so `2^62 == 2^62.0` is `false` there. LLVM compares exactly.

## Evidence

`native_float_ml` builds with the public AOT driver, runs each guest and compares
stdout byte for byte with output captured from the Java backend. Regenerate the
`.expected` files only from a Java run. `native_f64_scalar_e2e` covers IEEE edge
cases. `Double.toString` formatting was separately compared with the JVM over
2,000,000 values, including subnormals and exact powers of two; that comparison is
not part of CTest.
