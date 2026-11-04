# DOP cache-profile comparability and CPU/GPU placement gates

Status: **2026-11-01; host-counter verification only**. The three-layout
comparison is not an allocator, automatic layout chooser, actor/Task pinning
API, kernel launcher, GPU profiler, or measured cache improvement.

## Existing runtime boundaries

- LLVM `DopLayout.hpp` checks homogeneous POD AoS, SoA and AoSoA geometry,
  offsets, alignment and signed-64-bit byte limits. Physical memory residence
  and favorable L1/L2 behavior do not follow from a valid byte offset.
- The dynamic actor-ready bitmap work in LLVM PR #677 was merged after the
  October 20 locality audit. Benchmark current main rather than assuming the
  prior whole-actor ready scan remains current.
- Java `DataOrientedLayout` plans only checked geometry;
  `DopRegionSchedule` uses trusted region identities and conservative
  read/write conflicts. Neither is GPU scheduling or device-memory authority.
- Native actor affinity and Java soft carrier/lane placement are not
  automatically NUMA placement, nor proof of a permitted OS CPU binding.
  Existing draft Java PRs #301 and #1496 require their own exact-head tests.
- GPU buffer/queue preflight descriptors are host metadata, not authenticated
  provider handles. A device-queue or buffer generation match alone cannot
  authorize launch, memory reuse or actor state access.

## Reproducible host comparison

Build `native_actor_shard_layout_bench` from a trusted exact Git tree outside
the clean source checkout and attest its source-to-binary provenance. Use a
Linux host with permission to read un-multiplexed CPU perf counters.

Collect **three independent processes**, changing only `--layout`.
Pass the same trusted `--tree` SHA to each invocation:

```sh
python3 scripts/profile_cache_locality.py --tree "$TREE" \
  --output /tmp/ores-aos.json --repeats 5 --warmups 2 -- \
  /absolute/path/native_actor_shard_layout_bench --layout aos --rows 100000 --iterations 200
python3 scripts/profile_cache_locality.py --tree "$TREE" \
  --output /tmp/ores-soa.json --repeats 5 --warmups 2 -- \
  /absolute/path/native_actor_shard_layout_bench --layout soa --rows 100000 --iterations 200
python3 scripts/profile_cache_locality.py --tree "$TREE" \
  --output /tmp/ores-aosoa.json --repeats 5 --warmups 2 -- \
  /absolute/path/native_actor_shard_layout_bench --layout aosoa --rows 100000 --iterations 200

python3 scripts/compare_dop_cache_profiles.py \
  --aos /tmp/ores-aos.json --soa /tmp/ores-soa.json \
  --aosoa /tmp/ores-aosoa.json
```

The profiler requires an absolute path to an executable regular file and
records its SHA-256 and byte size before and after sampling. This detects
ordinary binary swaps or modification between runs, but not a forged report
or an attacker swapping the same bytes during a single sample.

The comparator fails before publishing a comparison if the source-tree SHA,
declared host/cpuset, event set, sampling configuration, executable/arguments
or workload differ, or if the SHA-256 digest/size of the actual native
benchmark executable changes between samples or across compared layouts,
or if one report lacks samples, declares the wrong layout, has invalid counters,
supplies contradictory derived rates or invents L2 coverage. It recalculates rates and medians from samples rather than
trusting the cached summaries. It does **not** validate the authenticity of
the JSON or physical hardware: reports from distinct hosts with indistinguishable
metadata can appear equivalent. Use a trusted runner, job provenance and
binary hashes; control frequency, thermal state, SMT, hardware revision,
microcode, page placement and background load independently.

L1D and LLC counters do **not** measure L2 misses. Supply real
architecture-specific `--l2-requests` and `--l2-misses` for all three runs
when the selected host exposes them; otherwise report L2 as *unmeasured*,
not zero.

Run `python3 -m unittest discover -s scripts -p 'test_*cache*py'`.
Synthetic JSON tests validate rejection logic, **not** hardware counters.

## Runtime hardening priorities

1. **Layout:** separate hot/cold actor and ECS fields; benchmark row-wise
   AoS versus field-wise SoA versus SIMD/GPU tiled AoSoA, including tail
   tiles. Choose tiles using the actual hardware cache-line, SIMD vector
   width, field access density and memory-transfer cost. Bound
   transposition, scratch and actor arena bytes, including temporary
   simultaneous layouts and padding.
2. **CPU locality:** measure actor state cache reuse, context switches and
   migrations under mixed CONTROL/Data and Task traffic. Prefer the existing
   permitted carrier, then measured shared LLC/NUMA locality, but allow
   bounded stealing/aging. Never break one-active-turn-per-actor, mailbox
   FIFO, shutdown generation fences or supervisor CONTROL priority.
   Do not block a carrier waiting for a locality permit.
3. **Task ownership:** shard regions only after compiler/runtime-verified
   disjoint write slices. Apply NUMA first-touch using the owning permitted
   worker; treat actor state as an owned region, not a thread-local alias.
   Revalidate physical CPU permissions across cpuset/quota changes.
4. **GPU:** actors coordinate CPU-side work; real device kernels consume
   typed, generation-checked device buffers and queue capabilities.
   Require authenticated allocation, residency/transfer plan, bounded
   in-flight submissions and physical fence completion on reset,
   cancellation and shutdown. No implicit CPU fallback or raw mutable
   actor-state access from kernels.

## Promotion evidence

Record exact commit/tree and binary SHA, compiler flags, topology/cpuset,
NUMA placement, P50/P95/P99 actor-turn latency, CONTROL fairness, L1D/L2/LLC
MPKI, coherence/false-sharing events, CPU migrations, perf sample spread and
memory footprint for representative workloads. Compare cases that fit L1,
spill into L2 and exceed LLC. Keep GPU launch occupancy, device-local
bandwidth, transfers and reset behavior as independent physical-device
measurements. Reject an apparent MPKI reduction that is offset by worse
tail latency, actor starvation, memory blowup or extra H2D copies.
