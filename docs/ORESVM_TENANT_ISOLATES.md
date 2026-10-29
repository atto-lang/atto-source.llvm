# OresVM tenant-isolate substrate (native host prototype)

**Status:** C++17 host-only substrate, not a guest-code VM, process sandbox,
LLVM compiler lowering, production scheduler, or verified Java/GraalVM parity.

## Shape and intended hierarchy

- **OresVM** is the root host authority for isolate creation, admission,
  resource accounting and lifecycle supervision.
- **IsolateVM** is the planned per-tenant execution boundary: tenant-local
  heap, actor registry, module globals, symbol identities, scheduler state,
  permissions, mailboxes and teardown generation.
- **Actors** live inside exactly one tenant isolate, with their own
  ownership arenas and generation-fenced state/messaging.

The current C++ header provides the first *host* OresVM object and opaque
isolate/actor/allocation tickets. It does not execute guest Oreslang source.

## Implemented contract

- Reject blank tenant IDs, invalid quotas, duplicate live tenant IDs and
  SandboxedProcess requests (no sandbox implementation exists yet).
- Issue generation-fenced isolate handles and actor-incarnation tickets.
  Cross-manager, cross-tenant, stale, duplicate and replayed tickets are denied.
- Maintain separate logically bounded 32-bit-offset bump arenas for tenants.
  Allocations are zero-initialized and aligned; all byte accesses are checked
  against both the isolate owner and the allocation length. No raw pointer
  escapes the public API.
- Apply per-isolate actor admission limits. Actor retirement accepts only
  the currently registered actor incarnation.
- Lifecycle: Created -> Running -> Quiescing -> Stopped -> destroyed.
  Shutdown closes admission immediately. Destruction requires the actor
  registry to drain. Recreating a tenant ID issues a fresh isolate generation.
- Serialize API operations under a host mutex. This is an intentionally
  simple correctness substrate, **not** a scalable scheduler.

The heap limit is a **logical allocation quota**, not a measured RSS/virtual
memory limit: std::vector capacity, allocator metadata and other native state
are outside it. 32-bit offsets do not provide hardware memory isolation.
Best-effort buffer overwriting on destroy is not secure erasure.

## Trust boundary

**TrustedInProcess is not a security boundary.** Native guest code, FFI,
arbitrary C++ pointers, native globals and asynchronous callbacks can violate
logical isolation if admitted without additional compiler/runtime controls.
Do not run hostile tenant code with this mode.

**SandboxedProcess fails closed** until a separate executable worker, OS
isolation policy, IPC protocol, process supervision and crash containment are
implemented and tested. An LLVM JITDylib is not an OS process sandbox.

## Local host verification

Without LLVM libraries, compile the new admission test directly:

    c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread \
      -Isrc/main/cpp tests/native_oresvm_isolate_test.cpp -o /tmp/oresvm-test
    /tmp/oresvm-test

With the normal repo CMake build:

    ctest --test-dir build -R native_oresvm_tenant_isolate_admission --output-on-failure

Covered: tenant state separation; invalid/misaligned/over-quota allocations;
out-of-bounds access; foreign handles; duplicate IDs; actor quota; replayed
actor tickets; shutdown/drain fencing; stale tickets after recreate; concurrent
host API operations; explicit refusal of nonexistent sandbox mode.

## Required follow-up before multi-tenant guest execution

1. Isolate-context ABI in generated LLVM calls: tenant-local module globals,
   symbols, host handles, capabilities, heaps, I/O and dispatch; prevent
   mutable process-global guest state from leaking across tenants.
2. Real actor integration: mailbox ownership, scheduler/continuation pins,
   physical quiescence, cancellation, supervisor control notices and
   dependency-drain ordering. Actor ticket accounting alone is insufficient.
3. Allocator integration: connect source-checked ownership, OwnedHeap,
   EpochArena and native memory domains; track actual committed memory, native
   allocations, stacks, callbacks, and resource handles rather than only
   logical bump bytes.
4. Versioned wire ABI for inter-isolate messaging; deny borrowed pointers,
   cross-tenant raw references, and stale cross-isolate capabilities.
5. Separate sandboxed OS worker: image/code attestation, explicit permissions,
   IPC authentication, CPU/memory/process quotas and fault recovery; test
   native crashes and adversarial payloads.
6. Differential execution suite on matched Java/GraalVM and LLVM commits.
   Require real compiled guest execution, ASan/UBSan and instrumented CI
   evidence before claiming isolate parity or production readiness.
