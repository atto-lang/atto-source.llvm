# LLVM actor-owned logical guest frame parity — v1

Status: DRAFT. This is a host runtime and scheduler slice, not compiler-generated actor frame execution or certified native stack isolation.

## Parity target

Java/GraalVM actor-stack drafts #2505, #2624, #2627, #2628, #2633 and #2639 distinguish an actor-cell-owned logical guest frame budget from the carrier thread's physical stack. LLVM #1350 guards limited recursive scalar calls on the carrier but does not provide a distinct native C call stack per actor. Both backend efforts are draft and should not be represented as complete parity.

This LLVM slice attaches one distinct 4096-byte, preallocated ActorGuestFrameStack to each actor registered in the experimental MultiActorCarrier. Up to 32 LIFO guest frames of 1..256 bytes may be allocated; no backing pointer or raw native stack pointer is exposed to callers. Capacity exhaustion, wrong actor or generation, stale frame serial/depth, illegal offsets, non-top pop and off-carrier accesses fail closed.

guest_frame_push/read/write/pop require the current worker to be inside that actor's exclusive callback or cleanup turn, matched by slot + globally minted incarnation generation. Frames survive a callback's cooperate return and sibling dispatch; they are scrubbed when popped. A complete outcome scrubs all remaining frames. Completion with unpopped frames is also a protocol failure and requests actor retirement, but its already-committed message completion remains counted. Actor cancellation executes its callback before stack retirement; physical scrubbing precedes the retirement acknowledgement. A retired stack instance cannot be reinitialized.

This is preallocated, bounded HOST memory: not stack-switching, not per-actor OS threads, not protected memory address spaces, not compiler-lowered lexical locals, not arbitrary pointer isolation, and not evidence of zero GC over a whole linked program.

## Executed tests versus authored gates

- A standalone header-only GCC 14.2 -O2 / -Werror smoke test was executed locally for frame ownership, LIFO/serial fencing, retirement and scrubbing. This does NOT execute integrated carrier paths or certify repo HEAD.
- tests/native_actor_guest_frame_stack_test.cpp provides a host-only scheduler regression: A holds one frame across repeated cooperative dispatches; B executes on the same physical carrier, cannot access A's stack, uses its own frame, and completes; A's cleanup accesses and releases its retained frame. It also checks generation, bounds, zero-after-pop, capacity, LIFO and off-carrier denial.
- CTest native_actor_guest_frame_stack_host_only and source-owned exact-head GCC/Clang/Clang ASan+UBSan workflow are authored. Until a real runner executes nonempty steps and emits hashes they remain NOT_EXECUTED.
- Compiler-generated .atto actor call frames, typed arguments/locals, actor-owned stack budget in source actor handlers, STOP/await/select/throw/panic cleanup, O0/O2 native AOT execution, real per-actor fiber/OS stacks and hard-RT WCET are NOT_CERTIFIED.

## Next integration gates

1. Obtain source-authorized exact-head nonzero CI execution; verify source blobs, GCC/Clang, ASan/UBSan, native scheduler cleanup and zero sanitizer findings. Do not export private source across orgs without explicit independent authorization.
2. Reconcile LLVM compiler stack-budget PR #1350 and actor-ledger #1126; drive generated .atto scalar frame slots through these checked APIs rather than storing guest frames in arbitrary host-owned structs.
3. Wire compiler-produced await/select continuations into actor-owned logical frames with a generation-checked resume and borrow/loan/FFI retirement barrier; run genuine source -> LLVM IR -> Clang native guest tests for A/B isolation and STOP.
4. Prove static/dynamic frame byte budgeting and stack-probe containment for the carrier's real C stack, separately from the logical frame budget. Evaluate stackful switching only if required; no unsafe ucontext or one-thread-per-actor shortcut.
5. Extend corresponding tests to Java, Wasm, Forge, .NET and Rust on their exact current heads. Contract-text parity and historical runs are not executable feature parity.

All changes stay inside the original private LLVM repository, target a draft PR and do not mutate Java or protected main.
