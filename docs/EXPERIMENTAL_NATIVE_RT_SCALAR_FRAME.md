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
  The counter is committed before returning 1. The compiler computes the
  bounded dispatch limit once per step and uses a counted native loop: no
  fuel-decrement, clock read, scheduler call or FFI poll on each backedge.
  A cheap loop-bound comparison still executes for every committed tick.
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

## Amortized batch hardening

The opt-in emitter computes `batch.limit = min(counter + fuel, upperBound)`
before entering `loop.commit`. For all valid fuel values (0..512), the
loop commits no more than that many increments and returns DONE if it reaches
the source upper bound or COOPERATE otherwise. No source stack survives a
handoff. The AOT harness executes real compiled guest code at fuel boundaries
0/1/127/128/129/511/512 and rejects a generated `fuel.remaining` loop.

This does **not** implement a generic LLVM 128-safepoint compiler pass:
general actors, recursive call cycles, critical sections and arbitrary loops
remain fail-closed until ownership-safe continuation and post-optimization
checkpoint coverage are independently verified.


## Hardened frame admission and optimized regression contract

A frame must be all-zero before initialization, not merely state=0. A live,
completed or cancelled frame must have a counter in the compiler-proven
[initial, upperBound] interval before step or cancellation. Published DONE
results require an exact terminal counter equal to upperBound. Invalid frames
trap through `llvm.trap`; never skip ahead or return a forged result.

The AOT harness executes 16 independent bad-frame/invalid-operation trap
scenarios on Linux, the exact 0/1/127/128/129/511/512 dispatch-fuel
boundaries, and three-frame cancellation/fairness isolation. It must pass at
Clang `-O0`, `-O2` and `-O3`. All tests require an actual compiler-generated
LLVM module and linked native guest executable, not host-only mock receipts.
These tests remain unverified on hosted CI while Actions jobs have zero steps.

A forged state=2 with counter below upperBound now traps in **all** of
`step`, `cancel` and `result`, since the scheduler can consume a DONE
status without fetching a result. The harness must print
`SCALAR_FRAME_AOT_INVALID_FRAME_TRAPS_16_PASS` after completing every
Linux child-signal trap test; a skipped trap suite or Python `-O` test
invocation is not acceptable evidence. The complete compiler-produced
guest harness is run independently with Clang `-O0/-O2/-O3`.
