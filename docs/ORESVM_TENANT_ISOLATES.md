# OresVM tenant-isolate substrate (native host prototype)

**Canonical naming (2026-11-11): TenantVM** is the OresVM-owned per-tenant execution domain; the existing `IsolateVM` C++ class is its legacy source symbol. See [TenantVM naming](TENANT_VM_NAMING_V1.md). This remains a prototype and does not yet certify hostile actor hosting.


**Architecture and terminology:** [OresVM vs IsolateVM (including V8 isolate lessons)](ORESVM_VS_ISOLATEVM.md) explains the root-host vs tenant VM vs actor responsibilities and separates V8 Isolates, V8 Contexts and OS sandboxing. IsolateVM is now a concrete noncopyable C++ tenant-state type, privately constructed and managed by OresVM. It is not yet an autonomous scheduler or OS sandbox.

**Status:** C++17 native host prototype with one experimental scalar source-to-LLVM guest entry. It is not a complete guest VM, process sandbox, production actor scheduler or verified Java/GraalVM parity.

## Shape and intended hierarchy

- **OresVM** is the root host authority for isolate creation, admission,
  resource accounting and lifecycle supervision.
- **IsolateVM** is the planned per-tenant execution boundary: tenant-local
  heap, actor registry, module globals, symbol identities, scheduler state,
  permissions, mailboxes and teardown generation.
- **Actors** live inside exactly one tenant isolate, with their own
  ownership arenas and generation-fenced state/messaging.

The OresIsolateVM.hpp header now defines the concrete IsolateVM tenant-state owner. OresVM alone constructs and mutates it; callers receive opaque isolate, actor and allocation tickets. One opt-in pure scalar .ores source function can be lowered and invoked via the isolate ABI. Full Oreslang actor/heap/scheduler parity is still missing.

## Implemented contract

- Each tenant owns a noncopyable IsolateVM object; its constructor and writable internals are private to OresVM. Compile-time regression assertions reject direct construction, copying and moving.

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

## Host guest-call ABI (follow-up integration)

The versioned C ABI lives in src/main/cpp/oreslang/runtime/OresIsolateAbi.h.
OresVM::invoke_guest(handle, entry, i64) pins exactly that isolate's
invocation before entering trusted native code. Each isolate has 16 private
signed i64 global slots. The guest accesses these through checked callbacks
rather than process-wide mutable globals. The callback ABI is synchronous
and trusted-only; call_data and context pointers must not be retained.
No asynchronous guest work, FFI pointer isolation, guest verification,
or safe unwinding from untrusted native code is provided.

Entering Quiescing rejects new invocations, but admitted calls can finish.
complete_shutdown rejects outstanding guest calls or actors. Throwing
test guest entries release their invocation pin via RAII.
load_i64/store_i64 is NOT an atomic increment: future concurrency contracts
must enforce actor serialization or provide explicit atomic operations.

New tests:
- native_oresvm_guest_call_lifecycle: private tenant slots, independent
  progress, shutdown pins, exception cleanup and stale handle rejection.
- native_oresvm_handwritten_llvm_aot_context: handwritten LLVM IR compiled
  to native code and called for two separate tenant contexts; only when
  a 64-bit pointer host and clang are available.

These tests are host-only. Handwritten LLVM IR is not Oreslang compiler
source lowering. TrustedInProcess does NOT sandbox native pointers.
Actual actor scheduler integration, compiler lowering, continuation
retirement, process sandboxing and Java/LLVM parity remain outstanding.

Run registered tests with:

    ctest --test-dir build -R 'native_oresvm_(tenant_isolate_admission|guest_call_lifecycle|handwritten_llvm_aot_context)' --output-on-failure

Full-repository CI must execute real steps before acceptance.

## Synchronous non-reentrant guest turns

Guest calls inside one isolate are serialized with a per-isolate host mutex.
Calls against different tenants do not share this mutex. Admission is
rechecked after a queued call obtains its turn: shutdown cancels queued
invocations while allowing already-running synchronous code to finish.
Nested calls on one host thread fail closed. Queued calls are pinned for
shutdown accounting; this is not FIFO, a scheduler, preemptive cancellation,
security isolation, or hard-real-time timing certification.

## First compiler-generated .ores isolate guest (experimental)

Compile one source-defined, synchronous scalar transition with
`oreslang-llvmc --isolate-entry=tenant_step tests/fixtures/oresvm-tenant-step.ores`.
The ABI-v2 `ores_guest_entry(OresIsolateContext*, int64_t, int64_t*)`
loads tenant slot zero and lowers the validated pure `.ores` expression
directly into checked LLVM instructions. The underlying standard function
is internal to the artifact and is NOT called by the guest wrapper because
its normal overflow trap would abort the shared host. The wrapper stores
the result only after every arithmetic operation succeeds; an overflow
returns `ORES_GUEST_OVERFLOW` without changing tenant state. No process-
shared mutable LLVM global is emitted by this entrypoint. This remains
a restricted, opt-in, trusted host-only stage:
only one pure public function with two i64 parameters, no actors, heap,
strings, FFI, suspension, or other side effects. The native host serializes
same-tenant invocations. ABI v2 separates status from the full signed
64-bit result; negative values are valid. This is not arbitrary-tenant
sandboxing, process isolation or a general runtime error/effect ABI.


## Guest result ABI v2 (no sentinel collisions)

OresIsolateAbi.h now defines ORES_ISOLATE_ABI_VERSION=2. The native entry
signature returns an int32 status and writes a signed int64 result through a
separate out pointer only on success. ORES_GUEST_OK=0; nonzero status codes
signal bad context, load denial, store denial, overflow or generic failure.
Negative integer values, including -1, -2 and -3, are valid results when
status is OK. OresVM::invoke_guest_result returns a status/value pair; the
legacy convenience invoke_guest throws on any nonzero status. On failed guest
execution the host deliberately discards the output field.

The compiler-generated ores_guest_entry wrapper, the handwritten LLVM AOT
fixture, and both host/source test declarations use ABI v2. **This is an
incompatible ABI bump**: old v1 artifacts must not be invoked as v2. A
production module loader must check an artifact ABI descriptor before
converting a symbol to OresGuestEntry. A mismatched function-pointer
signature is undefined behavior and cannot be fixed by checking the
in-function context version alone.

The isolate-entry compiler-only pure scalar expression subset maps signed
add/subtract/multiply/negation overflow to `ORES_GUEST_OVERFLOW` before
any guest-global write. The ordinary compiler path still emits traps,
and Native panic, stacks, FFI, borrowed pointers, unsafe extensions and
untrusted arbitrary C/C++ code remain outside this protection.
`tests/native_oresvm_source_guest_test.cpp` includes source-defined
overflow and underflow rollback assertions; the standalone host CTest
suite also includes `native_oresvm_shutdown_race_test.cpp` to check
queued-call shutdown cancellation and independent tenant progress.

**Verification gate:** The new tests are committed but have not been
verified by an executed exact-head full-repo CI job. Inspected source
LLVM jobs have failed before execution (0 recorded steps), which is not
evidence of a compiler failure or a passing test. A dedicated new
Actions workflow could not be created during this pass because the write
was blocked by security controls. Use the existing root CMake/CTest and
standalone `tests/oresvm-standalone` test entrypoints on an authorized
runner, recording the exact commit/tree SHA and nonzero executed steps.
