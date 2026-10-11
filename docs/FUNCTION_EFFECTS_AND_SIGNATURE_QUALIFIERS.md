# Function effects, `using`, `requires`, and signature qualifiers

**Status (2026-10-11): design / cross-backend contract, NOT a parser or typechecker implementation.**

## Grounded implementation audit

- **Java/GraalVM:** the inspected Lexer/Parser recognizes `nlex` and `trap` and has
  callable-modifier restrictions. The inspected lexer does **not yet recognize**
  `pure`, `critical`, `using`, or `requires` as source keywords.
- **LLVM:** the narrow native frontend reserves `critical`, `nlex`, and
  `trap` but rejects unsupported `critical`/exception source lowering.
  The inspected native frontend does **not yet implement** effect clauses.
- Java `docs/PURE.md` specifies `pure` as a future globally reserved,
  compiler-enforced *observational* purity contract. This is **not yet the
  current lexer state**.
- Java `docs/TRAP.md` specifies `trap` as `throw` -> `None`,
  success -> `Some(value)`, while `raise`, `panic`, cancellation and
  termination bypass it. Java's current parser rejects `async trap`,
  `trap` generators and actor trap callables pending correct lowering.
- `nlex` is the established no-lexical-activation-capture qualifier;
  do not reinterpret it as “non-exceptional,” “non-leaving,” or pure.
- These are separate from `critical`, a logical scheduler-owned admission
  and nested-continuation ownership property. Do not lower a critical method
  to an OS mutex held across suspension.

## Selected proposed surface syntax

```oreslang
pure fnc add(int a, int b): int {
    return a + b;
}

fnc get_count(): int using state.read {
    return self.handle_state(|s| s.count);
}

fnc increment(): int using state.read, state.write {
    return self.handle_state(|s| {
        s.count += 1;
        return s.count;
    });
}

async fnc fetch(): string using io, suspend {
    return await http.get("/data");
}

nlex fnc compute(int x): int using alloc {
    // `nlex` still means no enclosing activation capture.
    return x + 1;
}

trap fnc load(): User using io {
    return db_load();
}

critical fnc update(): int
    using state.read, state.write
    requires bounded, no_alloc, no_block
{
    return self.handle_state(|s| {
        s.count += 1;
        return s.count;
    });
}
```

**All examples using `using`, `requires`, `pure`, or `critical`
above are target syntax, not assertions that either compiler accepts them.**
`using` and `requires` should be contextual *signature* keywords where
unambiguous; preserve `with` as an independent existing construct.
`pure` follows the existing draft PURE.md requirement to become globally
reserved when that feature ships.

Effects are inferred for unannotated functions. Explicit `using` clauses
are upper bounds / API promises; a body may use fewer effects but never
more. Imports, callbacks, generics, dynamic dispatch, and function values
must carry compatible effect summaries. Unknown calls conservatively carry
unknown effects and cannot meet a stricter contract without proof.

## Effect kinds and checking rules

| Effect | Meaning |
| --- | --- |
| `state.read` | Observe actor-owned mutable state (ambient, therefore incompatible with `pure`) |
| `state.write` | Mutate actor state (requires state capability and logical lane) |
| `suspend` | May release logical execution admission at a compiler-approved boundary |
| `io` | External I/O or ambient mutable-world observation |
| `alloc` | Potential allocation/resource cost, **not intrinsically impure** |
| `block` | May block a physical carrier |
| `ffi` | Foreign effect without stronger trusted summary |
| `throw<E>` | Ordinary typed exceptional control |
| `raise<E>` | Non-trappable exceptional escalation |
| `panic` | Fatal/invariant failure control |

The `pure` qualifier prohibits ambient state observation, I/O and
externally visible mutation, actor messaging/spawning, scheduler/time
inspection, and untrusted foreign effects. **Pure may allocate invocation-
private memory, throw, panic, or diverge** subject to distinct effect facts.
Do not redefine `pure` to mean `no_alloc`, `no_throw`, or `bounded`.
See Java `docs/PURE.md` for the complete contract.

