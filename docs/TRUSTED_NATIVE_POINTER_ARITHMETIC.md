# Trusted native pointer arithmetic — audit contract

Status (2026-10-17): **host-native prototype only**. The new C++ header
TrustedPointerWindow.hpp is not an Oreslang guest type, does not enable source
unsafe operations, and does not implement rt ref/unref/deref. The LLVM source
parser must continue to fail closed on unlowered pointer intrinsics.

## Source-language boundary

This branch defines **no Oreslang pointer type or pointer operation**. Guest
source remains pointerless and may use only the checked ownership surface:
`rt copy`, `rt share`, `rt take`, `rt borrow`, and `rt mut borrow`
(with `borrow T` / `borrow mut T` in type positions).

`Pointer<T>`, `NativePointer<T>`, `RawPointer<T>`, `rt ref`,
`rt deref`, `rt unref`, `rt raw`, source pointer arithmetic, address
formation, integer/address casts, and guest MMIO pointer APIs are not part of
Oreslang source. TrustedPointerWindow and NativeRegionRegistry are host/runtime
TCB objects only. They may support compiler/core/runtime implementation work,
but they must never be surfaced as guest values, serialized, captured by user
code, or smuggled through generics/callbacks/messages/channels.

Trusted actor/runtime status is not by itself authority to create a native
memory window. Host admission still requires a broker/runtime-minted grant
binding runtime identity, actor identity, epoch, region bounds, address space,
and read/write permission.

## Host/runtime admission rules

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
offsets, aligned load/store (memcpy for unsigned 8/16/32/64-bit scalar RAM values),
unsigned 8/16/32/64-bit MMIO volatile load/store, and a synchronous FFI
address callback that holds the revocation lock until return. Same-thread
reentry into the window/nested FFI is rejected instead of deadlocking.
TrustedPointerWindow::revoke() returns true only when callback quiescence was
reached; a false return denotes deferred same-thread callback revocation and
MUST NOT authorize backing-memory unmap. NativeRegionRegistry::revoke()
rejects that self-revocation, retains its authority entry, and requires a
retry after the callback completes. Registry lookups and revocation never
hold the registry mutex while waiting for the FFI lock, preventing lock-order
inversion against a callback that accesses the registry. The caller must
keep the backing bytes mapped and alive through successful revocation.
This prototype uses a shared control block and mutex: zero-copy data, but
NOT zero allocation, lock-free real time or source/LLVM guest support.

The experimental NativeRegionRegistry additionally tracks active virtual address
ranges and rejects overlapping grants in the *same* registry, including
read-only overlaps. It stamps grants with process-local registry identities to
reject cross-registry replay, validates actor/epoch identity and keeps grant
issuance private to a trusted runtime authority (with a test-only friend shim).
This is NOT a finished supervisor: the trusted runtime authority is not yet
implemented, and caller-supplied Trust values are not authentication.
Physical aliasing through independent mappings and separately constructed
registries still requires a supervisor-owned allocation/mapping catalog.

Known limitations:
- The C++ constructor is callable by trusted HOST code; the explicit
  NativeRegionIdentity must be nonzero but is still a TCB proof object, not yet
  an unforgeable guest capability. There is no compiler admission or
  supervisor-only native capability minting path yet.
- Native host misuse, early free/unmap, callback pointer retention and invalid
  mapped device addresses can still be unsafe. Cross-thread revocation waits
  for a callback to finish; same-thread revocation is deferred and applied
  before the callback lock is released. The prototype intentionally
  cannot make unrestricted native C++ memory-safe.
- No integration yet with ActorOwnedGraphHeap generation/loan retirement,
  native actor permission policy, async FFI/DMA pins or Java/Graal FFM.
- Source-level `rt ref`, `rt deref`, `rt unref`, `rt raw`, and public
  pointer types are intentionally unsupported and must remain rejected. LLVM
  #526 is relevant only as host/runtime provenance machinery; it must not
  create a guest pointer surface.

CTest trusted_native_pointer_window_host_only tests arithmetic bounds,
one-past, signed extreme offsets, alignment, read-only denial, actor and epoch
fencing, revocation, FFI completion ordering, and MMIO/FFI separation.

## Verified host-only local test evidence (2026-10-17)

The test files and C++ headers were reconstructed locally from the exact
GitHub blob content, and each reconstructed file's Git blob SHA-1 was verified
against the private branch before building:

- ActorHeapIdentity.hpp: `51266a686ae35b82e42885e21a3df51d94dd0d54`
- TrustedPointerWindow.hpp: `3408e022b10e425e71b38b06f1eab10aff084bb3`
- NativeRegionRegistry.hpp: `b832a71b24782605a0b193b2624791d8271c34d2`
- trusted_pointer_window_test.cpp: `4d5d9b529e7d1a6ff87c942258c85abe86bc06ad`
- native_region_registry_test.cpp: `5177de1bd6c350a33c1b4aad3af7fa1c0d5fa04e`

For these exact file blobs, both host test executables **passed locally**
with Clang C++17 ASan+UBSan, GCC C++17 Release -O2 -DNDEBUG,
and GCC C++17 TSan (each compiled with strict warnings and -Werror).
A separate minimal host-only CMake harness configured successfully and its
CTest run passed both tests (2/2) using those exact source/header blobs.
Three compile-negative configurations were also checked with both
GCC and Clang: private issuance, forbidden bool access and forbidden float access.

This is *not* a full LLVM compiler build or its official project CMake/CTest run,
guest-source test, CI runner pass or production authorization proof.
GitHub Actions run #39058450205 created three matrix jobs, but each
failed with no recorded steps; it provides no executed test evidence.

## Required promotion evidence

Shared Java/LLVM source contracts and exact positive/negative Oreslang fixtures;
supervisor-minted region grant; real LLVM generated IR and guest execution;
Java/Graal host-memory parity without source pointer types; full CMake/CTest; ASan/UBSan/TSan, fuzzing,
ABA and lifetime/cancellation stress; platform MMIO target tests; exact-head
CI receipts. Zero-step jobs are NOT executions. Do not copy private source
between organizations without explicit independently reviewed authorization.

Related historical pointer-design branches Java #1021/#1022 must not be used as source-language precedent. Host/runtime hardening references: LLVM #415 and LLVM #416.
