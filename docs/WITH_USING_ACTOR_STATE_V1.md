# Oreslang pre-1.0 actor state: `with`, `using`, and `critical`

**Contract version:** draft v1, October 12, 2026. This document supersedes
older suggestions that used signature-level `using` for effects. It is a
language-level target with restricted compiler implementations, **not** a
claim of end-to-end Java/GraalVM–LLVM parity.

## Meaning of each construct

- `with` (following a callable signature) is an **effect upper bound**.
  It cannot grant access to an object, state, lock, or actor.
- `using state.read(...)` acquires a **read-only lexical state loan**.
- `using state.write(critical ...)` acquires a **mutable lexical state
  loan** under exclusive logical state admission.
- `critical` denotes exclusive logical admission; it is neither a
  transitive function effect nor an operating-system mutex held across await.
- `self.handle_state` is a sealed intrinsic beneath these forms. It must
  never become a normal overridable method or ordinary field access.

## Canonical user-facing syntax

```oreslang
define actor Counter as
    state { x: int = 0; }

    read(): int with state.read {
        return using state.read(|s| -> { return s.x; });
    }

    increment(): int with state.read, state.write {
        return using state.write(critical |s| -> {
            s.x = s.x + 1;
            return s.x;
        });
    }
end
```

Java/GraalVM parses these method-level forms and lowers the scoped
`using` expression to a compiler-owned sealed `self.handle_state` call
with `critical read` / `critical write` metadata. The existing Java
`$using$` internal annotation continues to store effect bounds during
migration; it is not a source-level capability and must eventually be
renamed to an internal effect-specific identity.

LLVM currently compiles only a **restricted fixed-int, synchronous,
tagged receive-arm** profile. Its equivalent syntax is:

```oreslang
receive(PipeMessages m) with state.read, state.write {
    do match m over {
        on Signal.Data as n {
            const next = using state.write(critical |state| -> {
                state.value = n + 1;
                return state.value;
            });
        }
    }
    return 0;
}
```

## Lexical and grammar separation (audit hardening, October 12)

Both `with` and `using` are **reserved keywords** in the Java and
restricted native source lexers. Neither can be an ordinary local, parameter,
actor state key, or callable identifier. A `with` declaration is metadata,
not an operation: it never manufactures a `StateAccess` loan or lock.

```ebnf
EffectClause = "with" Effect ("," Effect)* ;
Effect       = "state.read" | "state.write" | "suspend"
             | "io" | "alloc" | "block" | "ffi"
             | "throw" | "raise" | "panic" ;
StateScope   = "using" "state" "." "read" "(" InlineLambda ")"
             | "using" "state" "." "write" "(" "critical" InlineLambda ")" ;
```

The full effect vocabulary above is a *language target*. LLVM's restricted
native parser currently checks only state.read and state.write. Explicit
`with` bounds are optional and may over-approximate the actual effects;
when present, an operation outside them must be rejected. A lexical loan
cannot be created by naming an effect.

Java's scoped `using state.read/write` expression is accepted only in
actor instance methods. Its source-to-AST lowering is compiler-owned.
Free functions, static actor methods, and ordinary class methods cannot
acquire this capability merely by declaring `with state.read`. Type-checking
and runtime admission remain independent gates. Passing a parser check does
not confer runtime authority.

Java currently preserves the old signature `using` spelling solely for
stacked legacy tests, and emits a deprecation warning. The canonical
`with` spelling has a distinct internal `$with# Oreslang pre-1.0 actor state: `with`, `using`, and `critical`

**Contract version:** draft v1, October 12, 2026. This document supersedes
older suggestions that used signature-level `using` for effects. It is a
language-level target with restricted compiler implementations, **not** a
claim of end-to-end Java/GraalVM–LLVM parity.

## Meaning of each construct

- `with` (following a callable signature) is an **effect upper bound**.
  It cannot grant access to an object, state, lock, or actor.
- `using state.read(...)` acquires a **read-only lexical state loan**.
- `using state.write(critical ...)` acquires a **mutable lexical state
  loan** under exclusive logical state admission.
- `critical` denotes exclusive logical admission; it is neither a
  transitive function effect nor an operating-system mutex held across await.
- `self.handle_state` is a sealed intrinsic beneath these forms. It must
  never become a normal overridable method or ordinary field access.

## Canonical user-facing syntax

```oreslang
define actor Counter as
    state { x: int = 0; }

    read(): int with state.read {
        return using state.read(|s| -> { return s.x; });
    }

    increment(): int with state.read, state.write {
        return using state.write(critical |s| -> {
            s.x = s.x + 1;
            return s.x;
        });
    }
