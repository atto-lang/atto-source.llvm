# Native terminal `try/catch/finally`: return-snapshot subset

This is a deliberately narrow **nonthrowing** compiler slice. It is not
full exceptional control flow or a substitute for the Java/GraalVM runtime.

Admitted function body shape:

```oreslang
fnc f(borrow mut Counter c, int flag): int {
  defer c.value = c.value + 2;
  try {
    if (flag eq 0) { return c.value; } fi
    return c.value;
  } catch (err) {
    return 0;
  } finally {
    c.value = c.value * 10;
    c.value = c.value + 1;
  }
}
```

The protected `try` may contain only a bounded number of guarded scalar
returns followed by its unconditional final return. Guard operands and return
values are restricted to integer literals, existing scalar bindings, or direct
named class-field reads. Function calls, ownership transfers, and effectful
expressions are rejected here, rather than claiming that native `catch`
can intercept their exceptions. `catch` is required by
the shared Java syntax and may contain **only an integer-literal return**
(or be empty for void/fallthrough cases). It is unreachable by construction:
operations capable of raising source exceptions are not admitted in the
native protected region. Java's total-return checker requires the literal
catch return when the surrounding function is non-void.
The `finally` body admits direct checked calls or class-field writes only.
No `return`, `break`, `continue`, nested `try`, nested `defer`,
locals, closures, throw, await, or suspension are admitted in `finally`.

The frontend checks this subset and lowers the ordered finally actions into
the existing source return-cleanup representation *in reverse registration
order*. On exit the backend first captures the pending scalar return as an
LLVM SSA value, then executes the finally actions in source order, then
executes previously registered outer defers in reverse order. The ownership
pass checks each outgoing return path **after** return evaluation and before
the final resource-release/drop planner runs. It must reject invalid owners,
overlapping borrows, and moved class fields.

## Cross-backend conformance

`tests/fixtures/native-try-finally-return-snapshot.ores` is byte-identical
to Java/GraalVM
`contracts/v1/semantics/fixtures/native-try-finally-return-snapshot.ores`.
Both should return integer `246` through two sequential guarded/fallthrough
returns and observable class-field cleanup effects.

**Not yet supported:** real caught exceptions, general catch handlers,
throw/recover/panic, exceptional failure precedence or suppression,
suspendable/async finally, nested closures inside finally, and general
statement/branch CFG unwinding. Unsupported syntax must be rejected before
IR generation; do not silently treat a catch handler as executable.
