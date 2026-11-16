# ActorRegionV1 — first sealed LLVM compiler lowering

**Status:** compiler-generated LLVM IR prototype. The CTest verifies emitted
IR and negative admission only. The native executable is intentionally NOT
linked or run yet because the transitive runtime dependency closure and guest
fault boundary are not certified. This is not full actor-scheduler integration
or a whole-process zero-tracing-GC proof.

## Source admission

Opt in with:

```sh
oreslang-llvmc --aot-profile=actor-region-v1 program.atto
```

The profile reuses `sealed-class-fields-v1` closed-world call/effect checks
and narrows further:

- native synchronous `main/0` returning i64;
- at least one and at most 4,096 fresh class owners, all lexically declared
  in `main/0`;
- class declarations have 1–16 immutable, initialized scalar fields;
- owners do not move, escape, enter class-valued parameters/returns or cross
  helper-function boundaries, suspend, use FFI or become actors/tasks/continuations;
- branch-scoped class aliases are rejected until path-sensitive branch cleanup exists;
- no methods, parameterized constructors, arrays, nested owned fields,
  mutations or unsupported expression/statement roots.

The helper-function positive/negative distinction is explicit: scalar helpers
may be called, but they cannot own a class value in v1. Branch-scoped class
aliases are also rejected until path-sensitive cleanup is proven. Unsupported
programs must fail before LLVM IR is emitted.

## Generated native ABI

The compiler emits calls to:

- `ores_actor_region_guest_v1_begin(slots, stride, scratch)` in the native
  `main` wrapper;
- `ores_actor_region_guest_v1_alloc(object_bytes)` per class local;
- `ores_actor_region_guest_v1_write_i64(offset, byte_offset, value)` for
  8-byte scalar field initialization;
- `ores_actor_region_guest_v1_read_i64(offset, byte_offset)` for every
  admitted field projection;
- `ores_actor_region_guest_v1_drop(offset)` from the compiler cleanup path;
- `ores_actor_region_guest_v1_end()` after guest `main` returns.

Each owner has a compiler-only i32 logical offset slot. The guest bridge keeps
physical `OresActorRegionV1Ref` generation/domain proofs and authenticated
turn credentials in a trusted TLS sidecar. Guest code never receives a native
pointer, turn token or generation proof. Logical guest offsets are monotonic;
the backing physical arena may reuse slots without reviving old guest offsets.

Drop is emitted as an actual runtime call, not an LLVM lifetime intrinsic.
All admitted class owners are root lexical locals, so normal and early returns
converge on the existing lexical cleanup path before the guest `main` returns.
The bridge end call then refuses to retire a region with any remaining live
object.

## Explicit limitations

- The bridge currently creates a unique synchronous root execution region; it
  does **not** obtain identity or incarnation from `NativeActorScheduler`.
  Do not call this scheduler-owned actor heap parity.
- It is thread-affine and cannot migrate, suspend, survive task continuations,
  support multiple actors within one guest entry, or cross FFI.
- Host misuse still aborts. A production recoverable guest fault boundary,
  process/VM sandbox and restart protocol are not implemented here.
- Exception unwinding, cancellation, async continuations, actor termination,
  supervisor restart, callbacks and FFI pins are not admitted.
- The existing sealed AOT linker continues to reject these runtime imports.
  `compile-aot.sh` discovers the direct C sources and refuses to invoke Clang
  or publish a binary until an independent transitive symbol/dependency and
  fault-boundary audit approves a closure certificate.
- The current CTest proves compiler IR generation and no-IR rejection, plus
  separate host bridge behavior. It does **not** claim compiler-generated
  guest execution or a linked executable.

## Verification

The CTest `actor_region_v1_compiler_generated_ir_and_negative_admission`
compiles the `Point` fixture and checks that generated IR calls the begin,
alloc, read, write, drop and end symbols. It also verifies that class owners
inside ordinary helper functions and branch-scoped class aliases are rejected
before IR output.

The host-only tests separately exercise allocator generations, sidecar
mapping, stale and double-drop rejection, explicit leak refusal, supervisor
survival after child faults, and physical slot reuse. The allocator, bridge
and supervisor suites are registered at both `-O0` and `-O2`; set
`ORESLANG_ACTOR_REGION_SANITIZERS=ON` to instrument each target with ASan and
UBSan. Exact-head nonzero-step execution and sanitizer receipts remain
required before promoting this prototype beyond draft.
