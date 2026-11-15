# OresVM vs TenantVM: native multi-tenant runtime architecture (legacy IsolateVM)

**Canonical naming (2026-11-11): TenantVM** replaces *IsolateVM* in architectural prose. The C++ `IsolateVM`, `IsolateHandle` and `OresIsolateContext` are legacy implementation/ABI identifiers intentionally preserved. See [TenantVM naming](TENANT_VM_NAMING_V1.md). No new physical isolation or compiled guest evidence is implied.


Status: design contract for the Oreslang LLVM backend, October 2026.
This document distinguishes the target architecture from what the
[OresVM draft PR #1158](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1158)
has actually implemented. It does not assert source-compiler parity, hardened
native sandboxing, live actor scheduling or production security.

## Definitions (normative vocabulary)

- **OresVM (root runtime / host authority):** owns the tenant registry, admission
  policy, root supervision, global accounting, immutable code/artifact registry,
  platform I/O broker and host worker scheduling resources. There is one
  logical OresVM root for each host-runtime instance. It **does not** execute
  guest application logic as a tenant or own tenant-mutable module globals.
- **IsolateVM (tenant execution domain):** an OresVM-owned instance for exactly
  one tenant principal and one incarnation/generation. Owns the tenant's
  writable module state, runtime heap/arenas, actor registry, permission grants,
  local symbols, mailbox/continuation domain, resource limits and teardown.
  Multiple IsolateVM instances may execute the same *immutable* AOT artifact.
- **Tenant:** a security/accounting principal (customer or guest), not a VM
  implementation class. Recreating tenant "A" creates a **new IsolateVM
  generation**. Tenant ID alone is never a valid lifetime-safe handle.
- **Actor:** concurrency and sealed mutable-state unit *inside one IsolateVM*.
  Actors own their mailboxes, state and actor-local arenas; they must not own
  or borrow another tenant's memory. Actor isolation is not tenant sandboxing.
- **OresIsolateContext:** a versioned, stack-borrowed native guest-call ABI
  view of the current IsolateVM; **not** the root OresVM, nor the entire
  IsolateVM owner. It is invalid after the synchronous invocation returns.
- **IsolateHandle:** an opaque reference to one IsolateVM incarnation with
  stale-generation and ownership checks; not a C++ heap pointer or an actor.

    Host OS
      OresVM (root host / global supervisor)
        immutable AOT code and host services [target]
        IsolateVM (tenant A, generation 12)
          tenant A globals, capabilities, heap, scheduler domain
          Actor A1 -> sealed state + mailbox + arena
          Actor A2 -> sealed state + mailbox + arena
        IsolateVM (tenant B, generation 13)
          tenant B globals, capabilities, heap, scheduler domain
          Actor B1 -> sealed state + mailbox + arena

The hierarchy is an **ownership and supervision hierarchy**, not a promise of
one kernel thread or one process per actor. Tenants are separate execution
domains; actors are finer-grained concurrent units *within* a tenant.

## Responsibility matrix

| Responsibility | OresVM (root) | IsolateVM (tenant) | Actor |
| --- | --- | --- | --- |
| Identity | Host instance; registry | Tenant + isolate generation | Actor + incarnation within isolate |
| Creation | Authorizes/creates isolates | Admits and supervises actors | May spawn permitted child actors |
| Mutable guest module globals | Must not hold them | Owns them | Reads through checked state APIs |
| Memory | Root budgets, immutable code | Tenant-owned allocators and quota | Actor-owned heaps/arenas |
| Scheduling | Owns carrier pool and fairness policy [planned] | Per-tenant budgets/queues [planned] | Cooperative execution and mailbox [planned] |
| I/O capabilities | Policy/broker and host resources [planned] | Tenant grants/handles [planned] | Delegated actor grants [planned] |
| Lifecycle | Creates, restarts, destroys tenants | Running, quiescing, draining | Spawn, work, cancel, retire |
| Failure handling | Tenant supervisor [planned] | Actor supervisor tree [planned] | Reports faults via CONTROL path [planned] |
| Inter-domain messaging | Policy/transport broker [planned] | Ownership and boundary validation [planned] | Typed, capability-checked send [planned] |
| Isolation | Chooses trust mode | State/ownership domain | State and mailbox exclusivity |

Shared code is permitted; accidental shared **writable** guest state is not.
Core symbols may follow an explicitly shared language contract, but ordinary
user symbols, module caches, mutable globals and actor IDs must not leak
between IsolateVM generations.

## What V8 isolates teach IsolateVM

V8 is an important architectural precedent, **not** a library dependency.
OresVM uses C++/LLVM-native execution without embedding V8 or requiring JS GC.

### 1. Isolate is different from context

V8's Isolate has its own managed heap/engine state. A V8 Context is a separate
JavaScript global environment **inside** an Isolate. Multiple Contexts can
therefore still share the underlying Isolate/heap and host embedder resources.
For Oreslang, **one tenant maps to one IsolateVM**, rather than merely one
module context within a shared tenant heap. Future guest-module or task scopes
inside an IsolateVM should never be marketed as tenant isolation.

References:
- https://v8.dev/docs/embed
- https://chromium.googlesource.com/v8/v8/+/HEAD/include/v8-isolate.h

### 2. Explicit entry, lifetime scopes and no cross-isolate object handles

V8 makes Isolate entry/exit, handle scopes and separate heaps explicit. Its
documentation prohibits using objects from one isolate in another. Follow
the *principle* in Oreslang:
- Pass an explicit isolate context into compiled guest entrypoints.
- Bind every mutable module global and runtime handle to the isolate.
- Version and generation-fence handles; never expose long-lived raw C++
  pointers to other tenants.
- End guest entry scopes deterministically. An async continuation requires
  a separately owned, pinned lifetime token; never retain the stack-borrowed
  OresIsolateContext after return.
- Use schema/versioned serialization or explicitly checked transfer for
  cross-isolate messages. An actor-local borrow cannot cross tenants.

**Important difference:** V8's public Isolate enter/Locker model permits at
most one entering thread for a given isolate at a time. Oreslang's intended
IsolateVM is *actor-first*: many independently sealed actors of one tenant
may be scheduled on different carriers concurrently, subject to per-actor
write exclusivity, proper scheduler coordination and memory-domain rules.
We should not impose a blanket single-executing-thread-per-tenant model merely
to mirror V8. The current host prototype uses one mutex to serialize host
admission/callback access; it is not a production actor scheduler.

Reference: https://chromium.googlesource.com/v8/v8/+/HEAD/include/v8-isolate.h

### 3. Host platform versus guest execution

V8 embedders supply a platform, task scheduling, allocators and host callbacks.
Analogously OresVM should own root schedulers, lifecycle monitoring, code
loading, permission policy and I/O brokerage. IsolateVM should own its
**tenant-specific** queues, continuations, allocations and handles. A guest
should never be able to reach OresVM administrative interfaces using its
untrusted guest ABI pointer. Keep the guest-facing ABI narrow.

### 4. Snapshots and shared immutable code, not shared mutable heap

V8 startup snapshots reduce initialization cost by reusing prepared state.
OresVM may load preverified AOT artifacts once, share immutable machine-code
pages and instantiate per-tenant fresh mutable globals/actor roots. A later
snapshot/restore feature must:
- Attest code and schema versions and reauthorize tenant permissions.
- Recreate per-instance generations, handles, actor IDs and capability grants.
- Never restore raw native pointers, host file descriptors or stale callbacks.
- Specify if state migration is portable across backends and AOT revisions.

Reference: https://v8.dev/docs/embed

### 5. Compressed pointers and isolate groups: useful but separate concerns

V8 uses 32-bit compressed references within a known pointer-compression cage
on supported 64-bit platforms. Current V8 also documents IsolateGroups, which
can group isolates into a shared pointer-compression cage / sandbox arrangement
(subject to build configuration and group memory limits). **Neither a shared
cage nor compression by itself enforces tenant security**.

For Oreslang: retain the compact 32-bit managed-reference objective, but use
an explicit owning tenant/arena identity, generation and validated bounds
before converting offsets to native addresses. Decide whether per-tenant
regions or trusted pools have better locality after measuring, without
letting a memory-layout grouping redefine security or ownership policy.
A group's total compressed-pointer capacity, actual committed memory and
per-tenant limits are *different* quantities.

References:
- https://chromium.googlesource.com/v8/v8/+/HEAD/include/v8-isolate.h
- https://chromium.googlesource.com/v8/v8/+/refs/heads/main/docs/heap/pointer-compression.md

### 6. In-process heap sandboxing is not OS-process fault containment

V8 has added a separate **V8 heap sandbox** with pointer indirection,
bounds/offset checks and protected host-pointer tables. This is a distinct
mitigation from merely creating a V8 Isolate. Chrome additionally uses
**sandboxed renderer processes** as security boundaries between sites.

Do not equate OresVM's present TrustedInProcess logical heaps with V8's
hardened sandbox. C++ guest AOT code, unchecked FFI, raw host pointers and
native memory-corruption bugs could still break into other tenants or the
host in one address space. OresVM must **reject SandboxedProcess** until a
real OS process worker, restricted capabilities, authenticated IPC and
supervised crash recovery are tested. Never silently downgrade isolation
mode when an untrusted tenant requests a sandbox.

References:
- https://v8.dev/blog/sandbox
- https://www.chromium.org/developers/design-documents/site-isolation/

### 7. Reclamation and scheduling: adopt invariants, not V8's GC

V8 has GC and handle scopes; Oreslang targets native ownership/borrows,
deterministic destruction, actor-local arenas and explicit quiescence.
V8's background LocalIsolate/LocalHeap design suggests keeping
worker-local fast paths and avoiding a single contention-heavy global
mutex, but Oreslang should preserve the language's actor-state rules.
Guest cancellation must await *physical* I/O, callback and continuation
retirement before reclaiming tenant-owned memory.

Reference:
https://chromium.googlesource.com/v8/v8/+/HEAD/docs/compiler/concurrency-and-background-compilation.md

## Lifecycle: intended production contract

1. **Admit:** root OresVM authenticates tenant principal, grants capabilities
   and checks root/tenant budgets and required isolation mode.
2. **Create:** instantiate fresh per-tenant state and a new generation.
3. **Start:** admit only this isolate's actors and guest calls. Generated
   guest calls receive a typed/versioned OresIsolateContext.
4. **Quiesce:** deny new admission and new transfers; revoke capabilities
   and signal actor supervisors/cancellation.
5. **Drain:** wait for actual actor, mailbox, continuation, I/O registration,
   guest-call and callback retirement. Zero active actor tickets alone is
   insufficient.
6. **Stop/destroy:** reclaim owned regions and external resources. Reusing
   tenant ID never revalidates handles from earlier generations.
7. **Restart:** restore only validated tenant data with fresh runtime
   identities; never resurrect raw pointers or expired capabilities.

## Implemented in LLVM PR #1158 vs still planned

| Feature | Status in #1158 |
| --- | --- |
| Host OresVM registry and tenant admission | Host prototype exists |
| IsolateHandle generation and actor-ticket admission | Host prototype exists |
| Checked per-tenant logical bump-buffer allocations | Host prototype exists; not RSS quota |
| C ABI OresIsolateContext and 16 private i64 globals | Synchronous host ABI exists |
| Guest invocation pin and quiescence admission gate | Synchronous host proof exists |
| Handwritten LLVM-IR guest call against two tenants | Test fixture exists; **not** .ores lowering |
| A first-class separate IsolateVM C++ class | **Implemented as a private, noncopyable tenant state owner** in OresIsolateVM.hpp; only OresVM constructs and mutates it; this is not yet an autonomous scheduler |
| Compiled .ores guest module-global lowering | **Not implemented** |
| Actual per-isolate actor scheduler, mailbox, async/await, I/O drain | **Not implemented** |
| Production tenant heap & host/global resource quotas | **Not implemented** |
| OS process sandbox + authenticated cross-tenant IPC | **Not implemented**, process mode fails closed |
| Source-level Java/GraalVM ↔ LLVM parity | **Not verified** |

## Acceptance tests required next

- Two real byte-identical Oreslang source programs execute on separate
  IsolateVMs with private writable globals and identical observable results
  to Java/GraalVM; include concurrent execution and stale-handle rejection.
- A tenant's actor dies or stalls, but another tenant remains schedulable;
  shutdown cannot complete while any pinned continuation or OS I/O remains.
- Reuse actor IDs and tenant IDs through thousands of lifecycle generations;
  fail all stale generations, replayed tickets and cross-tenant transfers.
- Prove per-tenant and root quotas on memory including native/off-heap
  allocations, CPU, queues, file descriptors and scheduler utilization.
- Execute OS sandbox escape/fault-injection and crash-restart tests for
  untrusted guest mode; verify the host and adjacent tenants survive.
- Run exact-head CTest with exercised steps, ASan/UBSan, AOT fixtures and
  cross-backend conformance. A workflow marked failed with zero recorded
  steps is neither an execution failure nor a successful acceptance test.

**Implementation note:** The first-class IsolateVM type now owns per-tenant heap, actor registry, guest slots and guest-turn mutex; OresVM holds the isolate registry and provides the only lifecycle methods. The guest-visible OresIsolateContext remains a borrowed ABI record, not a native IsolateVM pointer.\n\n**Stable naming rule:** OresVM is the root host. IsolateVM is the logical
per-tenant VM, whether implemented initially as a private State or eventually
as a separately testable C++ type. OresIsolateContext is a *borrowed execution
ABI view*, never a synonym for either VM. V8 Isolate and V8 Context are
separate reference concepts, not aliases for our naming.


### ABI evolution and code-loading guard

The initial scalar guest-call ABI is now **v2**, with independent int32 status
and i64 output pointer. An OresVM module loader must associate the ABI version
and validated symbol signature with each compiled AOT artifact *before* any
function pointer cast or native invocation. Do not load or hot-swap a v1
entrypoint as v2; context version checks performed by a wrongly cast native
function cannot prevent calling-convention undefined behavior. The host
test harness uses known compile-time typed functions, not a dynamic loader.

## Cross-backend IsolateVM parity rollout

The lifecycle vocabulary above applies to every Oreslang execution backend.
Actual host prototypes are being developed independently in the following
**draft PRs**, each with a versioned tenant handle, explicit shutdown
and backend-specific security boundaries:

| Backend | Draft PR | Current narrow scope |
| --- | --- | --- |
| .NET Native AOT | [#243](https://github.com/ores-truffle-oreslang/oreslang-source.net/pull/243) | Native AOT tenant host with actor mailboxes and a statically linked generated-CIL scalar entry smoke; full stateful .ores actor lowering and CLR/OS isolation remain missing |
| WASM | [#172](https://github.com/ores-truffle-oreslang/oreslang-source.wasm/pull/172) | Tenant admission/quota wrapper over real disposable import-free WebAssembly workers; workers are not OS sandboxes |
| Forge | [#131](https://github.com/ores-truffle-oreslang/oreslang-source.forge/pull/131) | Independent C++20 IsolateVM plus compiled Forge x86-64 scalar entry CTest for two tenant states; no native sandbox, recoverable overflow ABI or actor guest lowering |
| Cranelift | [#16](https://github.com/ores-truffle-oreslang/oreslang-source.cranelift/pull/16) | Rust tenant registry plus host-mediated execution of one compiled Cranelift JIT scalar entry for separate tenant state; JIT remains in-process and unsandboxed |

Each repository has `docs/VM_ISOLATE_CONTRACT.md`, which repeats the
lifecycle and V8-inspired distinctions and records the backend-specific
limitations. None of these four draft PRs proves full .ores guest lowering,
native process sandboxing, per-actor runtime parity, or cross-backend
conformance. Require actual executed jobs at exact hashes before accepting
any as merged parity. The Cranelift and Forge engines remain **independent**;
one must not wrap or link to the other for implementation convenience.
