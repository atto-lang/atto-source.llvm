# ActorRegionV1 — bounded native actor-region bridge (host substrate)

**Status:** host-side implementation only; **not** a compiler-backed `.atto`
actor-object allocation, deterministic drop-lowering, a fully closed AOT
runtime link manifest, or complete process-wide zero-GC certificate.

`ActorRegionV1` wraps the existing, independently maintained `ActorArena`
implementation; it **does not replace or delete** the established i64 heap,
byte heap, actor arenas, scheduler, or their APIs. The arena reserves its backing
slab, metadata and scratch at startup, enforces one authenticated carrier turn
at a time, and zeroizes dead slot bytes. Each region has a fixed physical slot
stride and a bounded slot count: current hard limits are 4,096 slots, 1 MiB per
slot, and 64 MiB per actor region including scratch. No tracing collector or
per-allocation malloc is needed during an actor turn in the backing arena.

## Encoded 32-bit guest offset and trusted proof

`OresActorOffset32` has exactly **four bytes**. Its nonzero value is `1 +
slot_index × slot_stride`, so zero remains null and a byte location can be
represented without exposing a native host pointer. This is *not* an
independently authorized pointer. `OresActorRegionV1Ref` carries the original
arena `domain/index/generation` as a **trusted out-of-line proof**. A valid
actor turn and that exact proof are required for read, write and drop; offsets
are checked independently against the original proof. A guest must never be
allowed to manufacture a proof from an offset, or copy these host tokens via
channels, FFI, or pub-sub. The source compiler must lower ownership and member
access to this or a semantically equivalent checked ABI before guest exposure.

## Lifecycle

At initial allocation, the backing arena memory is zeroed. Every drop scrubs
the entire physical slot, advances the generation, and permits reuse without
resurrecting the previous reference. A shared read loan may survive an actor
turn and prevents writes/drops. Retirement fences new accesses and destruction
waits for active turns and loans. The supervisor must still fence timers,
continuations, actor handlers and FFI before the final destroy: this C ABI
**does not** prove those external obligations. Termination uses fatal abort on
unauthorized access, and must not be exposed directly to untrusted callers;
production guest diagnostics, cancellation and supervisor recovery must add a
controlled fault boundary.

## Validation and AOT integration still required

- Run `native_actor_region_v1_offsets_generation_drop` with real native
  `ActorArena.c` and `ActorRegionV1.c` on an exact source head; run all abort
  cases in isolated child processes. Existing arena tests must still pass.
- Add compiler lowering of real `.atto` object allocations and field access
  with explicit borrowed/owned lifetimes and cleanup on every CFG edge.
- Produce source-authenticated actor turns and prove no API leaks forgeable
  native authorization metadata into guest memory.
- Admit this native runtime to sealed AOT **only after** a transitive symbol,
  ownership, cleanup and FFI audit. Preserve the current fail-closed sealed
  linker restriction until then.
- Exercise task/actor cancellation, exception and supervisor restart,
  repeated reuse and hot-upgrade quiescence under a real scheduler.
- Prove cross-target parity in GraalVM, .NET NativeAOT, Wasm and Cranelift;
  this host-only test is not evidence of guest parity or zero GC in those VMs.

## Isolated native acceptance gate (independent follow-up)

The host-only CMake mode builds the **real** `ActorArena.c` and
`ActorRegionV1.c` instead of an API-equivalent mock. The two native sources
and two test programs are rebuilt at both `-O0` and `-O2`. It needs no LLVM
development libraries:

```sh
cmake -S . -B build-actor-region -DORESLANG_ACTOR_REGION_HOST_ONLY=ON
cmake --build build-actor-region --parallel
ctest --test-dir build-actor-region -R '^native_actor_region_v1_' --output-on-failure

# Repeat with ASan/UBSan on supported toolchains in a separate build tree.
cmake -S . -B build-actor-region-san -DORESLANG_ACTOR_REGION_HOST_ONLY=ON \
  -DORESLANG_ACTOR_REGION_SANITIZERS=ON -DCMAKE_C_COMPILER=clang
cmake --build build-actor-region-san --parallel
ctest --test-dir build-actor-region-san -R '^native_actor_region_v1_' --output-on-failure
```

The extra `actor_region_v1_fault_supervisor_test.c` exercises actual
`posix_spawn` + `exec` child-process isolation: forged generation,
foreign actor, stale slot reuse, double drop, exhaustion, read during
retirement, and incorrect actor incarnation must abort in the CHILD.
The parent verifies that it remains alive and a fresh worker can execute
afterwards. A failure MUST NOT be silently accepted merely because the
worker exited nonzero. This is POSIX-only test evidence.

