# Actor state access: canonical `self.handle_state` contract

> **Pre-1.0 design update (2026-10-12):** The normative contract is
> [WITH_USING_STATE_CONTRACT.md](WITH_USING_STATE_CONTRACT.md):
> `with` declares permitted effects, `using` acquires lexical capabilities,
> `self.handle_state(|state|...)` reads by default, and `critical`
> grants exclusive write access. Historical syntax discussions below
> are retained for audit history but are superseded where they disagree.


Status: **restricted LLVM compiled execution verified; Java compiler fail-closed**.
Updated October 12, 2026.

## Normative source syntax

An actor has methods and at most one fixed-key `state` map; ordinary
actor instance `let`, `val` and `const` properties are not allowed.

```ores
define actor Counter as
    state { value: int = 0; }

    // Callback is implicitly read-only if no "critical" modifier.
    read_value(): int {
        const value = self.handle_state(|state| -> {
            return state.value;
        });
        return value;
    }

    // The "critical" modifier grants exclusive write authority.
    increment(): int {
        const next = self.handle_state(critical |state| -> {
            state.value = state.value + 1;
            return state.value;
        });
        return next;
    }
end
```

The source above specifies **the intended language API**. The current
restricted LLVM compiler supports the two callback spellings in exhaustive
tagged receive arms, **not yet in ordinary actor methods**. Do not label
the complete class-method example runnable until general method-state
lowering is implemented and tested. The current Java compiler deliberately
rejects either spelling before guest execution.

For this API, the name is `self.handle_state` (dot access), not
`self_handle_state` as a standalone global function.

## Borrow/capability rules

| Contract | `self.handle_state(|state| -> {...})` | `self.handle_state(critical |state| -> {...})` |
|---|---|---|
| Borrow category | Immutable/read-only state loan | Exclusive/mutable state loan |
| Read declared fixed keys | Yes | Yes |
| Write declared fixed keys | **Compile error** | Yes |
| Copy an `int` result out | Yes | Yes |
| Return/retain the state reference | **Forbidden** | **Forbidden** |
| Suspend via `await` or `select` | **Forbidden** | **Forbidden** |
| Nest a state loan or invoke arbitrary guest code during a loan | **Forbidden in native subset** | **Forbidden in native subset** |

The permission applies **only** inside its callback. The result must
not contain a borrowed state object or pointer; the current executable
subset copies an `int` scalar. The actor's private state remains
unavailable in `self.state`, from an ordinary member field, or
outside the callback. The compiler must reject a write through a read
loan even if the surrounding actor method is otherwise mutable.

Oreslang actors remain **single-threaded at any instant**. The read-only
mode does not currently admit simultaneous executing readers; parallel
read snapshots would require a separate scheduler and isolation design.

## `using` and `critical`

The **canonical user-facing actor-state API is `self.handle_state`**.
The presence of `critical` selects the mutable write capability; its
absence means read-only. The earlier prototype's
`using state.read(|state| -> {...})` and
`using state.write(critical |state| -> {...})` currently continue to
compile in the restricted LLVM profile as implementation-era aliases,
**not** as the preferred language design. The `using` keyword should
continue to denote lifetime/resource management where appropriate;
it need not be part of ordinary actor state access. Removing these
prototype aliases can be decided separately before the 1.0 syntax
freeze.

## Verified implementation boundary

- **LLVM:** [funded exact-source Actions run #38314809579](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314809579)
  passed **57/57 focused CTests**. It includes compiled 15-actor forward/
  backward pipelines with both canonical forms, LLVM IR checks proving
  read-only callbacks cannot emit actor-heap writes, and Clang
  AddressSanitizer/UndefinedBehaviorSanitizer execution.
- **Java/GraalVM:** [funded JDK 25 run #38314867754](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38314867754)
  passed **27 focused parser/compiler checks**. Both canonical forms
  explicitly fail closed because scoped guest state access isn't
  implemented; neither form runs on Java yet.

Full native repository regressions, historical `let` actor-field
migration, ordinary-method callback support, async/suspend-aware loans,
general state value types, multiple concurrent flows, reader snapshot
semantics and Java/LLVM executable parity are still release blockers.
**Keep both PRs draft and do not claim full feature parity.**
