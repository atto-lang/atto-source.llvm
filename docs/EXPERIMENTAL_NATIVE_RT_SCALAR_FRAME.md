# Experimental scalar-loop frame ABI (v0)

This is an **opt-in LLVM AOT prototype**, not a production actor scheduler,
not automatic suspension of arbitrary native stacks, and not a 3ms guarantee.

Compile a standalone `routine` with `--experimental-rt-scalar-loop`.
The only admitted source pattern is one `val` i64 counter initialized to a
nonnegative constant, `while (counter < constant)` containing exactly
`counter = counter + 1;`, followed immediately by `return counter;`.
The bound must not exceed 1,000,000,000. No classes, actor references, calls,
loans, defers, exception effects, other statements, nested loops or WebAssembly.
Normal compiler invocations still reject `while` and `rt cooperate`.

The generated LLVM module exports four C functions:

- `void oreslang_rt_scalar_frame_init(void* frame)`: a *fresh*, zero-initialized,
  exclusively owned 24-byte frame; state transitions 0 → 1.
- `int32_t oreslang_rt_scalar_frame_step(void* frame, uint64_t fuel)`: a bounded
  dispatch slice with fuel 0..512, returning 0 DONE, 1 COOPERATE or 2 CANCELLED.
  The counter is committed before returning 1.
- `void oreslang_rt_scalar_frame_cancel(void* frame)`: owner-turn cancellation
  transitions active state 1 → 3; terminal states are idempotent.
- `int64_t oreslang_rt_scalar_frame_result(void* frame)`: requires state 2.

Host frame layout is three little-endian/native-layout 64-bit words:
`[state, counter, magic]`. The caller heap-allocates at least 24 bytes,
zero-initializes them, serially owns the frame and releases it only after
quiescence. Invalid states, bad magic, null pointers, excessive fuel, repeated
initialization and reads before DONE trap. Do not call concurrently from
different carriers: this ABI intentionally does not implement cross-thread
frame ownership transfer, mailbox ordering or cancellation acknowledgements.

The source is compiled to an actual LLVM state machine with a durable program
counter. No native stack activation is kept when `step` returns. An external
host test interleaves **two frames on one thread** and proves bounds, progress,
cancel isolation and exactly-once increments. Subsequent work must integrate
this frame contract with HungryCarrier, source actors, pinned loans, watchdog
requests and real task scheduling before expanding the supported grammar.

## Frame integrity (forward-port of #1570 onto #1569's batching)

#1569's batched step stays canonical: the endpoint
`dispatch.batch.stop = min(counter + fuel, upperBound)` is computed once per
dispatch and the only backedge test compares the SSA induction value
(`counter.before`, seeded from the validated dispatch counter) against it.
There is no per-iteration fuel decrement, clock read or scheduler call. Each
committed tick is still stored to the frame before any COOPERATE return.

On top of that the emitter enforces:

- **Zero-initialised admission.** `init` requires state, counter *and* magic
  to be zero; `state == 0` alone could resurrect a recycled incarnation.
- **Counter range in every state.** `step` and `cancel` trap unless the
  committed counter is inside the compiler-proven `[initial, upperBound]`
  interval, including for DONE and CANCELLED frames.
- **No forged completion.** DONE is accepted only with the exact terminal
  counter `upperBound`, in `step`, `cancel` and `result`. A scheduler may
  consume status 0 without ever calling `result`, so validating `result`
  alone would leave a bypass.
- **Cancellation is not completion.** A cancelled frame returns status 2
  forever and `result` traps on it.

`native_rt_scalar_frame_integrity_aot` (CTest, `tests/*.cmake`) links the
compiler-emitted module with the C oracle at Clang `-O0/-O2/-O3`, plus
UBSan/ASan where a sanitized probe builds and runs (`ORE_SANITIZER_CLANG`
may name a second clang). It requires 20 child-signal trap scenarios on
Linux/macOS, the exact dispatch-fuel boundaries 0/1/127/128/129/511/512
(from a fresh frame and from `fuel`/`fuel + 1` short of the bound), and
stable DONE/CANCELLED retirement. It then mutates the real emitted IR (fuel
cap bypass, doubled commit, stale persisted counter, CANCELLED disguised as
DONE, and removal of each new guard); every mutant must compile and be
killed by the named oracle check, not by a parser error or a crash.
