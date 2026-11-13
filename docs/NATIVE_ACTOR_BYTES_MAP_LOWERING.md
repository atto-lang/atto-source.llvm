# Native LLVM `Map<Bytes, Bytes>` actor backing — first runtime slice

## What this branch actually implements

`src/main/cpp/oreslang/runtime/NativeActorBytesMap.hpp` is a **generic
native runtime substrate**, not a Redis implementation and **not** yet
source-compiled AttoLang. It stores opaque binary keys and values in inline,
bounded per-actor memory. A distinct native Map instance is created per
actor-owned storage domain.

- Full byte-for-byte key matching including embedded NUL, high-bit bytes,
  and zero-length keys and values. No UTF-8 decoding or C-string semantics.
- Short non-suspending `put`, `get`, `erase`, and `count` calls. Every read
  **copies out**; no raw pointer, reference, or state loan escapes to an
  await continuation or another actor.
- No per-operation allocation, tracing GC, dynamic growth, or carrier-parking
  mutex wait. Concurrent contention returns `busy`; the scheduler is
  responsible for future logical retry, rather than spinning the actor carrier.
- Checked fixed capacities, fail-closed null/overflow/oversized inputs,
  all-or-nothing updates, and stable monotonically increasing mutation versions.
- A trusted-runtime `ActorHeapIdentity` including runtime, actor ID and
  incarnation gates access. This is an **assertion supplied by the scheduler**,
  not cryptographic or forge-resistant guest authority.
- `try_retire` seals admission, returns `busy` on an in-flight critical
  call, and scrubs all key/value bytes once quiesced. Destruction without
  completed retirement is fail-stop.

Caveats: this is an inline C++ object whose lifetime must be managed by an
actor-owned runtime allocator/supervisor. No claim is made here that every
guest `Bytes` value is physically allocated in the actor's native arena.
`ActorHeapIdentity` must never be constructed from untrusted actor-mail
bytes, guest integer IDs or a user-visible handle. This map supplies only
volatile generic map semantics; Redis tombstones, WAL, durability and protocol
logic belong in Atto source, **not** this host primitive.

## Executed verification boundaries

Build without LLVM development packages:

```sh
cmake -S . -B build/actor-bytes-map-host \
  -DORESLANG_ACTOR_BYTES_MAP_HOST_ONLY=ON \
  -DBUILD_TESTING=ON
cmake --build build/actor-bytes-map-host
ctest --test-dir build/actor-bytes-map-host --output-on-failure
```

`tests/native_actor_bytes_map_test.cpp` covers binary values, empty vs
absent, exact lengths, quota, failed-update atomicity, stale incarnation,
foreign actor isolation, map-instance independence, zero-copy *nonexposure*,
reused slots, version advance, retirement, and real multithreaded read/write
contention. CI also runs Clang/GCC and ASan+UBSan on the exact source blobs.

When the full `oreslang-llvmc` is available, the production compiler gate
`tests/native_actor_bytes_map_source_negative.cmake` proves that the
canonical pending `tests/fixtures/native-actor-bytes-map-target.atto` is
**rejected before IR**. A negative compiler gate is NOT guest execution.

## Executed provenance (November 13, 2026)

- [C++ native Bytes-map host run 43520109877](https://github.com/akrion-sim/akrion-sim-e2e/actions/runs/43520109877): four Git blobs matched, GCC/Clang O1 and O2 12/12 native executions plus Clang ASan+UBSan 1/1. The tests execute the real generic C++ runtime substrate, **not** a compiled Bytes/Map guest.
- [Production LLVM source compiler run 43521079675](https://github.com/akrion-sim/akrion-sim-e2e/actions/runs/43521079675): 78 selected source blobs SHA-matched the current LLVM compiler baseline; built `oreslang-llvmc`; compiled the scalar-actor .atto source to IR; linked it against the native C scheduler/heap and executed it with exit 43. The new typed-actor `native-actor-bytes-map-target.atto` failed closed with a sealed-state diagnostic and no partial IR. **Typed Bytes/Map/ActorMail remains unimplemented.**
- Source-org Actions may have zero-step runner failures; these private-carrier receipts are from separately executed workflows. Neither run certifies complete actor memory safety, real Redis TCP, durable async I/O, .NET NativeAOT or Java/Graal backend parity.

## Required compiler/native guest follow-up

1. Make `Bytes` a distinct, ownership-checked type in AST, type checker and
   MIR; never treat arbitrary binary data as String or untyped i64.
2. Lower `Map<Bytes, Bytes>` state into actor-private, bounded native memory
   with typed C/LLVM ABIs, one compiler-owned map per actor incarnation,
   failure cleanup and deterministic teardown. Prove constructor exceptions,
   capacity handling and no unowned raw pointers.
3. Implement sealed `state {}`, `critical-read`/`critical-write`, and
   exclusive writer vs concurrent reader permits with compiler borrow
   checking; state loans cannot survive `await`.
4. Implement typed `receive(ActorMail<T>)`, scheduler-issued move/borrow
   capabilities, admissible mailbox payload ownership, FIFO admission and
   STOP/restart generation fencing. No integer DATA command may mint CONTROL.
5. Replace the **expected failure** `.atto` fixture with a successful native
   compiler+link+run receipt for the real source actor. Then execute
   RESP2 SET/GET/DEL and proper WAL/fence semantics as **Atto source** on
   Java/Graal, LLVM, and .NET NativeAOT; compare to Rust reference.
6. Only after three native guests are running, report correctness, throughput,
   latency, RSS, CPU, physical arena bytes, restarts and ASan/UBSan evidence.
   The >=70% reference throughput goal is **not** established by this branch.

**Status:** actor Bytes-map **host substrate only**; Java/LLVM/.NET native
typed actor source execution remains pending.