The `trap` qualifier is a **control-effect handler**, not a broad purity
restriction. A sync `trap fnc(): T` exposes `Option<T>` at a callsite.
It consumes escaping *ordinary* `throw`, not `raise`, `panic`, or
cancellation. An async `trap` target would expose
`Future<Option<T>>`, but is not currently admitted by Java's parser.
Internal body effects and outward call effects can differ because `trap`
discharges an ordinary throw after cleanup; external I/O is not discharged.
Never collapse `throw`, `raise`, and `panic` into one `panic` effect.

## Sealed actor-state intrinsic

`handle_state` is a sealed, non-overridable, capability-checked intrinsic.
Its callback is **effect-polymorphic**: operations on `StateAccess`
contribute their precise state read/write effects, and callback/callee
effects (including `suspend`) propagate out. Do not make a read-only
callback automatically a state writer.

Conceptual type representation (NOT valid current source syntax):

```text
handle_state<T, effect-row E>(
  callback: fnc(StateAccess) -> T using E
): T using E
```

`E` is an **effect-row variable**, distinct from a value/type generic `T`.
Effect-row binding, subtyping, and function type grammar must be specified
before adding a concrete surface form. There is no implicit catch, trap,
or loss of the callback's `throw`/`raise`/`panic` effects.

**Reentrancy:** nested synchronous `critical` calls by the *same logical
continuation* reuse its logical ownership. A distinct continuation, even
on the same actor, cannot acquire an active exclusive gate. An actor-state
access loan is a separate, narrower capability: nested state callbacks
fence the outer loan while the inner loan is active. A state-backed
mutable loan cannot cross a boundary admitting other continuations;
yielding a `handle_state` callback must revoke/segment loans and later
revalidate capabilities. No monitor or native mutex spans await.

## Real-time requirements are not effects

```oreslang
fnc tick(): int
    using state.read, state.write
    requires bounded, no_alloc, no_block
{
    return self.handle_state(|s| { s.count += 1; return s.count; });
}
```

`requires` is a verified constraint: `bounded` requires an actual
target-specific bound, not an estimated reduction count. `no_alloc` and
`no_block` constrain transitive effects. Even `pure` code can diverge,
allocate, or run longer than a deadline. A strict real-time profile must
add WCET, memory/stack, scheduler and platform admission proof.

## Grammar sketch (not implemented)

```ebnf
Callable = Qualifiers? ("fnc" | "routine") Name Params Return?
           EffectClause? RequirementClause? Body
EffectClause = "using" Effect ("," Effect)*
RequirementClause = "requires" Requirement ("," Requirement)*
Effect = "state.read" | "state.write" | "suspend" | "io" | "alloc"
       | "block" | "ffi" | "panic" | "throw" TypeArgs?
       | "raise" TypeArgs? | EffectVariable
Requirement = "bounded" | "no_alloc" | "no_block" | Identifier
```

Function types and method declarations must gain corresponding metadata
and compatibility checks. The clause boundary is after the return type,
not inside the function body. The parser must reject duplicate effect
entries, unknown effect names (unless bound row variables), and
`requires no_alloc` alongside a provable `alloc` effect.

## Executable conformance milestones

1. Snapshot existing `nlex` and `trap` behavior, including rejected
   async/generator/actor combinations.
2. Reserve/parse `pure` according to PURE.md and add observable-effect
   checking; never accept a decorative purity marker.
3. Parse `using`/`requires` in Java and LLVM, preserving contextual
   `with` syntax; add positive/negative parser fixtures.
4. Infer transitive effects; enforce explicit promises; type effect rows
   on callbacks, imported symbols and dynamic calls.
5. Verify `trap` ordinary-throw discharge and distinct raise/panic channels.
6. Lower sealed `handle_state` with nested same-continuation scope fencing
   and precise read/write/suspend inference.
7. Prove borrow-lifetime and reentrancy safety at every reduction boundary.
8. Apply real-time certification/admission; run *identical guest source*
   through both compilers and verify exact-tree CI. Tests of host ABI alone
   are not guest-language parity.

**Release gate:** Documentation and native host tests do not imply that
new signatures or suspendable actor state compile.
