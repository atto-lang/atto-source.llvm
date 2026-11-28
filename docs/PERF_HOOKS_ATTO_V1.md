# LLVM compiler-owned .atto perf hooks (source candidate)

`stdlib/perf.atto` is the clock/sampling std tier. The independent
`stdlib/alloc/perf_trace_buffer.atto` is the alloc tier; its buffer is
actor-local, bounded and never performs I/O or clock reads. ID declarations
in `stdlib/perf_ids.atto` match the shared numeric registry.

**Not activated:** Native std import resolution and the clock adapter
have NOT been verified for this module. Do not treat checking in .atto
source as successful import/linking or a strict AOT proof. The standalone
clock and buffer units also require an explicit compiler-owned import/ABI
mapping before generated probe calls can link.

Next backend steps: provide a target-specific monotonic clock capability;
wire recognized `atto.std/Perf` and `atto.alloc/perf_trace_buffer` imports;
lower typed function/actor/async exit edges to fixed numeric hook calls;
test both OFF and ON with real native LLVM guest execution including
throw/cancel and full-buffer overflow; prove native AOT linked executable;
optionally use XRay-style sites on supported Linux targets without linking
a runtime JIT. No wallclock, tracing thread, payload formatting or ambient
network on the producer path.

`node --test tests/perf_source_contract.test.mjs` validates the checked-in
source structure only. It is NOT a guest compilation or execution receipt.
