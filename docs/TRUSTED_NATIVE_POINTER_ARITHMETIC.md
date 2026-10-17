# Trusted native pointer arithmetic — audit contract

Status (2026-10-17): **host-native prototype only**. The new C++ header
TrustedPointerWindow.hpp is not an Oreslang guest type, does not enable source
unsafe operations, and does not implement rt ref/unref/deref. The LLVM source
parser must continue to fail closed on unlowered pointer intrinsics.

## Two distinct types

- Pointer<T> (safe, affine owner): rt ref moves owned reference-backed T into a
  handle; rt deref lends a tracked immutable view; rt unref consumes the owner
  and returns owned T once, invalidating its handle. There is no pointer math,
  free-standing address, implicit copy/share, or cross-actor transport.
  This is Java #1021 candidate behavior, not LLVM source feature support.
- RawPointer<T> (canonical proposed unsafe, non-owning source capability):
  explicit, privileged view into supervisor-authorized native memory, an FFI
  buffer, or mapped MMIO. Never silently convert between Pointer<T> and
  RawPointer<T>. The C++ TrustedPointerWindow in this branch is an internal
  runtime/grant substrate, not a second guest-visible pointer type.

SHARED/PRIVATE trusted actors can be eligible, but trusted status is
**not sufficient**: compiler unsafe-effect admission and a broker-minted native
grant must both exist. Grants bind runtime identity, actor identity, epoch,
region bounds, address space, and read/write permission. UNTRUSTED actors and
normal guest code must reject pointer mapping/math through any direct or
indirect path (generics, callbacks, FFI, reflection, messages or channels).

## Proposed source language design (not yet compiling)

    unsafe {
        const NativePointer<u32> gpio = rt map_mmio<u32>(grant, base, bytes);
        const NativePointer<u32> register3 = rt offset(gpio, 3);
        rt store_volatile(register3, 1);
        const u32 status = rt load_volatile(register3);
    }

The example is schematic; unsigned primitive spelling and unsafe-block syntax
remain to be settled across backends. rt offset is ELEMENT based; a separate
rt byte_offset would be BYTE based. A possible rt from_addr would require a
matching grant and must not authorize a free-standing numeric address.

Compiler/runtime admission rules:

1. Mapping requires a supervisor/platform-approved region; physical vs virtual
   mapping, device pages and memory ordering are platform responsibilities.
2. Arithmetic is checked against original provenance. A one-past cursor may
   be formed, but never read/written. Positive/negative offsets, PTRDIFF_MIN,
   PTRDIFF_MAX and multiplication overflow fail closed. Alignment, layout,
   pointer width and target ABI matter. Bitcast widening must not evade bounds.
3. Ordinary reads/writes must respect source loan exclusivity. Native FFI and
   device producers can outlive actor turns, even with actor single-flight.
4. A native pointer cannot cross await/select, actor handoff, cancellation,
   actor death, ActorRef/message/channel serialization, untrusted isolation
   or rt share without an independently proven lifetime/pinning protocol.
5. FFI raw-address export needs a synchronous scoped lease or an explicit
   async completion pin. Closing/unmapping waits for callbacks, DMA and
   completion quiescence; unsafe callbacks can violate safety if they retain
   a raw address.
6. Resource cleanup on normal return, throw, raise, panic, finally/defer and
   cancellation revokes handles exactly once. A monotonic epoch/generation
   prevents old handles resolving into reused allocations.
7. MMIO volatile is NOT atomic and supplies no portable CPU/bus barrier.
   Target-specific access widths, ordering, endian rules, barriers and QEMU or
   embedded-hardware validation remain separate gates.

## What is actually implemented

TrustedPointerWindow is a host-only C++17 region view with actor/epoch identity,
explicit region/generation/allocation provenance, byte length, read/write mode
and revocation. It supports checked typed element
offsets, aligned load/store (memcpy for ordinary trivially-copyable objects),
unsigned 8/16/32/64-bit MMIO volatile load/store, and a synchronous FFI
address callback that holds the revocation lock until return. The caller must
keep the backing bytes mapped and alive until revoke returns. This prototype
uses a shared control block and mutex: zero-copy data, but NOT zero allocation,
lock-free real time or source/LLVM guest support.

Known limitations:
- The C++ constructor is callable by trusted HOST code; the explicit
  NativeRegionIdentity must be nonzero but is still a TCB proof object, not yet
  an unforgeable guest capability. There is no compiler admission or
  supervisor-only native capability minting path yet.
- Native host misuse, early free/unmap, callback pointer retention and invalid
  mapped device addresses can still be unsafe. The prototype intentionally
  cannot make unrestricted native C++ memory-safe.
- No integration yet with ActorOwnedGraphHeap generation/loan retirement,
  native actor permission policy, async FFI/DMA pins or Java/Graal FFM.
- Actual source-level rt ref, rt deref and rt unref are still unimplemented in
  LLVM. The separate LLVM #526 draft defines the unsafe.memory/raw-provenance
  compiler/runtime contract and must be reconciled with this runtime window
  rather than exposing two competing guest pointer types.

CTest trusted_native_pointer_window_host_only tests arithmetic bounds,
one-past, signed extreme offsets, alignment, read-only denial, actor and epoch
fencing, revocation, FFI completion ordering, and MMIO/FFI separation.

## Required promotion evidence

Shared Java/LLVM source contracts and exact positive/negative Oreslang fixtures;
supervisor-minted region grant; real LLVM generated IR and guest execution;
Java Pointer ownership parity; full CMake/CTest; ASan/UBSan/TSan, fuzzing,
ABA and lifetime/cancellation stress; platform MMIO target tests; exact-head
CI receipts. Zero-step jobs are NOT executions. Do not copy private source
between organizations without explicit independently reviewed authorization.

Related: Java #1021, Java #1022, LLVM #415, LLVM #416.
