# LLVM 128-step source-cooperation quantum: clock-overflow audit

**Scope:** host-only `BasicSourceCooperationQuantum<Clock>`, stacked on
`oreslang-source.llvm` draft #1570 at source commit
`1dc72735cc51a3d15882b663d6f5accc971da5c9`. No guest compiler
instrumentation, FFI preemption, process kill, or backend parity is
certified by these changes.

## P0 edge

`Clock::now() - dispatchStarted_` and the diagnostic
`Clock::now() - lastCooperate_` use signed `chrono::duration` subtraction.
With extreme signed tick values, the arithmetic can overflow in C++, which
is undefined behavior. If the overflow appears as a small positive number,
a previously expired fairness clock could look fresh. A backwards clock
sample can also silently suppress a legitimate fairness request.

The candidate now performs signed-tick ordering and computes the
mathematical elapsed difference in an unsigned representation **without**
signed subtraction. Clock regression saturates the elapsed result and
requests a safe poll. Diagnostic elapsed nanoseconds saturate at
`nanoseconds::max()`, including very large forward differences.

No new clock reads, scheduler calls, memory allocation or native FFI
are introduced into the normal loop backedge. Poll cadence remains
**every 128 committed backedges**, including when the 512-reduction
fairness target is overdue. The separate ticks-to-poll counter is
required: masking the low bits of a saturated UINT64_MAX counter would
permanently disable polling.

## Tests and release gate

`native_cooperation_quantum_test.cpp` adds deterministic fake clocks
for a signed-wrap that appears to be only 11 ns, a backwards clock
sample and an enormous valid forward interval larger than INT64_MAX.
All those must request handoff or return saturated diagnostics without
signed undefined behavior. These tests exercise the host quantum only,
not actual LLVM-generated guest code.

Suggested source-owned execution, with flags appropriate for the runner:

```sh
c++ -std=c++17 -O1 -g -fsanitize=undefined -fno-sanitize-recover=undefined \
  -Isrc/main/cpp tests/native_cooperation_quantum_test.cpp \
  -o /tmp/native-cooperation-quantum && /tmp/native-cooperation-quantum
c++ -std=c++17 -O2 -Isrc/main/cpp \
  tests/native_cooperation_quantum_test.cpp \
  -o /tmp/native-cooperation-quantum-opt && /tmp/native-cooperation-quantum-opt
```

**NOT_EXECUTED / NOT_CERTIFIED** until exact-head runs with nonempty job
steps, executable outputs and independent reviewer approval exist.
Production untrusted actor enforcement still requires independent hard
fuel accounting, source-compiled CFG/recursion safepoint coverage,
post-optimization verifier checks and isolate/process termination for
non-cooperative imported/native/FFI calls.


## Emission-time scalar CFG proof (additional hardening)

The experimental `ScalarLoopLowering.cpp` now checks actual LLVM SSA
objects and all *reachable* cyclic SCCs before emitting textual IR.
Its bounded-counted-loop proof verifies a single self-cycle; a signed
no-wrap `+1` step, an actual post-commit frame-counter store, a strict
batch-limit comparison, an admitted fuel-derived batch endpoint, and
domination by the admitted batch entry. Every function must have all of
its basic blocks reachable from its entry; otherwise it fails closed,
preventing an unreachable extra cycle from evading `scc_begin`.

**Important qualification:** This check runs at **IR emission time**,
before later LLVM `-O0/-O2/-O3` pipelines. It is **NOT** a
post-optimization metering verifier and **NOT** a general compiler
proof for actor loops/recursion. The actual compiled guest AOT suite
must still verify the optimized executable, and any future generic
post-optimization CFG pass must independently establish hard fuel for
every reachable loop/call cycle, including indirect calls and FFI
isolation. Do not authorize untrusted arbitrary guest execution from
this experimental scalar proof.

The user/source cannot supply proof flags or override the compiler's
SSA-derived verifier. The verifier gate is authored on this draft but
its source compiler/LLVM-header build and guest execution have **not**
run on a compatible exact-head runner.

## 2026-11-08 parent reconciliation

The #1570 parent advanced to `1dc72735cc51a3d15882b663d6f5accc971da5c9` with **four executable
compiled-LLVM-IR security mutation tests**. Stacked #1574 was rebased
with a force-with-lease branch update and a verified single-commit
merge tree. The Python driver now retains all four parent mutation
probes and adds the three negative source loops; all Python comments
use `#`, not C++ `//`. The resulting stack comparison was one commit
ahead and zero behind the parent, with five scoped changed files.

This reconciliation is source verification only. The newly merged test
driver and emitted-IR CFG verifier have **not** executed against an
approved LLVM compiler and native guest toolchain.
