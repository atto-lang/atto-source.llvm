# Native actor reliability parity v2

This branch hardens the bounded native reliability primitives to match the
shared actor-system contracts being established in Java.

## Aligned semantics

- payload-free bounded event history with stable sequence/counter evidence;
- observer clock/provider failure is counted/dropped rather than injected into
  actor correctness;
- VM/process-fatal conditions are not intentionally swallowed;
- restart intensity may charge an entire replacement group atomically;
- restart-window entries exactly at the cutoff remain counted;
- deterministic backoff attempt 1 is never below the configured base;
- deterministic spread is upward-only and cannot exceed the configured cap;
- DATA overload hysteresis never retroactively drops accepted work.

## Important non-equivalence

`OverloadGate::ControlAccept` means only **bypass the DATA shedding
classifier**. It does NOT prove that CONTROL has storage or delivery capacity.

Full actor fault-tolerance parity still requires the native runtime to provide:

1. one generation-fenced supervisor CONTROL reservation before child admission;
2. an independently reserved root fallback generation slot;
3. no-drop publish/ack/retry/transfer semantics independent of DATA mailbox
   capacity;
4. parent logical-carrier-only supervisor callback execution;
5. explicit root delivery leases where dequeue is not acknowledgement;
6. cause-free immutable terminal metadata across actor boundaries;
7. priority CONTROL with the shared bounded anti-starvation rule;
8. generation-fenced restart orchestration that physically quiesces old
   generations before replacements.

The event ring is diagnostic evidence. It may overwrite old **telemetry** while
counting overwrites; terminal supervisor CONTROL itself must never use that
overwrite policy.

## Evidence boundary

The focused C++ test exercises this header as a host-only reliability component.
It does not certify Oreslang guest source, actual native actor mailbox CONTROL
transport, or Java/LLVM differential parity.
