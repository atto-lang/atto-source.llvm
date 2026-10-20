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
