# Critical read/write + using: LLVM admission parity (pre-1.0)

Normative replacement for the older unqualified `critical |s|` state callback.
The legacy discussion in `CRITICAL_HANDLE_STATE_USING_PARITY.md` is historical
where it conflicts with this contract.

## Source grammar

```oreslang
define actor Counter as
    state { value: int = 0; }
    get(): int using state.read {
        return self.handle_state(critical read |s| -> s.value);
    }
    set(int x): void using state.write {
        self.handle_state(critical write |s| -> { s.value = x; });
    }
    increment(): int using state.read, state.write {
        return self.handle_state(critical write |s| -> {
            s.value = s.value + 1;
            return s.value;
        });
    }
end
```

- Ordinary `critical` is synchronization/admission, **not state authority**.
- `critical read` admits a readonly state capability; `critical write`
  admits an exclusive read/write capability. Bare `critical` and implicit
  lambda modes are rejected at `handle_state`, including on serial actors.
- `using` is an effect upper bound, not a capability. `using state.write`
  never authorizes mutation from `critical read`; `critical write` never
  bypasses a missing `state.write` effect bound. A pure assignment needs
  only `state.write`; read-modify-write needs both.
- `requires no_block`, `pure`, and `critical` remain orthogonal to
  effect declaration and permission checking.
- State loans are actor-owned, nonescaping, generation-checked, and must not
  remain live across suspension. The actor retains at most one carrier running
  guest code. Shared logical reader grants require separate scheduler proof.
- This branch updates the native `CriticalHandleStateContract` admission
  policy and focused C++ tests only. LLVM's restricted guest frontend **still
  rejects general `state { ... }` source**; there is no claim of executable
  Java/LLVM actor-state parity or reader-writer scheduler concurrency yet.

## Native policy regression expectations

| Callback | Inferred effects | Declared using | Expected |
| --- | --- | --- | --- |
| read | read | read | Accept |
| read | read | write | EffectExceedsUsing |
| read | read, write | read, write | ReadModeCannotWrite |
| write | write | write | Accept |
| write | read, write | write | EffectExceedsUsing |
| write | read, write | read, write | Accept |
| none | any | any | MissingCallbackCritical |

The next gates are shared source fixtures, JSON Schema/TypeSpec differential
validation, native compiled actor-state lowering, full sanitizer CI, and a
model-checked scheduler proof of concurrent logical readers and writer fairness.
