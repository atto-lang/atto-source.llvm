# Research-backed IsolateVM, zero-copy and data-oriented runtime adoption

**Status:** design + one executable **host-only** OresVM admission slice in the draft tenant-isolate PR. Neither compiled Oreslang tenant isolation nor zero-copy messaging is claimed. In particular, arbitrary native guest code in `TrustedInProcess` is **not** securely isolated.

## Existing architecture, preserved rather than replaced

- OresVM is the trusted, top-level manager, resource policy authority and lifecycle supervisor.
- IsolateVM is a single tenant's execution/accounting domain. Multiple actors, their sealed state and ownership arenas belong to it.
- Actors are not independent V8/Graal isolates. Oreslang keeps its reductions, bounded message admissions, cooperative continuations and actor-private heaps.
- Scheduler domains MAIN/SHARED/TRUSTED_PRIVATE/UNTRUSTED are placement/scheduling labels, **not evidence of physical sandbox separation**. The default two-domain Graal design in [TWO_DOMAIN_EXECUTION_SECURITY.md](TWO_DOMAIN_EXECUTION_SECURITY.md) and this tenant hierarchy are compatible: tenant IsolateVM objects are logical execution domains; the physical untrusted execution tier must separately establish its security boundary.
- The prototype's 32-bit offsets, `std::vector` heap and source-entry wrappers do **not** provide V8's heap sandbox, hardware-protected fault domains or a process sandbox.

## Research translated into executable contracts

| Evidence | Design lesson | Oreslang adoption / measurable gate |
|---|---|---|
| V8 Sandbox (Groß, 2024): https://v8.dev/blog/sandbox | Sandboxed pointers are checked offsets; host objects require indirection. V8 **isolate** and V8 **sandbox** are different abstractions. | Never expose raw native pointers from guest ABI. Validate base, length, owner generation and integer arithmetic at every boundary; test corrupted offsets and host-pointer substitution. |
| V8 embedding concepts: https://v8.dev/docs/embed | A V8 Isolate owns its heap; handles and handle scopes manage references. | IsolateVM owns tenant state; bound all guest-visible handles to owner/generation; require scoped/explicit loans and teardown pin accounting. |
| Wasmtime pooling/CoW: https://docs.wasmtime.dev/examples-fast-instantiation.html | Preallocated instance slots and copy-on-write initialization reduce cold-start work. | Evaluate a bounded pool for **trusted or verified-Wasm** tenants. Enforce slot reinitialization, zeroing and generation regeneration before reuse. Record start latency, RSS and stale-state tests. |
| USENIX Security 2025, Yu et al.: https://www.usenix.org/conference/usenixsecurity25/presentation/yu-zhaofeng | Wasm memory isolation alone does not bound host resource exhaustion through system interfaces. | Enforce three-level CPU, heap, open-resource, output, mailbox and queued-invocation caps (OresVM / IsolateVM / actor); fuzz each escape route. |
| Wasabi, NSDI 2026: https://www.usenix.org/conference/nsdi26/presentation/baqershahi | Hierarchical placement can improve density without ignoring isolation and queuing. | Admit tenants into explicit execution tiers; apply global + tenant quotas before instantiation, not only when memory is touched. |
| Chen et al., USENIX ATC 2024: https://www.usenix.org/conference/atc24/presentation/chen-xiangdong | MPK/MTE/PAC/capabilities have practical limitations, especially revocation and shared data. | Hardware protection is defense-in-depth, not the sole boundary. Forbid promotion of an MPK-only tenant to hostile-native secure status absent proof and threat-model review. |
| Agnocast, 2025: https://arxiv.org/abs/2506.16882 | Borrowed shared-memory messages can avoid serialization, but ownership and lifetime determine correctness. | Introduce explicit immutable read loans with lease revocation, copy-on-write only on verified mutation, and copy fallback across incompatible placement domains. |
| Apache Arrow shared-memory work (2024): https://arxiv.org/abs/2404.03030 | Immutable columnar arrays permit efficient read sharing without object-by-object serialization. | Prefer typed immutable columnar snapshots and batch/vector scans; keep buffers, schemas and alignment validated. |
| TCMalloc at scale (ASPLOS 2024): https://research.google/pubs/characterizing-a-memory-allocator-at-warehouse-scale/ | Allocation patterns and reuse locality affect performance more than universal allocator rules. | Profile actor persistent-vs-turn scratch lifetimes and size classes; require allocation-count and cache-locality measurements before any allocator switch. |
| Prequal (2024): https://research.google/pubs/load-is-not-what-you-should-balance-introducing-prequal/ | In-flight work and latency matter for multitenant tails. | Evaluate actor/tenant admission by inflight requests and measured delay, not raw CPU busy-percent alone. |
| Provably-Safe Multilingual Sandboxing using WebAssembly (USENIX Security 2022): https://www.usenix.org/system/files/sec22-bosamiya.pdf | Compiler-generated code and runtime implementation both form the sandbox trusted computing base. | Differential and negative tests must run against **actual compiled guest code**; reject invalid IR/host imports, avoid treating host-only models as guest guarantees. |

