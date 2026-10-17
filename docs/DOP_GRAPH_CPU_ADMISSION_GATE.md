# Owned CPU graph scheduler admission gate (LLVM; draft)

## Scope and threat model

The native `DopGraphCpuIntegrationV1.hpp` composes a real owned F32 host
region with a distinct, CPU-only I64 typed graph and a one-shot Future. It
does not parse Oreslang `cpu {}`, launch GPU kernels or implement a typed
F32-to-I64 dataflow edge. It requires an externally budgeted scheduler;
generic inline / join-before-return dispatch is not a valid CPU worker.

An external scheduler may (a) retain and dispatch a closure more than once,
(b) invoke it on the submitting thread inline, (c) enqueue and start it
before returning, then throw, or (d) wait indefinitely for its worker.
Without an admission barrier, (c) can mutate an owned native output buffer
*before* the caller learns scheduling was rejected. A cancellation bit alone
does not prevent this race.

## Enforced ordering

1. Run structural layout, owner, privilege, row and field admission without
   acquiring write access or contacting GPU.
2. Create a shared one-shot admission latch; callback and submitter own it.
3. Atomically claim at most one callback delivery.
4. Reject inline execution when that callback runs synchronously on the
   submitting thread before schedule() returns.
5. Other workers wait for the *outcome* of schedule(): accepted only when it
   returns normally, denied when it throws. Wait is bounded at 30 seconds;
   timeout fails the Future without writing to owned buffers.
6. After accepted admission, check cancellation again, acquire physical
   input READ/output WRITE loans via execute_cpu_region and run the kernel.
   After the kernel completes and cancellation remains false, compute the
   independent CPU-only I64 graph and settle the Future once.
7. A later cancellation can suppress Future publication but cannot roll
   back host-buffer writes already performed. Failure of an output-loan
   acquisition releases the input loan before writing.

The callback strongly captures shared native buffer owners, avoiding
dangling references if the caller releases its HostBuffer wrappers.

## Scope of evidence

**Checked-in native test:** `tests/dop_graph_cpu_integration_v1_test.cpp`
includes inline-executor rejection and 128 concurrent
worker-start-before-scheduler-rejection cases, in addition to 128
duplicate-dispatch pairs and 64 cancellation races.

**Locally executed algorithm-only stress:** a transparent standalone C++20
admission-gate reproducer compiled under GCC 14 Debug, GCC 14 Release and
Clang 17 ASan+UBSan; each passed 10,240 assertions across 1,024
iterations (inline denial, reject-after-start, one-shot delivery and
pre-worker cancellation). **This is not the exact native production
header or a whole LLVM CMake/CTest build.** It cannot establish
HostBuffer RAII isolation, LLVM frontend compilation, full actor-carrier
behavior or GPU execution.

**Exact branch source/static checks:** mainline reconciliation commit
`346e3939cb69247e483381d7d39cff5b35c0ca07` preserves
Git history of both previous LLVM graph head and current native `main`.
The integration retains the latest mainline CMake targets as well as
all owned DOP and graph tests. The merged `CoreLog.hpp` matches `main`
by Git blob. GitHub reports zero commits behind main; this proves
ancestry and content, *not executed code tests*.

## Required promotion gates

- On a source-authorized, exact-head runner compile the real
  `DopGraphCpuIntegrationV1.hpp`, `DopKernelIR.hpp`,
  `DopRegionSchedule.hpp` and graph tests with GCC and Clang,
  Debug/Release, Address/Undefined and ThreadSanitizer.
- Retain nonempty real CTest job steps, CMake output, test counts,
  candidate commit/tree SHA and relevant artifacts. The source-org
  workflows have been failing before allocating test jobs.
- Run a hostile executor that starts workers before throwing,
  duplicate workers, cancellation, 30-second stall, and all existing
  data-race/loan/move/close ownership tests; no output must change
  before successful scheduler admission.
- Java #1125 has equivalent host-level admission ordering.
  .NET/Forge host graph branches currently lack comparable owned F32
  DOP buffer primitives; Wasm delegates to a pinned LLVM compiler.
  Do not equate their documented contract with executable parity.
- Keep private-source transfer prohibited until independently
  reviewed/protected source policy and numeric destination approvals,
  and security incident #22 containment. Do not reuse an old private
  carrier run to certify this exact HEAD.
- Resolve source-level no-value `void` versus `Unit` (#1098) before
  enabling positive guest syntax; no GPU-to-CPU implicit fallback.
