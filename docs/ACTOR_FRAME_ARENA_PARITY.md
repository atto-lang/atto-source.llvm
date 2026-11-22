# LLVM actor-owned logical frames — Java parity M1

**Evidence status:** Implementation authored; hosted native CTest/ASan/UBSan
execution and compiler-backed AttoLang guest execution must be verified
separately on this PR's exact head. This is a host/runtime implementation,
not proof that arbitrary AttoLang actors have independent OS stacks.

## Implemented

- Per-actor-incarnation ActorFrameArena<64, 65536> stores real frame bytes
  in the native carrier alongside its independent ActorHeap<64, 256>.
  Frame memory is separate from the C++/pthread call stack.
- Nonparking, single physical execution admission. Logical execution IDs
  persist across await_io and cooperate. Suspension releases carrier
  access without releasing frame bytes; only the original execution may resume.
  The arena primitive also supports physical carrier migration.
- Frame references contain arena identity, execution identity, sequence and
  stack position. Only the active actor carrier can read/write/pop frames;
  LIFO is enforced. Out-of-order pop and quota exhaustion poison the arena.
- Byte and depth quotas are currently **64 KiB** and **64 frames** per
  carrier-owned actor. All frame bytes are scrubbed on pop, cancellation and STOP.
- A poisoned arena causes the actor carrier to STOP even if a host callback
  returns normally; unrelated actor carriers continue running.
- Native CTests: frame lifecycle/migration/ABA/4096 retirement cycles and
  actual HungryCarrier actor handler async-frame retention, sibling survival
  and terminal budget handling. The exact-head CI checks four host/runtime
  CTests and runs the two new checks under ASan/UBSan.

## Important boundaries

This first integration is intentionally in the native HungryCarrier host
runtime, **not** in the production SourceActorBootstrap.c source-compiled
actor path, nor in the LLVM frontend/typed MIR/codegen. No compiler currently
guarantees that all AttoLang guest locals, reference roots, return PCs,
exception frames or suspended tasks live in this arena. The host handler
explicitly requests frame storage, so coverage is incomplete.

The arena cannot catch native C/C++ stack exhaustion, SIGSEGV, SIGBUS, JNI,
host callbacks that recurse without compiler lowering, or process OOM.
Do not advertise that an actor stack overflow *cannot* kill the OS process.
Confinement of arbitrary untrusted native code requires an independent
sandbox/process or complete verified stackless lowering and native fault
containment. In particular, the UNTRUSTED scheduling domain is not by
itself a memory-protection boundary.

A return from a host callback does not prove compiler-backed .atto frame
execution. CTest passing only proves native host behavior. A workflow with
zero executed steps is an Actions runner-admission failure, not a test pass.

## Remaining parity gates

1. Move the primitive to a generation-fenced source-actor runtime owner
   (SourceActorBootstrap, physical scheduler and continuation store) with a
   unified actor heap+stack memory budget; quiescent finalization must be
   proven before frame storage is destroyed.
2. Add compiler-owned typed frame descriptors (PC, locals, parameters,
   operand slots, handles and borrow roots), then lower all supported scalar
   fnc/method/actor invocations into an iterative interpreter or compiler
   state machine without using native recursion for guest call depth.
3. Keep suspended SourceTask continuations and actor non-reentrant mailbox
   turn leases bound to the same incarnation and execution across migration.
   Reject stale resume after STOP, restart and hot-upgrade.
4. Run canonical .atto actor A (deep non-tail recursion/overflow), sibling B
   (healthy), supervisor restart C, tail recursion, await/resume, cancellation,
   stale frame and O0/O2 native AOT traces through the **actual**
   oreslang-llvmc frontend and native actor runtime. Require low native
   stack limit, ASan/UBSan and negative no-IR checks.
5. Compare with Java's draft #2627/#2631/#2633/#2640 using separately
   executed exact-head evidence. Neither draft source nor isolated host tests
   establish Java/LLVM parity until compiler-backed guest fixtures pass.

No private-source CI mirroring should take place without separate
authorization and independent review.
