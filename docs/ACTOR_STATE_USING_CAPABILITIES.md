# Actor state capability scopes: `using`, `critical`, and ownership

Status: **restricted LLVM executable profile; Java compiler fail-closed**.
Updated October 12, 2026.

## Canonical scoped capability syntax

```ores
define actor Worker as
    state { value: int = 0; }
end
```

Actor class data is permitted **only** within one fixed-key `state` map;
methods may be declared without state. `self.state`, direct actor fields,
and assignments such as `value = 10` outside a state capability fail.

The first executable native profile admits these value-returning scopes
inside an exhaustive typed `receive` / `receive_forward` /
`receive_backward` arm:

```ores
const seen = using state.read(|state| -> {
    return state.value;
});

const next = using state.write(critical |state| -> {
    state.value = state.value + 1;
    return state.value;
});
```

Legacy `self.handle_state(critical |state| -> {...})` remains the
**write** capability equivalent, not a read capability. All results are
**copied `int` scalars**, never references to the state map.

## Capability matrix

| Access | `using state.read(...)` | `using state.write(critical ...)` |
|---|---|---|
| Read declared state keys | Yes | Yes |
| Mutate a declared state key | **No; compile error** | Yes |
| Return copied `int` result | Yes | Yes |
| Return/capture state map reference | **No** | **No** |
| Await, select, nest a loan, or call arbitrary functions | **No** | **No** |
| Reenter actor methods while holding a loan | **No** | **No** |
| Reach actor properties outside `state` | **No** | **No** |

The callback ends synchronously before any subsequent `self.forward()` or
`self.backward()`. Nested capability scopes and suspending operations
are rejected before LLVM IR is emitted; the loan cannot be promoted,
extended, stored, or returned.

`critical` on `state.write` is deliberately mandatory. The present
native scheduler enforces **single-threaded actor execution**; write
exclusivity derives from that lane and its actor-owned heap turn, not
from a separate general-purpose reader/writer mutex. In particular,
**`using state.read` does not currently run multiple readers in
parallel** and is *not* proof of a multi-reader concurrency feature.
Future `critical read` / `critical write` scheduling or off-lane
immutable snapshots require their own formal rules and runtime tests.
Do not weaken the single-flight actor invariant to enable parallel readers.

## Audited failure modes

- No class-level `let`/`val`/`const` actor data.
- No duplicate state maps, unknown keys, or reserved capability names.
- No write through a read scope.
- No write loan missing its `critical` keyword.
- No `critical` write syntax on a read scope.
- No forged `state.take` or other unsupported capability.
- No `await`, `select`, nested `using`, or loan-reference return.
- No arbitrary guest function call inside a state callback; this blocks
  synchronous reentrancy while the state loan is active.
- Native read-only fixture IR contains **no call** to
  `ores_actor_i64_heap_field_write`; write fixture IR contains the call.
- Clang AddressSanitizer and UndefinedBehaviorSanitizer execute real
  15-stage pipelines with both capability modes.

## Evidence and limitations

Native: [exact source funded Actions #38313725825](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38313725825)
passed **45/45 selected CTests**, with 61 Git source blobs verified
prior to compilation. The 15-actor read and write pipeline examples
complete and return the expected result on the real native runtime.
These tests are a targeted subset, not the full LLVM repository suite.

Java: [focused JDK 25 Actions #38313160385](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38313160385)
passed **25 parser/compiler tests**. Java explicitly diagnoses
`using state.read/write` as requiring unavailable scoped capability
lowering, rather than treating it as an ordinary guest method call.
Java actor state and directional guest execution are still not runnable.

The restricted native implementation supports one synchronous loan per
tagged match arm and fixed `int` keys. General state values,
reader/writer lock scheduling, multi-flow message concurrency, full
`using` statement syntax, async-safe immutable snapshots, and Java/LLVM
executable parity remain **unimplemented**. Maintain both PRs in draft.
