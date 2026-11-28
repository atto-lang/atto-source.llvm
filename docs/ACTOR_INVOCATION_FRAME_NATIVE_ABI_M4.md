# LLVM actor invocation stack ABI — lease-authenticated M4 draft

**Status: DRAFT / HOST_NATIVE_BRIDGE_NOT_EXECUTED.**

Stacked on [LLVM PR #1625](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1625), itself stacked on [LLVM ledger #1126](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1126). This is the smallest safe native integration gate before enabling source `suspends` lowering; `.atto` source admission remains **fail-closed** for native suspending actor methods.

## What the code actually does

- `ActorInvocationFrameBridge<Capacity,Depth,StackBytes,ActorBytes>` keeps the actor's existing `ActorLedgerLeaseAdapter` and `ActorInvocationFrames` together. No runtime mutation is allowed without the physical actor lease provided by the native scheduler.
- `admit` allocates a ledger event AND binds its distinct frame-stack token, rolling back queued admission if frame binding fails. `claim` admits an execution only after the ledger selects it, and binds its running frame stack.
- An actor-lease-bound AWAIT first parks its own frame store, then the ledger. Early completion causes an inline restore of that SAME frame stack and does not park a carrier. Deferred WAKE selects the original token before later queued DATA.
- `push/read/write/pop` are never exposed as unauthenticated frame-store calls. Future-producer threads and foreign carriers are rejected. Siblings share no mutable frame slots. Pinned/queued bytes and actor-aggregate budget are attributed to the original actor generation.
- `cancel` and `stop_and_request_cleanup` reserve physical retirement rather than falsely reclaiming running/suspended frames. Forced frame unwind requires an authenticated recorded cancellation/STOP request. Terminal frame poison prevents a new DATA claim before the supervisor requests STOP.
- `ActorInvocationFrameAbiV1.hpp` defines exactly sized pointer-free C-compatible token/frame layouts for a future LLVM-generated ABI, with reserved-field and bounds validation. These structs are **not capabilities**; the actor lease and live generation must still be checked inside the bridge. They are not yet linked to emitted IR or exported as runtime functions.

## Host-native test

`tests/native_actor_invocation_frame_bridge_test.cpp` uses the **actual native scheduler's** DATA, reserved WAKE, retained DATA and physical acknowledgment calls, rather than a standalone event-only model. It exercises A's nested frames surviving suspension and WAKE, rejects same-actor DATA B overtaking A, lets independent actor C progress, denies outsider calls and malformed ABI tokens, verifies return-order cleanup, and denies stale token reuse. Registered in CMake as `native_actor_invocation_frame_lease_bridge_host_only`.

Verified exact-head private runner must run:

```sh
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
  -Isrc/main/cpp -c src/main/cpp/oreslang/runtime/NativeActorScheduler.c \
  -o native-actor-scheduler.o
c++ -std=c++17 -O2 -UNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread \
  -Isrc/main/cpp tests/native_actor_invocation_frame_bridge_test.cpp \
  native-actor-scheduler.o -o actor-frame-bridge-test
./actor-frame-bridge-test
```

Clang ASan/UBSan also required, plus negative actor-generation and cancellation/STOP interleaving tests.

## Missing gates

This is NOT proof that a source `suspends actor method` compiles, is verifier-admitted, emits ABI calls, resumes a nested native callee, or provides hard-real-time or OS memory isolation. LLVM IR generation, code image pins, captured root/borrow-checker provenance, and typed frame return/unwind ABI remain **NOT_IMPLEMENTED/NOT_EXECUTED**. Do not remove existing compiler negative cases. Need real source guest fixture, independently authenticated exact-head SHA/tree/blob, nonzero job steps, GCC/Clang/ASan+UBSan and compiler acceptance/rejection receipts.

The source-org Actions runner currently does not execute jobs. **No private source has been mirrored to an alternate organization** here: [parent #1126](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1126) documents an independent security-approval gate for cross-org export. Obtain that approval before authorized exact-tree mirroring. No CI receipts are certified by this draft.
