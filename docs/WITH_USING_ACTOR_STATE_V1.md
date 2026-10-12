# Oreslang pre-1.0 contract: `with`, `using`, `critical`

**Status: draft; normative syntax target, not a cross-backend release certification.**
Updated October 12, 2026. This contract supersedes historical use of
`using` as a callable effect clause.

## Language semantics

| Source form | Meaning |
| --- | --- |
| `fnc ... with state.read, state.write` | A statically verified upper bound on a callable's effects; no state capability is acquired |
| `using state.read(|s| -> ...)` | Acquires an immutable lexical actor-state loan |
| `using state.write(critical |s| -> ...)` | Acquires an exclusive mutable lexical actor-state loan |
| `self.handle_state(|s| -> ...)` | Sealed intrinsic; immutable read loan by default |
| `self.handle_state(critical |s| -> ...)` | Sealed intrinsic; exclusive mutable loan |
| `critical read` / `critical write` | Explicit spellings for read/write callback modes |

`with` and `using` are **both reserved keywords**. They are not aliases:
`with` never grants authority, while `using` never declares a callable
effect row. Legacy signature-level `using state.read` is rejected in the
canonical pre-1.0 grammar; migrate fixtures rather than silently accepting it.

```oreslang
define actor Counter as
    state { x: int = 0; }

    get(): int with state.read {
        return using state.read(|s| -> { return s.x; });
    }

    increment(): int with state.read, state.write {
        return using state.write(critical |s| -> {
            s.x = s.x + 1;
            return s.x;
        });
    }

    reset(): int with state.write {
        return self.handle_state(critical |s| -> {
            s.x = 0;
            return 0;
        });
    }
end
```

A direct `self.handle_state(|s| -> ...)` is read-only even if its caller
declares `with state.write`. An assignment of a literal requires
`state.write`; reading a field requires `state.read`; read-modify-write
requires both. A wider effect row does not upgrade a read loan.

## Safety requirements

1. An actor has at most one fixed-key `state { ... }` aggregate. Ordinary
   persistent actor fields, direct `self.state`, and user-defined
   `handle_state` methods are forbidden in the final language.
2. State access is only through compiler-recognized lexical callbacks.
   Loan handles and state-backed references cannot escape via return,
   closure capture, `rt take`, `rt share`, `rt borrow`, `rt copy`,
   FFI, messages, or arbitrary guest calls without a separate verified
   snapshot/value-transfer contract.
3. A live loan cannot cross `await`, `select`, channel suspension,
   `rt cooperate`, cancellation handoff, or an unproved preemption point.
   Neither `with suspend` nor `critical` waives this restriction.
4. Nested loans are rejected in guest source until reentrancy, loan lifetime,
   and independent scheduler admission are proven.
5. Missing/unknown effects, unknown callee effects, illegal state keys,
   incompatible capability modes, and forged intrinsic metadata fail closed.
6. Actor guest execution remains single-flight: multiple pending read
   tickets do not mean concurrent native execution of that actor.

## Verification and integration boundary

The Java/GraalVM branches #760 and #765 implement parser/AST and some
type/effect checks. Native LLVM branch #244 and companion #259 implement a
restricted synchronous fixed-int tagged-receive profile; branch #257
supplies independent native admission/loan checks. Neither backend has
proven complete method-level executable parity with the other.

Before merge: migrate old fixtures, pass exact-tree full Maven/JUnit and
native CTest, run Graal Native Image AOT/hybrid, ASan/UBSan, negative
ownership/suspension tests, typed guest-state round trips, and cross-backend
shared JSON Schema/TypeSpec conformance. Treat focused compiler suites as
partial evidence only. Do not infer parallel readers or release readiness.
