# Data-oriented CPU cache locality and CPU/GPU affinity: audit and evidence gate

Status: 2026-10-20. This is a **performance-measurement hardening slice**,
not a completed NUMA allocator, actor/task CPU-binding implementation, device
scheduler, or proven hardware speedup. No L1/L2/GPU performance numbers are
claimed without completed device-specific measurements.

## Findings from actual mainline code

* Native DopLayout.hpp checks AoS/SoA/AoSoA geometry, signed-64-bit ABI
  sizes, offsets and alignment; this protects against invalid addressing, not
  bad cache locality or physical memory residence. Scalar widths are v0
  homogeneous POD, not pointer-bearing actor graphs or heterogeneous rows.
* NativeActorScheduler.c retains a mutex-linearized CONTROL-first loop with
  a bounded same-carrier DATA reuse opportunity, and a full per-actor scan for
  ready CONTROL and DATA. The scan is O(actor_count) under the scheduler mutex;
  repeated cold metadata scans and lock contention are a plausible locality
  bottleneck but require counters before alleging measured L1/L2 thrashing.
  Dynamic ready-bitmap PR #677, stacked on #676/#672, should be integrated
  and verified rather than duplicating it on main.
* The CPU-budgeted host VM constructor is a heuristic. Its minimum separate
  domain workers can oversubscribe fractional CPU quotas. No hard global
  per-CPU execution permit, cpuset hot reload, per-actor NUMA placement, or
  guest Task/actor affinity guarantee follows from selecting a scheduler lane.
* Real GPU execution is not enabled at the Oreslang source level: DOP CPU
  kernels and OpenCL source emission are not proof of GPU launches.
  A GPU-bound actor is a host-side coordinator; device affinity must bind
  validated allocation/queue/stream capabilities, not an imaginary GPU thread.
* The existing native_actor_shard_layout_bench ran AoS, SoA and AoSoA
  in one process, so a single perf process counter blended all layouts.
  This change adds a backward-compatible single-geometry CLI for attributable
  measurements. The CLI retains production ActorShardColumns integration and
  its correctness assertion, including actor budget reclamation.

## Priorities and invariants

1. **Data layout / ownership:** expose hot/cold field separation, fixed-width
   homogeneous SoA for field sweeps and AoSoA tiles for SIMD/GPU accesses,
   while keeping AoS for small row-oriented hot paths. Do not automatically
   transpose every dataset or co-reside three layouts without costed capacity.
   Select tile width based on measured SIMD width, cache capacity, access
   density, and actual device memory coalescing, not a constant cache-line
   folklore rule. Bound transpose/scratch and per-actor resident bytes.
2. **False sharing:** independently benchmark per-carrier queue heads,
   scheduler counters and per-actor state writes on distinct cores; check
   actual coherence events/cache-line contention. Align/pad mutable
   *independent writers* when evidence warrants; avoid increasing a huge
   actor table's cache footprint blindly. Actor's single guest state is
   accessed only through its validated critical state loan; never export
   mutable fields or raw pointers to a Task or GPU kernel.
3. **CPU scheduling:** preserve one physical actor turn at a time, FIFO data
   semantics, supervisor CONTROL priority, cancellation and generation checks.
   Prefer stable carrier > same known LLC > same known NUMA > permitted remote,
   with bounded stealing/aging for fairness. Use the process's effective
   allowed CPU mask and quota; do not treat a lane affinity hint as CPU pinning.
   A true hard-CPU request must fail closed if OS pinning cannot be established.
   Never block a carrier waiting for a locality permit or GPU fence.
4. **Task / actor memory affinity:** actor state ownership is independent of
   pthread identity. Region-sharded Tasks can parallelize only with
   compiler-proven disjoint read/write loans. NUMA allocation first-touch
   belongs to the permitted region owner and must be revalidated after
   cpuset changes; migration costs must be measured. Continuations may
   resume on different carriers while preserving actor exclusive entry.
5. **GPU scheduling:** independently select authorized device/queue/stream,
   constrain tenant memory and outstanding submissions, and use explicit
   upload/download barriers and generation-fenced buffer ownership. Device
   reset, cancellation and actor shutdown must await authenticated physical
   completion before reclaiming allocations. No implicit GPU-to-CPU fallback,
   shared mutable CPU/GPU pointer, or direct GPU access to actor state.

