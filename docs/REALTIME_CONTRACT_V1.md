# Oreslang real-time contract v1 — LLVM admission

## Guarantee boundary

Oreslang separates **scheduling intent**, **bounded execution proof**, and
**end-to-end hard real-time certification**. LLVM/zero-GC alone proves no
deadline. A compiler-emitted request or sampled p99 latency is NOT WCET proof.

The first LLVM implementation recognizes this actor attribute:

```oreslang
@realtime(mode: #soft, period_us: 1000, deadline_us: 1000,
          budget_us: 100, mailbox: 16)
define actor Controller as {
  let total: int = 0;
  pub fnc on_message(int n): int {
    total = total + n;
    return total;
  }
}
```

The fields, with explicit integer microsecond units, are mandatory:
- `mode`: one of `#soft`, `#bounded`, `#hard`.
- `period_us`: a positive activation period (at most 24 hours).
- `deadline_us`: a positive relative deadline no larger than period.
- `budget_us`: a positive requested per-activation CPU-execution budget no
  larger than deadline. **Not a measured or verified WCET.**
- `mailbox`: declared fixed-capacity upper bound, from 1 through 16 in the
  current LLVM actor substrate. This is not yet a proof of actor mailbox
  enforcement for arbitrary profiles.

Reject duplicate/unknown fields, missing fields, unsupported positions,
out-of-range values, signed integer overflow, unknown modes and malformed
tokens before IR emission. No silent annotation drop is permissible.

## Current implementation (first milestone)

- `#soft` records an advisory **request** for each annotated LLVM actor in
  named module metadata `!oreslang.realtime.soft.v1`, with operands in order
  `actor name, period_us, deadline_us, budget_us, mailbox`.
- No runtime scheduler consumes this metadata yet. It is informational and
  **does not configure scheduling, reserve CPU capacity, or enforce a mailbox
  bound**. Linking, optimization, or LTO may strip LLVM metadata.
- `#bounded` rejects with a proof-obligation diagnostic until we have
  checked call graphs, allocation, cleanup, synchronization and runtime
  operation bounds; it never downgrades to soft mode.
- `#hard` rejects until target-specific WCET analysis, schedulability
  analysis, interrupt/OS interference, binary provenance and hardware
  admission have been proven.
- Other `@` attributes and macros remain unsupported and fail closed.
- The current restricted LLVM source actor subset remains unchanged:
  this is not new suspension, borrowing, hot-reload, isolation or priority
  support, and it does not imply Java/GraalVM annotation parity.

The source spelling is intentionally narrow while the shared Java/LLVM
attribute grammar is designed. The user-facing `1ms` syntax is deferred to
the common frontend design instead of ambiguously treating a duration as an
untyped native integer.

## Required next gates

1. Publish a backend-neutral TypeSpec/JSON contract, grammar, and Java/GraalVM
   parser tests. GraalVM remains soft-real-time only.
2. Embed a versioned, provenance-bound manifest in the final output artifact;
   do not treat potentially stripped IR metadata as an admission token.
3. Add interprocedural real-time effect checking (allocation, destruction,
   locking, calls/FFI, recursion, suspension, actor ownership and I/O).
4. Implement runtime-enforced preallocated arenas, mailbox capacities,
   overflow behavior, message-size caps and bounded supervision.
5. Add schedulability/admission with a specified target OS, processor,
   interrupt model, timer source and fixed workload. Reject on missing evidence.
6. Bind target-specific machine-code WCET and response-time certificates to
   exact binaries, runtime version and hardware config. QEMU and CI simulations
   are functional evidence, not physical hard-RT timing proof.

For end-to-end deadline D, proof must bound release jitter J, blocking B,
interference I, CPU execution C and runtime/communication overhead L such that
`J + B + I + C + L <= D`, including cross-priority effects and actor turn
exclusivity.

## First-slice test obligations

`tests/realtime_contract_admission.py` checks typed host/wasm IR emission,
metadata operands, ordinary source compatibility, rejection of malformed,
unsupported and falsely certified contracts, and absence of partial IR/files
on every negative. The separate existing actor and annotation tests must also
continue passing. These source tests are **not** hardware timing validation.
