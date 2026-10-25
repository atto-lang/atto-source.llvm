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


## 2026-10-25 audit/hardening and funded-runner fallback

The source repository remains PRIVATE. The independent funded candidate
`ores-otel-test/next-loggers-runtime-conformance` (repository ID 1328290859)
has executable source-FREE runner capacity. A fresh rerun of security workflow
`40158547876` produced successful verifier job `121246470811`, including
CODEOWNERS, isolated workflows, live-boundary checks, and the PR #931 scalar
Future model. The source-bearing job was correctly **skipped**. That receipt
is a carrier-capacity/security-model result, never LLVM implementation evidence.

Exact authorization blockers were reread, not merely inferred:
- `.github/private-ci-approved-repositories.txt` blob
  `3f7e029aa9de0bcd15b8f22e035fbf83c325de96`: no approved destinations.
- `.github/private-ci-carrier-contracts.json` blob
  `4d8eb20a6931315bfb86d7188dfcc7bd54498d94`:
  no PR #987 plan or approved destination authority.
- LLVM source `main` and carrier `main` both report `protected=false`.
- `oreslang-platform-conformance` incidents #22, #77 and #127 are **OPEN**.
- Carrier #13 remains OPEN/unmerged.

**No secretless Git-object mirroring of private source is permitted yet.**
Writing private blobs through the GitHub connector would still be source
export, even without GITHUB_TOKEN, checkout or a custom secret.
Do not create carrier source branches until the reviewed v3 authorization
ledger, protected-branch and incident gates independently pass.

A review-only immutable candidate command plan is staged at
`ci/test-plans/llvm-pr987.json`, `allowed_prs=[987]`.
It is *not* installed in the carrier, and its presence is not an approval.
The plan is deliberately red: it requires the source compiler to emit two
real guest bridge calls for each repeated-observer fixture, and runs the
independent compiler-to-Clang native source suite. Merely generating valid
LLVM IR cannot pass the full L01/L02 gate.

The hardened `tests/lifecycle_repeat_observer_parity_probe.py` does not
accept one generated await, a declaration/comment mentioning the bridge,
more than two calls, invalid LLVM text, or partial IR on a rejected compile.
Eight source-free local harness tests covered these probe outcomes. They
do **not** certify actual LLVM compiler behavior. The native host CTest now
also registers distinct Linux TSan and ASan/UBSan variants; actual
native compilation/sanitizer execution remains outstanding.

Source Actions with zero recorded job steps (including PR #987 source
build/conformance runs) remain pre-execution evidence only. No compilation,
runtime behavior, or source-level `send_async` parity is certified from them.
