# AttoLang trusted pointer capabilities v1 — proposed guest contract

**Status: DESIGN ONLY, NOT IMPLEMENTED.** The source parser still rejects
guest `unsafe` and raw pointer intrinsics. The native host
`TrustedPointerWindow`/`NativeRegionRegistry` are trusted-computing-base
helpers and **are not** evidence of compiled AttoLang pointer execution.

## Why objects are not pointers

A value `Particle` owns its storage, whether inline, in an actor's arena,
inside a contiguous array, or in a data-oriented column. A `Pointer<Particle>`
is an *explicit access capability*, not the value. The language MUST NOT
implicitly box inline structs or turn references into independently allocated
heap objects. AoS, SoA, AoSoA, SIMD, and sealed full AOT remain available
without any public raw pointer.

## Proposed surfaces, not executable syntax

- Ordinary users: value `T`, `borrow T`, `borrow mut T`, `Box<T>`,
  `Arc<T>`, and generation-checked actor-relative handles. These do **not**
  require an unsafe/native-memory grant.
- Trusted, audited native code only: `NativePointer<T>` / `Pointer<T>` as a
  possible future alias with *region provenance*, an explicit `unsafe` effect
  and a supervisor-issued per-actor-incarnation native-memory grant.
- `Pointer<T>` is not a JS/CLR/JVM object reference and is never implicitly
  convertible from `T`, `Box<T>`, `borrow T` or integer values.
- Raw address export is an additional synchronous FFI lease. Merely obtaining
  a pointer does not authorize exporting it outside the owning actor's region.
- Arbitrary numerical address construction / integer-to-pointer casts remain
  forbidden, including in trusted guest code. MMIO requires a separate,
  device-scoped grant and target-specific ordering and access-width checks.
- Explicit `unsafe` syntax is necessary but not sufficient: module identity,
  execution domain, concrete actor-incarnation identity, lifetime, backing
  allocation, read/write mode and grant origin must all be checked.

## Nonnegotiable isolation invariants

1. **Untrusted actors have no native pointer capability.** Reject at source
   admission, type/effect checking, module linking, runtime issuance, callback
   boundary, and every guest-visible native memory entry point. A trusted
   parent sending a message does not delegate native pointer authority.
2. **No address laundering:** `Pointer<T>` and pointer-bearing closures,
   Boxes, generics, FFI wrappers and handles are non-`Send`, non-`Share`,
   non-serializable and forbidden in ActorRef messages, channels, pub/sub,
   task outcomes and durable snapshots. No generic erasure may upgrade them.
3. **Lifetime/provenance:** each region binds process/runtime, actor ID,
   incarnation epoch, region ID, allocation generation, checked bounds,
   alignment, address space, mutability and exact pin/loan status.
   Out-of-range access, one-past dereference, stale arena epoch and aliasing
   writes must reject. Storage compaction invalidates physical cursors.
4. **Full-AOT/zero-GC:** grants and access plans must be known to compiler
   and runtime before use. No runtime `eval`/JIT fallback. Actor-owned 32-bit
   arena-relative handles stay *offsets*, not host pointers. Guest loads/stores
   in untrusted domains must pass an unavoidable bounds/owner enforcement
   mechanism; having a C++ checking helper is not an isolation proof.
5. **Revocation:** actor termination, trust downgrade, supervisor restart or
   native-permission loss fences the complete actor incarnation. Existing
   cursors become invalid. Synchronous FFI leases must quiesce before backing
   memory is released; async/DMA callbacks additionally require generation-
   fenced, awaited completion pins, with retry/quarantine after failed cleanup.
6. **Host compromise:** compiler-generated trusted machine code and native FFI
   are inside the TCB. If arbitrary machine code is untrusted, isolate it in a
   verified sandbox (separate process/Wasm instance or another proven boundary)
   rather than relying on a type-level `trusted` flag.

## Backend gates

| Backend | Mandatory evidence before promotion |
| --- | --- |
| Java/GraalVM | Compiler effect/type admission, guest native-image AOT, ownership/FFM scopes, no host reference smuggling into untrusted isolates |
| LLVM | Broker-minted grants; actual `.atto` typed lowering; O0/O2 executable AOT; ASan/UBSan/TSan; proof every guest load/store is gated |
| WASM | Disallow native host address export, separate guest memories/tenants, reject pointer imports and indirect capability laundering |
| Forge | Typed IR/effect checking and either a proved AOT access gate or fail-closed unsupported diagnostics |
| .NET | Compiler effects, P/Invoke/NativeAOT admission and VM-isolate import checks, no IntPtr/Unsafe bypass via erased types |

## Minimum executable conformance matrix (not yet executed)

Positive, once implemented: trusted actor with explicit runtime grant reads
and writes an aligned in-bounds native allocation; a distinct later actor
incarnation receives a fresh grant; value arrays are contiguous without
pointer indirection. Tests MUST actually compile and run as `.atto` guest
programs and in native AOT profiles.

Negative: untrusted actor pointer creation/loads; actor-to-actor transfer;
channel/serde wrapping; erased generic transport; integer-to-pointer cast;
stale actor epoch; late FFI callback; double revoke; read-only write;
one-past dereference; offset overflow; cross-tenant aliasing; pointer capture
across `await`/`select`; hot reload / cancellation with outstanding pin;
unapproved MMIO; missing compiler lowering. Reject before IR generation where
the compiler can prove invalidity. Every backend must provide separate
exact-commit, stepful evidence. Zero-step jobs are NOT_EXECUTED.

This document intentionally does not introduce grammar or grant APIs to
production guest code: source-level promotion requires its own compiler,
runtime, test and security-review PRs on each backend.