## Transport matrix: when zero-copy is honest

| Transfer | Default | Zero-copy conditions |
|---|---|---|
| Same actor, single lifetime | Move/take, or scoped borrow | Owner and lifetime statically proven; no borrow survives scratch reset or suspension without pinning |
| Different actors, same trusted IsolateVM | Move a unique buffer or immutable shared loan | Proven Sendable, scoped generation/lease bookkeeping, no concurrent mutable alias, bounded recipient memory accounting |
| Different tenant IsolateVM objects in one host | Deep copy into destination by default | Future opt-in read-only shared page pool only if physical trust tier permits; both tenants charged; revoke after all loans/continuations drain |
| Different sandboxed processes | Versioned wire copy by default | Future sealed/shared-memory descriptor or handle transfer with authenticated peer, page-level permissions, trusted broker and lifetime fencing; never raw host pointer serialization |
| Network/remote actors | Typed serialization | No end-to-end same-address zero-copy claim; kernel scatter/gather is a separately measured optimization |

**No zero-copy optimization may bypass `Sendable` vs `Serializable`, actor state sealing, permissions or `rt borrow`/`rt take` semantics.** An immutable shared buffer is not a host-object capability and cannot contain unchecked pointers or embedded foreign handles.

## DOP physical-layout policy

1. Logical ECS `EntityId` is stable only through slot generation checks; physical row indices and actor references are separate.
2. AoS, SoA, AoSoA, and SoAoS are storage plans selected with measured access profiles, not distinct logical object identity.
3. Layout migration requires no outstanding physical-address loan, checked size/alignment multiplication, a fresh epoch and transactionally committed copy. Never retain a field pointer across `await`, migration or actor teardown.
4. SoA/AoSoA batches can share immutable typed columns inside the same permission domain; mutable columns need exclusive authority and should be split by read/write hotness.
5. Use a fair comparison against equivalent logical data: throughput, p50/p99 latency, bytes copied, malloc count, peak RSS, LLC misses, SIMD width, and padding overhead. Do not claim AoSoA is universally faster.

## First code slice: hierarchical quotas (draft branch)

The OresVM prototype now admits only while all **configured** budgets fit:

- `OresVmLimits.max_tenants`: bounded live tenant count.
- `OresVmLimits.max_admitted_heap_bytes`: sum of tenant-declared **logical heap limits**, charged on creation and returned on destruction, to prevent oversubscribing the admission budget.
- `OresVmLimits.max_inflight_guest_calls`: global count of pinned, queued-or-running guest calls.
- `IsolateConfig.max_inflight_guest_calls`: per-tenant count of pinned, queued-or-running calls.

Counters are fenced under the existing OresVM host mutex. A rejected admission leaves counters unchanged. RAII guest completion returns both counters after success or exception; shutdown still waits for all guest pins. The physical host allocation, thread counts, stack memory, file descriptors, GPU buffers and true CPU fuel remain **unmetered** and MUST be separately governed before exposing an external-tenant endpoint.

Native CTest: `native_oresvm_resource_admission`, covering root/tenant budget rejections, per-tenant and global pinned-call caps, independent tenant progress, teardown and same-ID recycling. It is a host test, not a hostile guest execution proof.

## Ordered implementation and promotion gates

**P0 — correctness/security:** exact-head compile + CTest + Clang/GCC ASan/UBSan; malicious invalid handles; integer-overflow; tenant destroy/recreate; concurrent queue flood; active pinned-turn shutdown; negative source entry diagnostics. The draft remains unmerged until jobs actually execute.

**P1 — truly bounded runtime:** add actor/message/output/io/continuation limits at both aggregate and tenant levels and host-native resource accounting. Add cancellation/fuel/watchdog enforcement at compiled guest safepoints, tested with adversarial uncooperative loops.

**P2 — safe zero-copy fast paths:** typed immutable arena allocations, lifetime-fenced `take` and loan handles and a copy fallback contract. Prove no cross-tenant mutable alias, stale page reuse, capability smuggling or unmetered shared memory. Only advertise zero allocation under explicit warm steady-state and benchmark allocation instrumentation.

**P3 — sandbox + pooling:** trusted native isolate pooling plus independently validated Wasm/process workers for mutually hostile tenants. Audit reset/secret erasure, start snapshots, JIT/AOT code sharing and code-cache authority by tenant. No arbitrary native host callbacks from untrusted guests.

**P4 — source and backend parity:** run the same `.ores` fixtures through LLVM, Java/GraalVM, Wasm, Forge and .NET AOT for message ownership, handles, quotas, actor state loans, DOP layouts and cancellation. Keep backend-specific optimizations behind explicit common semantics.

**No performance or safety claim may be promoted from reference-only tests or zero-step GitHub Actions failures.** Pin commit SHA, compiler versions, executed test counts, sanitizer logs, workload shapes and copy/allocation instrumentation with the result.
