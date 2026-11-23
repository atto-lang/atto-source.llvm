# Backend-owned Atto library policy slice

This repository (atto-source.llvm) owns its own native `.atto` source.
No dependency on `atto-common-source` is required to build these modules,
and no byte-for-byte source identity with other compilers is mandated.

## Layer boundaries

- `stdlib/policy/core/byte_budget.atto` is freestanding, pure and allocation-free.
- `stdlib/policy/alloc/capacity_plan.atto` is a pure policy helper for an explicit
  allocator. Actual allocation and ownership are NOT implemented by this file.
- `stdlib/policy/alloc/arena_plan.atto` adds overflow-safe reservation and conservative
  rewind/tail-release/close/generation policy. A trusted runtime must enforce
  true ownership, live-loan state, alignment, destructor completion, scrubbing
  and actor incarnation. No heap or arena allocator is exposed by this file.
- `stdlib/policy/std/stream_watermarks.atto` provides stream backpressure decisions.
  Channel admission, actor scheduling, permission checks and I/O are still
  enforced by the runtime.

All public function names are snake_case. Signed byte counts must be nonnegative.
For invalid inputs, predicate functions fail closed, `bytes_left` returns -1,
and `next_capacity` returns -1. Max budgets are inclusive. Growth uses a
backend-local initial capacity floor of 4 items; this value may vary
across compilers without changing safe ownership or streaming contracts.

The LLVM-local `stdlib/policy/alloc/arena_lifecycle.atto` separately checks sealed
admission, completed drops, active turns, continuations, I/O and producer
leases before physical reclamation. `arena_can_close` in `arena_plan.atto`
is **not** an alias for that stronger `arena_can_free` gate. Existing
`arena_limits.atto` validates alignment; arena reservation planning cannot
replace the runtime's aligned placement checks.

## Verification state

`contracts/atto-library-policy-v1.json` records 23 proposed behavior cases.
`contracts/atto-arena-policy-v1.json` pins 38 additional arena-policy vectors,
including exact i64 boundaries encoded as decimal strings, and its Git blob.
The source-only interpreter gate can be run with:

```sh
node --test tests/atto-arena-policy-source.test.mjs
```

It deliberately accepts only straight-line arithmetic Atto statements. A pass
proves the pinned policy source against those vectors, **not** native guest
compilation, heap allocation, memory-safe destruction or backend parity.
These are source-only fixtures, **not evidence of compiler guest execution**.
Integration requires verified native Atto parsing, local import resolution,
guest execution, and platform/AOT tests before claiming implemented library
availability. An accepted policy decision never authorizes memory or I/O.

Do not turn these `.atto` implementations into Java/C#/C++/JS runtime bodies.
Backends may expose minimal, sealed low-level primitives, and future language
libraries should use those primitives from native Atto code.