**Security boundary:** Host-side `abort()` is not a catchable guest
exception. A signal handler or `setjmp` around the existing arena is
not a safe substitute for isolation. A production guest worker still
requires an OS-isolated process or equivalent stronger VM boundary,
capability-filtered IPC, resource limits, sandbox privileges, authentic
actor identity, termination/restart fencing and scheduler-approved
quiescence before memory destruction. These tests demonstrate *fault
containment*, not a finished untrusted guest sandbox.

The isolated native suites remain **HOST-ONLY** even when their real
CTest processes execute. Do not count them as compiler-backed `.atto`
allocation, guaranteed deterministic cleanup across all CFG edges,
scheduler-connected termination, verified zero-GC whole-process
execution, or transitive sealed-AOT linker proof. They must have
nonzero GitHub Actions job steps at the **exact head SHA** to count as
host CI evidence.

## Minimum compiler integration contract — still a separate gate

The upcoming opt-in compiler profile must only admit immutable scalar
class fields whose exact native sizes and alignments are resolved.
Compiler-generated code must allocate via an authenticated active actor
turn, preserve proof metadata in a host-owned sidecar (not in source
values, channels or guest pointers), use checked field reads, and issue
one actual region drop on every reachable owned-object cleanup edge.
An `llvm.lifetime.end` marker is **not** a replacement for a destructor.

The first compiled native receipt must provide the `.atto` source
hash, exact compiler commit/tree, generated LLVM IR with calls to
verified region-runtime symbols, linked ELF/Mach-O dependency listing,
final binary hash and process exit status. Only after the fault boundary,
the compiler's ownership CFG and the full transitive native linker
closure are reviewed should `sealed-class-fields-v1` be expanded or
a separate `actor-region-v1` profile be enabled. Preserve existing
profiles and default code paths.

## Compiler-facing guest sidecar prototype (host ABI only)

`ActorRegionGuestV1.{h,c}` is an intermediate bridge between the trusted
host ABI above and a future compiler profile. Its current semantics are
intentionally narrower than the actor scheduler:

- `begin` mints a host identity and opens one prefaulted region on the
  current physical thread. The source guest supplies neither actor id,
  incarnation nor turn credentials.
- `alloc` returns only the fixed-width 32-bit region offset. The complete
  `OresActorRegionV1Ref` remains in a trusted thread-local sidecar indexed by
  the physical slot.
- checked i64 field reads/writes and drops resolve the offset to that exact
  sidecar ref before calling the authenticated ActorRegionV1 API.
- `end` rejects any remaining live object, leaves the physical turn, retires
  the region and requires successful quiescent destruction. It does not sweep
  leaked objects as a substitute for generated destructors.

**ABA constraint:** a bare compact offset cannot distinguish an old reference
from a new object at the same physical slot. The guest bridge therefore marks
every slot as issued for the whole scope and aborts if the allocator attempts
to reuse it after drop. This v1 contract intentionally bounds the number of
allocations to the configured slot count, not merely the peak live-object
count. A future ABI may use a distinct opaque generation-bearing guest handle,
but it must not pretend the raw 32-bit offset is a capability.

**Execution constraint:** this bridge is thread-affine and synchronous. It
does not integrate with `NativeActorScheduler`, actor migration, task
continuations, cancellation, FFI callbacks or concurrent regions. It is not
safe to use across `await` or from multiple worker threads. The host
`abort()` behavior is still fail-stop, so this is not a production sandbox
or a recoverable guest exception ABI.

The CTest `native_actor_region_guest_v1_sidecar_contract_host_only` exercises
allocation, checked reads/writes, explicit drops, leak refusal, stale/double
drop, forged offsets and no-reuse ABA refusal. It is host-only evidence until
built and executed on a trusted exact source head. **No compiler lowering or
sealed AOT linker admission is included in this change.**

### Next compiler integration gate

A compiler may only emit this bridge after an independent audit and test
execution. The restricted source profile must keep class owners opaque, place
offsets only in compiler-generated storage, forbid user integer conversion,
unsafe/FFI calls and suspension, statically assign every object slot, and
insert `guest_v1_drop` on every reachable exit. The native `main` wrapper
must bracket the guest entry with `guest_v1_begin/end`; no asynchronous or
general actor code can enter this ABI. Sealed AOT must continue to reject
these newly discovered imports until the transitive runtime dependency closure
and process fault boundary are certified. The existing sealed scalar/class
profiles and default codegen remain unchanged.
