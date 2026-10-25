# Actor lifecycle L01–L06: exact-head parity gate

The shared manifest at `contracts/v1/ACTOR_LIFECYCLE_CONFORMANCE_V1.json`
has the same semantic content in Java and LLVM. **Passing a host substrate test
is not proof of guest source-language parity.** Keep the language contract
reusable even though LLVM currently emits only one lifecycle await continuation.

## Executable check added here

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target oreslang-native-lifecycle-parity-host-test
ctest --test-dir build -R '^native_actor_lifecycle_l01_l05_host$' --output-on-failure
cmake --build build --target oreslang-native-lifecycle-parity-host-asan-test
ctest --test-dir build -R '^native_actor_lifecycle_l01_l05_host_asan_ubsan$' --output-on-failure
```

These tests verify native host lifecycle futures (repeated ready/DONE,
stop-before-READY failure replay), physical finalization with retained loans,
and one-carrier-at-a-time CONTROL+USER execution. They do not use the compiler.

## Red source parity probe — NEVER label rejection a pass

```sh
python3 tests/lifecycle_repeat_observer_parity_probe.py \
  --compiler build/oreslang-llvmc --require-ir-admission
```

The three `source-actor-lifecycle-repeat-*-target.ores` programs must
ultimately emit correct LLVM IR *and* link/run via Clang with two actual
continuation resumes. IR success alone is insufficient. A compile-time
rejection counts as an open implementation gap. The current one-await source
admission restriction is not language authority. Preserve negative no-IR
checks for bogus ActorRef receivers, forged/escaped tokens, invalid effects,
stale generations and post-STOP send.

## Remaining red L06 integration requirement

The native wake escrow in `NativeActorScheduler.h` is an independent
prerequisite. A real `send_async` implementation must under the same
mailbox-credit lock atomically claim a free USER slot, transfer the exact
generation-bound RESERVED wake ticket, and take cancellation authority.
Publishing the resumed envelope or aborting transferred credit must settle
once, retain caller payload on rejection, and never execute guest code on the
acknowledging thread. Test capacity=1 with adversarial ACK/register,
ACK/STOP, producer cancel, duplicate ACK, stale-generation ABA, and OOM races.

Tests that separately exercise `PriorityActorInbox` and wake escrow cannot
make the L06 integration gate green. Do not synthesize it with an isolated
model or assert readiness from a zero-step GitHub Actions failure.

## Evidence requirement

Record actual source SHA/tree, compiler/runner identifiers, executed job steps
and exit statuses, JUnit and CTest reports, sanitizer execution, LLVM IR,
Clang-linked executable outcomes and Java/LLVM differential traces. Keep this
PR draft until L01–L06 are exercised on an authorized source-bearing runner.
No cross-organization private-source transfer is authorized here.
