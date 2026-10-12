# Critical / using / with — LLVM admission contract (pre-1.0)

This document supersedes the older `critical |s|`/signature-`using`
effect conventions in `CRITICAL_HANDLE_STATE_USING_PARITY.md`.

## Language contract (shared with Java/GraalVM)

- `with` declares the upper bound on function effects.
  `with state.read, state.write` grants *no* state resource or lock.
- `using` acquires a scoped capability: `using state.read(callback)`
  for a readonly loan, `using state.write(critical callback)` for
  exclusive read/write. It is **not** legal in a function signature.
- `critical` means exclusive admission. Bare `critical |s|` in a
  state scope means WRITE, plain `|s|` means READ. The explicitly
  qualified `critical read` / `critical write` forms remain valid.
- `handle_state` is a sealed actor intrinsic. `using state.read/write`
  lower to it, not to a publicly callable runtime method.
- A pure overwrite needs only `with state.write`, a read only
  `with state.read`, and increment needs both.
- State grants cannot escape across messages, closures, actors, or live
  suspension. Physical actor guest execution remains single-carrier.

```oreslang
define actor Counter as
    state { value: int = 0; }

    get(): int with state.read {
        return using state.read(|s| -> s.value);
    }

    increment(): int with state.read, state.write {
        return using state.write(critical |s| -> {
            s.value = s.value + 1;
            return s.value;
        });
    }
end
```

## Native admission ABI

The host-side `CriticalStateCallback.state_mode` must be **explicitly
lowered** by the compiler to `Read` or `Write`. Its `None` value is
invalid admission metadata, not the Oreslang unmarked-lambda spelling.
The `with_effects` argument is the declared effect upper bound;
`CriticalStateAdmission::EffectExceedsWith` identifies violations.
`ReadModeCannotWrite` rejects writes even with a wide declared `with`.

| Lowered mode | Inferred effects | Declared `with` | Admission |
| --- | --- | --- | --- |
| Read | state.read | state.read | Accept |
| Read | state.read, state.write | both | ReadModeCannotWrite |
| Write | state.write | state.write | Accept |
| Write | state.read, state.write | state.write | EffectExceedsWith |
| Write | state.read, state.write | both | Accept |
| None | any | any | MissingCallbackCritical |

**Verification boundary:** this native C++ admission and scoped-loan ABI
does not prove that the restricted LLVM Oreslang frontend compiles general
`state { ... }` programs. Java/Graal and LLVM compiled guest parity,
logical multi-reader admission, writer fairness, and cross-suspension
leases require separate implementation and evidence.
