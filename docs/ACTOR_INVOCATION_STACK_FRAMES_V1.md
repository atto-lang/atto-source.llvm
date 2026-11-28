# LLVM native actor invocation stacks — bounded host frame store v1

**DRAFT / HOST-RUNTIME ONLY / NOT_COMPILER_GUEST_EXECUTED.** Stacked on [LLVM PR #1126](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1126). This feature refines the ledger's already serial, nonreentrant DATA/WAKE scheduling; it does **not** claim concurrent source-level message receives.

## Runtime bridge

`src/main/cpp/oreslang/runtime/ActorInvocationFrames.hpp` adds a preallocated, fixed-capacity, actor-owned logical frame store. Each ledger token identifies a separate stack slot, and each CALL frame gets a generation-fenced LIFO ticket. A slot is bound only by trusted scheduler code **after** `ActorExecutionLedger::admit`; the frame store is not an independently authenticated host API.

- `bind(token)` reserves an identity without allocating guest frame bytes. Old sequences and recycled slot generations cannot be rebound after retirement.
- `enter(token)` corresponds to a successful actor-leased ledger claim. Only one stack may be physically executing at a time under the existing serial profile.
- `push`, `read`, `write`, and `pop` use bounded preallocated per-slot byte arrays. Every live frame's bytes are included in the actor-wide live quota; suspended stacks remain charged.
- `suspend(token)` preserves frames and live charges for future scheduler WAKE and claim of that **same** token.
- `retire_after_guest_exit(token)` requires empty frames for normal return. Cancellation can retire live frames only after physical exit. A dedicated `enter_for_cleanup(token)` grants no guest-frame access and stays admissible after terminal quota poisoning.
- Byte bounds, wrong stack tickets, stale tokens, duplicate entry and aggregate quota are fail-closed. Quota exhaustion poisons the actor; a swallowed error cannot admit more guest frames. Cleanup scrubs the entire per-slot backing store before reuse.

Physical per-slot arrays are preallocated; the actor quota counts **live** bytes, not reserved `sizeof(ActorInvocationFrames)` storage. Volatile zero writes reduce the risk of compiler-elided scrubbing, but this is not OS page isolation and is not a complete FFI protection boundary.

## Test and build

```sh
c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -Isrc/main/cpp tests/native_actor_invocation_frames_test.cpp \
  -o /tmp/actor-invocation-frames-test
/tmp/actor-invocation-frames-test
```

Or build the CMake target `oreslang-actor-invocation-frames-test` and run CTest `native_actor_invocation_frame_ownership_host_only`. The authored test checks A's nested frames surviving suspension, B not overtaking default-serial A, LIFO isolation, stale ABA token denial, queued slot recycling, cancelled cleanup after a physical EXIT, actor-wide quota and sibling survival. One small test directly models an **unimplemented future opt-in concurrent receive** only to exercise aggregate quota, not to claim that the existing ledger allows two running messages.

## Explicit missing proof

This code is not yet wired to emitted `.atto` `receive()` or `suspends` IR, and a native frame store is not a hardware-isolated native machine stack. No execution on the exact PR head has yet been attested. Required next: actual GCC/Clang/ASan/UBSan CTest receipts, LLVM compiler-emitted frame ABI and compiled nested-guest suspend/resume/cancel fixtures, plus supervisor cleanup receipts. Keep draft until stepful exact-head tests and independent review; runner-admission `steps=null` is **NOT_EXECUTED**.

Related [spec #26](https://github.com/ores-truffle-oreslang/attolang-language-spec.md/pull/26), [conformance #635](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/635), [common evidence gate #255](https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/255).