## Host CPU measurement protocol

Build the repo's existing native_actor_shard_layout_bench target using the
project's supported CMake configuration. First check the legacy benchmark
still passes. Profile **one exact layout and one workload per process**:

    native_actor_shard_layout_bench --layout aos --rows 100000 --iterations 200
    native_actor_shard_layout_bench --layout soa --rows 100000 --iterations 200
    native_actor_shard_layout_bench --layout aosoa --rows 100000 --iterations 200

On a Linux machine granting access to CPU performance counters:

    python3 -m unittest discover -s scripts -p 'test_profile_cache_locality.py'
    python3 scripts/profile_cache_locality.py \
      --output /tmp/ores-aos.json --repeats 5 --warmups 2 -- \
      /absolute/path/native_actor_shard_layout_bench --layout aos --rows 100000 --iterations 200

Repeat with soa and aosoa, one new JSON path per run, holding workload,
compiler flags, CPU model, governors, allowed CPU set, SMT, CPU-quota and
background load constant. Compare medians and their full samples, wall-clock
ns/row from the validated benchmark, IPC, L1D MPKI, LLC MPKI and migration /
context-switch counts. Vary row counts across L1 / L2 / LLC-sized working
sets. A single invocation can run only one layout to avoid counter blending.

The JSON report contains CPU allowed-mask and kernel; L1D and LLC counters
are NOT L2 counters. Optional architecture-specific L2 request and miss events
may be supplied as --l2-requests and --l2-misses, with availability checked by
actual perf execution. Do not infer L2 misses from generic cache-misses or LLC.
Malformed, missing, permission-denied, duplicate, zero-denominator, or
unsupported perf counters abort without publishing success output. An existing
output path is refused, so a fresh run cannot overwrite old evidence.

For exact-tree evidence, optionally pass --tree <trusted 40-hex HEAD tree>.
The script compares git rev-parse HEAD^{tree} and rejects untracked, tracked or
ignored dirt. Place compiled binaries outside the checkout. This checks the
invoking checkout, not the authenticity of an external compiled binary: the
build provenance, binary hash and source-to-binary relationship must also be
attested independently.

perf stat profiles the **process**, not individual actor, Task, core, L1
cache, or physical NUMA hop. Hardware-counter interpretation and statistical
variance vary by architecture, SMT, multiplexing and kernel configuration.
For true coherence contention use platform-validated cache-to-cache tooling,
and for NUMA locality measure page placement and local/remote memory events.
GPU requires independent real-device profiling (kernel occupancy, coalesced
transactions, H2D/D2H bandwidth, queue depth, device loss) with authorized
device tools: perf CPU events cannot certify GPU affinity.

## Regression / promotion gates

- Preserve no-argument benchmark behavior and reject invalid layout, missing
  flags, negative/zero/overflowed row/iteration values before dispatch.
- Keep correct final position, actor generation, budget release, CONTROL
  priority and no concurrent actor entry in native actor suite.
- Run true production-enabled native CMake/CTest, GCC and Clang warning-clean
  builds, ASan/UBSan and targeted TSAN on the exact PR source; record actual
  run steps, binary hashes and Git tree SHA. Local parser-only tests do not
  certify the complete compiler or scheduler.
- Before merging locality-oriented scheduler changes, measure p50/p95/p99,
  throughput, L1D/L2/LLC misses, coherence transfers, migrations and CPU
  energy on representative mixed actor + Task workloads. Reject starvation
  regressions, including cold actors and reserved CONTROL wakes.
- Before advertising GPU-affinity support, require real device allocation,
  launch, authenticated fence, reset and cancellation execution, plus
  two-tenant quota isolation and cross-device transfer rejection.

## Relevant existing work to integrate, not reimplement

Java PRs #1486 and #1500: topology-local stealing and generic placement
(stacked on core-affinity #301). Java #1630: layout advisor. Java #1505:
guest Region<Motion> compiler slice (stacked on selectable residency).
LLVM #672: pthread locality; #676/#677: indexed/dynamic ready bitmaps;
#710: native layout advisor; #720: typed inverted columns. Their drafts
must pass their own exact-head and parent/stack verification before merge.
No performance improvement is inferred from a green functional test alone.
