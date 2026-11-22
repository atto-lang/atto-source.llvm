# TenantVM generation-fenced object slots (LLVM host stage 1)

**Status:** C++17 trusted-host-only ownership primitive; NOT source-language
lowering, untrusted native machine-code isolation, or a production sandbox.
Stacked on the callback-cookie lifetime hardening in LLVM PR #1890.

## Rationale

The older `OresVM::allocate`/allocation offsets are monotonic bump regions
without individual object retirement. Tenant-level generations fence VM
recreation but are not sufficient when an object dies and its physical
memory region is recycled **within the same TenantVM**. Reusing an offset
must not resurrect a stale object capability.

## Contract

- `OresVM` still owns a distinct `IsolateVM` (canonical `TenantVM`) per
  tenant ID + generation. This feature does not introduce another VM layer.
- `allocate_object` returns a host-only, non-forgeable-by-public-constructor
  `ObjectHandle` containing opaque owner provenance, tenant generation,
  descriptor slot and **object** generation. The guest never sees a raw base
  or a native object address.
- Object allocation rejects zero length, unsupported/non-power-of-two
  alignment, alignment exceeding the tenant heap limit, heap byte exhaustion
  and a configured per-tenant descriptor-table cap (`max_object_slots`,
  default 1024, maximum 65536).
- The heap is a logical quota, not a native RSS/cgroup/physical-memory cap.
  Descriptor table overhead, allocator metadata, thread stacks, and kernel
  resources remain unmetered.
- Every byte read/write checks the live TenantVM/host identity, exact
  descriptor slot and generation, object liveness, and **object** byte size
  under the same `OresVM` mutex used for retirement/reuse.
- Retirement zeros the entire backing descriptor capacity while holding
  that lock, marks the object dead and increases its generation. An exhausted
  generation never wraps or becomes reusable; stale handles cannot alias
  a later allocation with identical offset or size.
- A recycled descriptor may service a smaller compatible object when its
  existing offset obeys the requested alignment; the old capacity is
  preserved, but the new object cannot read the unused tail.
- Shutdown refuses to complete until all live objects, actor tickets,
  guest-turn pins and native resource pins have drained. Destruction still
  wipes tenant heap bytes on its trusted lifecycle path.
- Legacy `allocate`/allocation byte APIs are intentionally left intact.
  They are separate trusted-host-only allocator surfaces until compiler
  migration; they MUST NOT be represented as two forms of untrusted guest
  authorization.

## Explicit missing security properties

**This does not isolate arbitrary native LLVM AOT code.** The trusted host
can enforce these APIs; a malicious native callback or FFI can use arbitrary
process pointers and bypass them. `ObjectHandle` is a *host integrity and
lifetime token*, not a cryptographically authenticated actor credential.
Object handles are not yet tied to a verified physical actor turn and cannot
be serialized or transferred to another tenant. Guest memory accesses have
not been rewritten to mandatory bounds checked calls, and the allocation
is still backed by a `std::vector` rather than guarded pages.

`TrustedInProcess` remains trusted-only; `SandboxedProcess` must still
fail closed. Hardware MPK is **not** installed, and even if present, a plain
pkey heap would not sandbox attacker-controlled native instructions or FFI.
No `wasm32` backend parity, source-compiled actor test or hard real-time
latency claim is made.

## Focused local verification

```sh
for CXX in g++ clang++; do
  "$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror \
    -UNDEBUG -O2 -pthread -Isrc/main/cpp \
    tests/native_oresvm_object_handles_test.cpp \
    -o "/tmp/tenantvm-objects-${CXX}"
  "/tmp/tenantvm-objects-${CXX}"
done
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  -UNDEBUG -O1 -g -pthread -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Isrc/main/cpp \
  tests/native_oresvm_object_handles_test.cpp \
  -o /tmp/tenantvm-objects-sanitized
ASAN_OPTIONS=detect_leaks=1 /tmp/tenantvm-objects-sanitized
```

The standalone test covers cross-tenant/cross-VM denial, stale-after-reuse,
out-of-bounds and alignment rejections, zeroed recycled storage, descriptor
limits, shutdown gating, same-tenant incarnation restart and concurrent
trusted-thread object access. These are **HOST_ONLY** evidence, not a
compiler-emitted LLVM guest test.

## Next implementation gates

1. Bind actor ownership to these object handles, including explicit
   physical-turn credentials and borrow/critical scopes. Deny access
   after suspension or actor retirement, not only after tenant stop.
2. Lower **source `.atto` object load/store** into a mandatory checked
   access path, never raw `GEP`/native pointer arithmetic. Verify generated
   IR under O0/O2 and execute boundary, stale, forged and cross-tenant guest
   probes, ensuring no unchecked alternate path exists.
3. Connect the generation proof to `ManagedRefLowering.hpp`'s original
   allocation ledger, with exact object bounds rather than only arena span.
4. Provide resource quotas and fuel/time checks for guest execution; bound
   map metadata, native resident bytes and physical working sets.
5. Integrate a separately authenticated Wasm/process worker for hostile
   code; evaluate MPK as defense in depth only with supervisor-controlled
   thread entry/exit, revocation, and cross-thread memory-fault tests.
6. Obtain exact-head executed native compiler, sanitizer and cross-backend
   evidence before promoting any security or parity claim.

**CI verdict:** NOT_EXECUTED unless stepful job evidence is attached to
this exact branch/head. A failed zero-step Actions run is admission trouble,
not a failing object test.
