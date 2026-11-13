# Native actor-owned Bytes inbox (trusted runtime substrate only)

This is a bounded, preallocated **copying** mailbox payload store per actor
incarnation. It is not the `.atto` `Bytes` source lowering, not a general
zero-copy borrower, not a Kafka broker, and not a network or disk I/O adapter.

- Dedicated heap allocation at trusted actor admission: 1–128 physical slots,
  each 1–65536 bytes; no more than 8 MiB per inbox and a separate admitted
  byte budget. All storage is **per actor**, not one JVM/CLR arena or a shared
  host vector. An external supervisor must enforce aggregate memory quotas.
- `try_send` copies bytes into the destination actor's storage, including
  NUL and high-bit payloads; an admission receipt does **not** mean the actor
  processed or durably ACKed the message.
- `try_receive` copies into a caller-provided buffer (never returns a
  pointer to actor-owned memory). If the output is too small it returns SMALL
  and leaves the message queued; a successful dequeue zeroizes its entire
  physical slot, not just the logical payload size.
- `try_retire` rejects subsequent admission, abandons queued DATA and
  scrubs all slots. `try_destroy` additionally **requires externally proven
  physical quiescence**, including STOP/DONE, producer/receiver in-flight
  operations, cancellation and outstanding FFI references. A matching
  3-component actor ID, incarnation and monotonic nonce prevents stale reuse
  of numeric actor IDs.
- A single `atomic_flag` implements nonparking try-admission: concurrent
  operations return BUSY and must be retried by a trusted scheduler later.
  No waiting mutex, condition variable, `Future.get` or blocking I/O.
  No `malloc`/`free` occurs in send/receive. Memory copying can still
  consume CPU, so this is not a hard realtime WCET certification.
- The ABI is only for the trusted runtime. The caller-provided identity is
  **not** a guest capability or physical turn proof. Source lowering must
  pass an unforgeable native OresVM ActorRef + authenticated turn. Untrusted
  guest code must not call this API directly.

## Verification

`tests/native_actor_bytes_inbox_test.c` covers two independent actor heaps,
stale references across incarnation/nonce reuse, byte-exact owned copies,
binary control characters, no overread, count and byte quotas, retry after
dequeue, FIFO wrap, zero-length message, failed receive without dequeue,
zeroization after consume and retire, peer survival, and once-only destroy.

The standalone C test may be compiled without LLVM libraries:

```sh
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O2 \
  -DORESLANG_ACTOR_BYTES_TESTING \
  -I src/main/cpp \
  src/main/cpp/oreslang/runtime/NativeActorBytesInbox.c \
  tests/native_actor_bytes_inbox_test.c -o /tmp/atto-native-bytes
/tmp/atto-native-bytes
```

A normal CTest and a focused Clang ASan/UBSan CTest are registered. Source
org CI currently fails before any job step; local sanitizer success is
not an approved exact-head CI run and cannot justify merging without review.

## Still required for genuine Kafka LLVM source execution

1. Compile `ActorMail<Bytes>` and `rt take`/affine message transfer with
   compile-time ownership diagnostics; guard receiving with a physical
   OresVM turn. A trusted host using this API is not a guest execution test.
2. Have the OresVM lifecycle own/open/retire this inbox and reconcile byte
   capacity with its DATA mailbox and cancellation credits without
   double-admission or lost messages.
3. Add real nonblocking TCP and disk/file futures; physical asynchronous
   durability/fence semantics and legitimate shared-region reader/writer
   leases require their own verified implementations.
4. Run an actual compiler-generated Kafka `.atto` actor at Clang O0/O2
   and Java/GraalVM, and the same semantics in .NET Native AOT; execute
   official Kafka clients, SIGKILL/restart, fsync fault negatives, ASan/
   UBSan and exact-head approved CI.

This commit contains **no source-level Bytes actor execution**.
