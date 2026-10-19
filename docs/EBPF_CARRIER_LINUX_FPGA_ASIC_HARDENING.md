# Oreslang: eBPF / carrier Linux / FPGA / ASIC deployment hardening

**Phase 0 (2026-10-19): fail-closed admission, not working new backends.**
The LLVM CLI recognizes five explicit deployment profiles:

| Profile | Required independently verified output | Current state |
|---|---|---|
| `ebpf-xdp` | Actual BPF ELF and appropriate BTF accepted by XDP kernel verifier | Blocked |
| `ebpf-tc` | Actual BPF ELF and appropriate BTF accepted by TC kernel verifier | Blocked |
| `carrier-linux` | Qualified Linux native AOT ELF and pinned runtime stack | Blocked |
| `fpga-hls` | Synthesizable RTL, co-simulation, implementation and timing evidence | Blocked |
| `asic-rtl` | Formally checked RTL, DFT, STA and physical signoff evidence | Blocked |

Explicit selections **without experimental backend flags** fail before source I/O or LLVM IR emission (exit 2).
Neither `--target=host` nor `--target=wasm32` bypasses this rejection.
Unknown names, aliases and duplicates fail closed. Host emission without an
explicit deployment profile retains its existing **uncertified** meaning.

## eBPF / XDP / TC

Compiled programs are kernel callbacks, not general Oreslang processes.
XDP and TC require distinct context/packet action/helper allowlists.
Keep actors, supervision, loader, updates and privileged operations in userspace.
Compiler proof obligations:

- Finite loop bounds and bounded verifier state complexity; no unproven recursion.
- Per-attachment context, BPF helper/kfunc, kernel-version and capability checks.
- Packet bounds, `data_end`, typed stack/map/context pointers, initializedness, alignment.
- Invalidation of pointers when modifying packet helpers require it; no escape or arbitrary casts.
- No implicit heap, GC, threads, blocking, host FFI or syscalls inside BPF programs.
- Explicit map definitions, quotas, shared/per-CPU concurrency, pinning and rollback.
- BPF stack and instruction budgets, correct ELF sections, BTF/CO-RE if requested.

**Evidence gate:** real Oreslang source -> BPF ELF/BTF -> disassembly ->
kernel verifier load with recorded logs, packet differential tests, fuzzing,
privilege-separated loader and artifact-digest-bound provenance.
Compiling handwritten C for BPF does NOT establish Oreslang backend support.

## Carrier-grade Linux

- Separate data, management, supervisory and observability planes.
- Bounded mailboxes, reserved CONTROL credit, predictable overload/deadline policy.
- Affinity/NUMA placement, IRQ/NAPI steering, preallocated hot-path buffers.
- Audit blocking, priority inversion, page faults, scheduler and allocation behavior.
- Explicit PREEMPT_RT/DPDK/AF_XDP/SR-IOV negotiation, never automatic enablement.
- DMA zero-copy requires ownership, pinning, IOMMU isolation and quiescence fencing.
- Crash/restart storms, graceful drain, upgrade/rollback and partition failure tests.
- Pin CPU, NIC, kernel, toolchain and libc; capture p50/p99/p99.9/p99.99 latency,
  jitter, packet loss, throughput, max pause and time-to-recover under saturation.

**Evidence gate:** native AOT ELF from actual .ores, linker/ABI/SBOM review,
source-tree-hash provenance, performance tests and failure-injection qualification.
No hard real-time claims based solely on mean throughput or CI smoke tests.

## FPGA and ASIC

Do not conflate generic LLVM CPU IR with synthesizable hardware RTL.
Create a typed bit-accurate hardware kernel IR: static widths, arrays, loops,
ports, FIFOs, clock/reset, handshake and explicit state machines.
Reject heap allocation, general pointers, FFI, recursion, dynamic spawning,
opaque scheduler operations and floating-point IP without explicit budgets.

Hardware actor elaboration must preserve the Oreslang invariant of exactly
one persistent `state { ... }` schema per concrete actor. Critical state loans
become explicit arbitration/transaction scheduling, not host mutexes.
Clock-domain crossing (CDC), reset-domain crossing (RDC), FIFO exhaustion,
backpressure, DMA interfaces and deterministic overflow semantics are required.

**FPGA evidence:** synthesis, golden model/RTL equivalence, HLS co-sim,
LUT/FF/BRAM/DSP limits, clock, initiation interval, place-and-route and board tests.
**ASIC evidence:** formal equivalence/properties, lint, CDC/RDC, synthesis,
DFT/scan, static timing across PVT corners, power and DRC/LVS signoff.
Running an Oreslang binary on an FPGA soft-core is NOT hardware synthesis.

## Target-neutral admission envelope (planned, NOT supported syntax)

Infer and prove effect sets, memory address spaces and bounds, ownership,
initializedness, stack and recursion depth, maximum loop iterations, worst-case
cost where relevant, actor/mailbox/outstanding quotas, allocator provider,
blocking and I/O, atomic guarantees, overflow/endian/floating-point modes.
Unproven cost must fail hard timing admission. Self-asserted manifests do
not count as proofs. Bind validation reports to exact object/artifact digests,
kernel/hardware/tool versions and replayable test inputs.

## Prioritized follow-through

1. **P0 implemented on this branch:** no-silent-fallback profile selector,
   standalone ASan/UBSan negative contract and actual compiler CLI negative suite.
2. **P1:** shared Java/LLVM compiler-inferred effects, loop/stack budgets,
   freestanding core/alloc/actor/std dependency policy and negative parity fixtures.
3. **P2:** minimal XDP classifier .ores -> true BPF ELF/BTF; verifier and packets.
4. **P3:** actual carrier Linux AOT, bounded actor/NUMA/IRQ/zero-copy/upgrade gates.
5. **P4:** static stream kernel IR -> HLS/RTL; FPGA equivalence and place/route.
6. **P5:** ASIC IP/PDK and formal/STA/DFT/physical signoff.

Standalone test: `bash scripts/verify-deployment-admission.sh g++`.
Full LLVM CTest also runs the negative compiler CLI matrix.
When private GHA minutes are exhausted, mirror only an *identical Git tree*
into a funded trusted org, verify tree SHAs and run without cross-org secrets.
No CI result replaces hardware/kernel verification.

**Experimental exception (separate stacked PR):**
`--deployment-profile=ebpf-xdp --experimental-ebpf-xdp-ir` emits only the
admitted packet-nonempty XDP LLVM IR prototype, subject to an actual BPF ELF
object test and independent target-kernel verifier before deployment. It does
not enable the production/certified eBPF profile or any other profile. See
`docs/EXPERIMENTAL_EBPF_XDP_SCALAR.md`.


Official references:
- https://docs.kernel.org/bpf/standardization/instruction-set.html
- https://www.kernel.org/doc/html/latest/bpf/verifier.html
- https://docs.amd.com/r/en-US/ug1399-vitis-hls/Pointer-Limitations
- Existing internal docs: TARGET_ABI_V1.md, TARGET_ENVIRONMENTS_AND_CAPABILITIES.md,
  TYPED_ACCELERATOR_LOWERING_V0.md, CORE_ALLOC_STD_BOUNDARY.md (Java).
