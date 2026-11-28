# Execution stack bridge on current main

This integration preserves the native ledger/frame stack through PR #1640
(`c5624be229f0f9620625263dd361696faae6f8e0`) and main
`0ee037ca3e5dd9ac6ac1e9cc3d40f19ee6326a9c`. The logical ownership target is
**one stack per execution context**, regardless of whether a receive, timer,
task or another admitted entry point created the execution. Actor-owned memory
provides physical storage; an OS thread is not the stack identity.

## Semantic reconciliation

The scheduler merge combines retained DATA credits, reserved STOP cleanup,
resource-loan quiescence, original mailbox ordinals and WAKE outcomes with
main's intrusive ready queues and bounded idle-spin behavior. It keeps O(1)
ready selection rather than restoring the older full actor scan. A fourth
ready class represents prepaid physical cleanup; supervisor CONTROL and WAKE
remain distinct from ordinary DATA. A retained serial execution blocks later
DATA until it completes or physically abandons its frame. Cleanup publication
updates the ready queue and work epoch, so it is not lost when carriers park.

Native continuation and cleanup tests enable `ORESLANG_NATIVE_SCHED_VALIDATE`
to compare queue membership against lane state after mutations. This scan is
test-only and does not replace the production ready-queue implementation.

## Hardened frame storage

- Recycled-slot tokens are classified stale even while a newer execution owns
  the slot; only an exact duplicate of the current owner reports busy. The
  inherited test reproduced the wrong old-token classification before the fix.
- An execution cannot enter guest frames while another execution owns physical
  cleanup. The new regression verifies admission resumes after cleanup without
  poisoning the next execution.
- Stack-local frame offsets and lengths are explicitly `uint32_t`. Template
  admission bounds each stack, the live actor quota and total stack payload
  capacity to the 32-bit addressable range. Identity/generation metadata remains
  wider. This does not certify every guest allocation as ref32-lowered.

## Executed local evidence

macOS arm64, LLVM libraries 20.1.8 and Apple Clang 21.0.0:

- **14 native runtime CTests pass**, including ledger/stack authority, retained
  DATA ordering, STOP-versus-WAKE races, cleanup quarantine, ready-queue stress,
  physical-turn permits and wake-escrow failure cases.
- The same **14 tests pass under ASan/UBSan**, including ready-queue validation.
- The current-main source-compiled actor lifecycle suite passes separately.
- The retained guest bridge compiles canonical
  `native-retained-guest-steps.atto` into actual LLVM functions, screens the IR
  with the existing no-GC auditor, and links those functions with the real native
  scheduler/heap. **Six O0 and six O2 native runs pass**, plus **three O1
  ASan/UBSan runs**. A negative compiler check still requires unsupported source
  actor suspension to reject without IR.

The inherited guest driver is now Rust, with bounded subprocess execution and
kill/reap on timeout; the new fixture uses `.atto`. The native OS entry uses the
current `i32` ABI; its C declaration matches that ABI. Guest function bodies are
not replaced by host implementations. Existing unrelated legacy harnesses are
retained, rather than adding new Python infrastructure.

The retained guest test executes compiled scalar segments under a **trusted
host-owned continuation bridge**. It does not compile an entire suspending actor
method or infer its frame/borrow layout. Header-only/native runtime results are
distinct from this compiled-segment evidence and from full guest actor parity.

## Remaining integration

The ledger token provides stable logical execution identity across WAKE; native
trace execution-context records still describe execution segments and ancestry.
Their complete compiler-visible stack-context binding remains to be unified.
The bridge remains serial; concurrent-default actor execution with disjoint live
context stacks and protected shared-state loans is not implemented here.

Generated nested calls/await spills, independent borrow checking, general guest
allocation/ref32 coverage and full Spin HTTP execution remain open. Explicit
actor/supervisor shared regions require compatible readers or one exclusive
writer; private actor arenas must remain private. MessagePack data encoding must
not reconstruct frame, mutex, actor or HTTP capability authority. Tenant-VM work
follows actor-owned heap/stack proof. These local results do not certify hosted
Actions, Linux/Windows execution or a hostile-code sandbox.
