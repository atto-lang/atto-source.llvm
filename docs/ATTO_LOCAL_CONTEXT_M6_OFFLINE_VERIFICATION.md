# M6 source-local native verification while GitHub Actions admission is blocked

**Status:** executable *operator workflow* authored in the source repository; source compiler / guest execution is NOT attested. This is not a workaround to satisfy the hosted Actions requirement.

## Purpose

Run the pinned ActorLocal / TaskLocal / TLS C++ regression tests in an **authorized checkout of this same private repository**. No cross-organization mirror, external checkout token, deployment, public artifact, or private-source export is involved.

The verifier lives at `tests/run_native_local_context_m6_offline.sh`.

It checks: (1) clean repository, including untracked files; (2) reviewed 40-hex exact head and root Git tree; (3) tracked and working source/test Git blobs; (4) a fresh, external output directory; (5) GCC/Clang compilation and actual exit-zero execution of four native HOST test programs; and (6) optimized-mode negative mutation of the read-revocation check.

The script produces `host-local-receipt.json` with compiler version, source blob pins, executed host program names, hashes of produced native binaries and logs, and an explicit `LOCAL_HOST_ONLY_NOT_ATTESTED` classification. It *never* calls a guest compiler, publishes a hosted CI receipt, or establishes backend parity.

## Operator commands

Review the current exact PR head and Git tree from the GitHub PR and commit API **before** setting the pins. For the snapshot when this document was authored, the source-local hardening PR is [#1521](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1521). Its head may change with subsequent commits, so do not reuse a historical pin.

```bash
# In a clean, authorized checkout of LLVM PR #1521:
git status --porcelain --untracked-files=all
git rev-parse HEAD
git rev-parse 'HEAD^{tree}'

# Populate these two values from independent GitHub head/tree review:
export EXPECTED_HEAD='<reviewed 40-character PR head SHA>'
export EXPECTED_TREE='<reviewed 40-character root Git tree SHA>'

# Each run requires a DIFFERENT, initially empty directory outside checkout:
export OUTPUT_DIR="$(mktemp -d)"
CXX=g++ MODE=optimized bash tests/run_native_local_context_m6_offline.sh

# Optional separate strict builds; new output directories are mandatory:
export OUTPUT_DIR="$(mktemp -d)"
CXX=clang++ MODE=optimized bash tests/run_native_local_context_m6_offline.sh

export OUTPUT_DIR="$(mktemp -d)"
CXX=g++ MODE=asan-ubsan bash tests/run_native_local_context_m6_offline.sh

# Only if supported on the source host:
export OUTPUT_DIR="$(mktemp -d)"
CXX=g++ MODE=tsan bash tests/run_native_local_context_m6_offline.sh
```

If a compiler/sanitizer is unavailable, report that lane as **NOT_EXECUTED/UNSUPPORTED**, not passed. Never edit generated JSON to assert success. Keep logs and binaries restricted to authorized local storage.

## Exact semantics

- **Close during read copy:** no stale snapshot returned; no mutex deadlock or freed pinned value.
- **Erase/replace while read is pinned:** the old snapshot may be returned (snapshot linearizes when pinned); the next read sees the replacement. This is *not* an actor cancellation or scope revocation.
- **Copy to child:** only explicit value copy; cancelled parent cannot reintroduce already-revoked scoped values.
- **Physical TLS:** values belong to real threads; carrier migration does not make TLS logical TaskLocal.
- **Quotas and Unicode:** 32 slot / 8 KiB host budgets, strictly validated supported scalar/string values. Opaque arbitrary C++ composites remain shallow-accounted and **must not** be treated as guest-approved values.

## Outstanding release gates

1. Administrator diagnosis of [LLVM runner admission issue #159](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/159), followed by a stepful source-local no-checkout canary.
2. Stepful exact-head GCC/Clang and supported ASan/UBSan/TSan executions on this same PR tree, including the negative mutation.
3. Actual compiler-produced `.atto` TaskLocal binaries: two positives returning 42 and 14 semantic negatives, with independently read-back run/job/artifact receipts.
4. Trusted scheduler authority binding, actor/task generation fencing, tenant/isolate identity, and logical cleanup safety.
5. Independent ORESoftware review; no self-approval; keep #1521 and #1389 draft until all merge gates are satisfied.

## M7 verification hardening (November 7, 2026)

- The native negative mutation witness now demands **SIGABRT exit 134** and a source-file assertion diagnostic mentioning `!returned`; arbitrary nonzero exits, segfaults, and unrelated log messages are refused. An isolated local GCC/Clang assertion probe confirmed the expected exit/diagnostic shape. This is a probe, **not** an execution of the private LLVM runtime.
- The host workflow now also triggers on edits to this verifier, and SHA-pins the verifier's exact Git blob `f12cc91d8423d3861c39eabc05fb289143930f02` in addition to the mutation witness blob `0d256fe89fd963e520226f7207f434b9e6477be7`. The verifier's source-blobs allowlist has been updated to require that stricter witness.
- The local verifier is still **non-promoting**. Native host execution must be accompanied by fresh, authorized, independent head/tree review; full source-owned Actions and compiler-backed `.atto` guest execution remain separate mandatory release gates. Do not use a local receipt to override a zero-step hosted workflow.

## M8 ActorLocal STOP during pinned read — host-only integration boundary

- `ActorLocalSchedulerTurnV1.hpp` now combines scheduler-authenticated callback/actor generation with the scheduler's `poll_ready(actor)` STOP/retirement predicate; a carrier that still physically owns a STOPPING actor is **not** authorized to read ActorLocal. The privileged read adapter also rechecks the scheduler **after** `scope.get_copy`, to reject STOP occurring while a host copy constructor executes outside the scope lock. This is a read linearization fence, not a capability lease across subsequent guest callbacks.
- `tests/native_actorlocal_stop_during_copy_v1_test.cpp` schedules two real native actor callbacks. Actor A pauses during a pinned copy while the CONTROL thread calls STOP; its pending privileged read must return no value and further privileged set/erase attempts must deny. Actor B processes new DATA mail and preserves its independent ActorLocal state. Both actors reclaim cells only after their own scheduler terminal barriers.
- The five-lane native workflow now compiles `NativeActorScheduler.c` as C11 with GCC/Clang, links the new C++17 test, checks its exact Git blob and the adapter blob, and verifies scheduler dependencies belong to the reviewed head tree. The offline verifier also includes the fourth host program and pins its adapter/test Git blobs.
- **Unresolved:** The generic `actor_local_set_on_turn_v1` and `actor_local_erase_on_turn_v1` paths still precheck scheduler authority and then operate on a separately locked host store. They are not serialized against a concurrent STOP with one atomic commit point. `ores_native_actor_scheduler_commit_if_live_v1` explicitly forbids allocations, external locks and destructors under the scheduler mutex, so simply invoking `scope.set` inside it would violate its contract. Do **not** expose these host paths as guest ABI or claim atomic STOP-vs-write semantics before a scheduler-owned bounded mutation protocol is designed and tested.
- **Evidence:** All newly authored native host tests remain unexecuted at the source organization until runners admit real steps. Exact-head compiler-backed `.atto` proof and five-backend parity remain unproven.