end
```

Java/GraalVM parses these method-level forms and lowers the scoped
`using` expression to a compiler-owned sealed `self.handle_state` call
with `critical read` / `critical write` metadata. The existing Java
`$using$` internal annotation continues to store effect bounds during
migration; it is not a source-level capability and must eventually be
renamed to an internal effect-specific identity.

LLVM currently compiles only a **restricted fixed-int, synchronous,
tagged receive-arm** profile. Its equivalent syntax is:

```oreslang
receive(PipeMessages m) with state.read, state.write {
    do match m over {
        on Signal.Data as n {
            const next = using state.write(critical |state| -> {
                state.value = n + 1;
                return state.value;
            });
        }
    }
    return 0;
}
```

 annotation; the legacy
form retains `$using# Oreslang pre-1.0 actor state: `with`, `using`, and `critical`

**Contract version:** draft v1, October 12, 2026. This document supersedes
older suggestions that used signature-level `using` for effects. It is a
language-level target with restricted compiler implementations, **not** a
claim of end-to-end Java/GraalVM–LLVM parity.

## Meaning of each construct

- `with` (following a callable signature) is an **effect upper bound**.
  It cannot grant access to an object, state, lock, or actor.
- `using state.read(...)` acquires a **read-only lexical state loan**.
- `using state.write(critical ...)` acquires a **mutable lexical state
  loan** under exclusive logical state admission.
- `critical` denotes exclusive logical admission; it is neither a
  transitive function effect nor an operating-system mutex held across await.
- `self.handle_state` is a sealed intrinsic beneath these forms. It must
  never become a normal overridable method or ordinary field access.

## Canonical user-facing syntax

```oreslang
define actor Counter as
    state { x: int = 0; }

    read(): int with state.read {
        return using state.read(|s| -> { return s.x; });
    }

    increment(): int with state.read, state.write {
        return using state.write(critical |s| -> {
            s.x = s.x + 1;
            return s.x;
        });
    }
end
```

Java/GraalVM parses these method-level forms and lowers the scoped
`using` expression to a compiler-owned sealed `self.handle_state` call
with `critical read` / `critical write` metadata. The existing Java
`$using$` internal annotation continues to store effect bounds during
migration; it is not a source-level capability and must eventually be
renamed to an internal effect-specific identity.

LLVM currently compiles only a **restricted fixed-int, synchronous,
tagged receive-arm** profile. Its equivalent syntax is:

```oreslang
receive(PipeMessages m) with state.read, state.write {
    do match m over {
        on Signal.Data as n {
            const next = using state.write(critical |state| -> {
                state.value = n + 1;
                return state.value;
            });
        }
    }
    return 0;
}
```

. They are treated as the same **effect bound** by
the effect checker, not as state-capability grants. The eventual no-legacy
migration must remove the old form and update historical tests together.

`rt take`, `rt share`, `rt borrow`, and `rt copy` on actor state-backed
projections are rejected in the current Java checker. A future safe typed
copy or immutable snapshot must carry an explicit no-escape proof instead
of using the live ephemeral state loan.

## Mandatory checks

1. `with state.write` never upgrades a read-only loan.
2. A write-capable loan does not override omitted `with state.write`.
3. Reading a state value contributes `state.read`; assigning a literal
   contributes `state.write`. Read-modify-write requires both.
4. Every state key must be declared in the actor's single fixed-key `state`
   map. Direct `self.state` and ordinary persistent actor fields are
   forbidden.
5. The loan stays inline and lexical. No loan or state-map reference may
   escape through return, closure capture, FFI, field storage, or messaging.
6. Live loans cannot cross `await`, `select`, `rt cooperate`, blocking
   calls, or an unsafe implicit preemption point.
7. A read loan cannot be silently upgraded to write while live. Nested
   loans require explicitly verified continuation and lifetime fencing.
8. Actor guest code remains **single-flight** even when logical read
   tickets can coexist. Shared-reader parallel guest execution is not
   established and must not be inferred from this syntax.
9. Effects in `with` are strict, transitive upper bounds. Unknown
   effects/callees must fail closed; `io`, `suspend`, `alloc`,
   `throw`, etc. require separate verified implementation.

## Breaking syntax migration

The historical Java `fnc ... using state.read` signature remains
temporarily parsed by the stacked PR for older fixtures; it is **deprecated**.
The canonical signature is `fnc ... with state.read`. Old signature
spelling must be removed only with a complete test/fixture/doc migration.

Earlier compiler branches disagree on explicit
`self.handle_state(critical read|write ...)` versus implicit read /
`critical` write. The `using` forms above avoid exposing that difference,
but **direct handle_state parity is still an open conformance gate**.

## Implementation / release status

- Java/GraalVM: source parser desugaring plus existing sealed state/effect
  verifiers; run full Maven/JDK25, guest runtime, Native Image AOT/hybrid,
  escape/suspension and callback reentrancy regressions on exact source.
- LLVM: native tagged-arm compilation and effect-bound checks with CTest
  positive/negative IR and execute fixtures; full CTest, sanitizers,
  actor-method lowering, and state-value generalization remain gating.
- Shared fixture equivalence, JSON Schema/TypeSpec source-contract
  conformance, formal interleaving proofs, and full cross-backend guest
  execution remain mandatory before claiming feature completion.
