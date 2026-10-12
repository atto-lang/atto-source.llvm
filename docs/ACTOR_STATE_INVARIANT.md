# Actor instance-data isolation contract (draft, 2026-10-12)

## Normative rule

Actor declarations may contain methods and one compiler-owned `state { ... }`
map. They may not contain ordinary instance `let`, `val`, `const`, or
user-defined class data properties. State map keys are fixed at compile time;
they are never reflected as ordinary actor methods or public fields. An actor
may also have **methods only**, with no state map.

```ores
define actor Worker as
    state {
        value: int = 0;
    }
    pub fnc helper(int value): int { return value + 1; }
end
```

The canonical API is `self.handle_state`: no modifier means a
read-only state loan; `critical` grants exclusive mutable access. The
restricted LLVM tagged-message compiler supports both synchronous forms:

```ores
const current = self.handle_state(|state| -> {
    return state.value;
});

const next = self.handle_state(critical |state| -> {
    state.value = n + 1;
    return state.value;
});
self.forward(Signal.Data(next));
```

Its result is a **copied primitive**; it cannot be a state reference.
`self.state`, raw `value` access, raw class properties, duplicate maps,
unknown keys, and returning `state` itself are rejected. Callback-owned
state values cannot cross an `await`, `select`, or other suspension
boundary in this restricted profile. The native scheduler enforces
single-flight actor execution; the parser/lowerer does not implement a
general reader-writer lock or async state loans.

## Evidence

LLVM: [57/57 focused native CTests, with Clang ASan/UBSan](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314809579).

Java: [27 exact-source JDK25 parser/compiler checks](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314867754).
The Java `Ast.ActorStateSpec` records fixed-key state independently from
`ClassDecl.fields`; annotation expansion and tree shaking preserve it.
The Java checker **rejects** state execution until the scoped state-loan
runtime is built. A parsed declaration is not runnable.

## Unresolved release gates

The restricted native profile only supports fixed `int` keys and
synchronous state loans in tagged receive arms; general actor methods,
dynamic state maps, `critical read`/`critical write`, async suspension,
zero/many pipeline emissions, multiple concurrent flows, full source-tree
regressions, JVM/GraalVM guest execution, cross-backend parity and Native
Image are not yet verified.

Older fixtures/PRs that declare actor instance data directly need migration
to the new syntax before either draft PR may merge.
