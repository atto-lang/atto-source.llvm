# Asynchronous-only I/O: native LLVM contract

Status: **implemented host lifecycle/readiness substrate; source-level I/O remains fail-closed**.

Oreslang core/std I/O uses canonical names that are asynchronous by default.
There is no duplicate `*_async` surface and core/std does not expose
`*_sync` or `*_blocking` fallbacks.

Examples of the intended source surface:

| Source operation | Type |
| --- | --- |
| `network.connect(host, port)` | `Future<TcpConnection>` |
| `socket.read_line()` | `Future<Option<String>>` |
| `socket.write_text(text)` | `Future<void>` |
| `socket.close()` | `Future<void>` |
| `fs.read_text(path)` | `Future<String>` |
| `http.get_text(url)` | `Future<String>` |

Sequential source semantics use `await`; the canonical name itself never
authorizes a synchronous host syscall.

A transport is conformant only when its host operation is genuinely
readiness/completion driven. Moving `read`, `write`, `connect`,
`getaddrinfo`, file I/O, or synchronous HTTP onto a worker/platform/virtual
thread does not satisfy this contract.

## Native lifecycle implemented on this branch

The current host substrate now separates every relevant state transition:

1. `AsyncIoCompletionGate` provides generation-checked,
   single-winner terminal arbitration for
   `(actor_id, generation, operation_id)`.
2. `AsyncIoOperationLifecycle` separates logical terminal settlement from
   kernel registration, callback ownership, and physical retirement.
3. `AsyncIoEpollOperation` binds one operation to one-shot Linux epoll
   readiness, nonblocking rearm after EAGAIN, cancellation, timeout and
   deregistration.
4. `AsyncIoContinuationGate` binds an operation to an AWAIT continuation and
   refuses to publish scheduler-visible READY until the operation is physically
   reclaimable.
5. `AsyncIoActorLifetimePin` holds the exact actor incarnation's
   generation-fenced native resource loan until physical I/O retirement.
   Consequently ActorGroup resource reclamation and `get_done()` cannot
   complete merely because the Future settled logically.

The epoll/completion path never runs guest code. It publishes an opaque
continuation readiness transition; the owning scheduler must claim the
READY continuation on a legitimate actor/root turn.

## Linux TCP/readiness substrate

`NonblockingTcpLinux.hpp` uses:

- `SOCK_NONBLOCK | SOCK_CLOEXEC`;
- `recv(..., MSG_DONTWAIT)`;
- `send(..., MSG_DONTWAIT | MSG_NOSIGNAL)`;
- nonblocking `accept4`;
- one-shot epoll readiness;
- eventfd wakeups reserved for the reactor.

A dedicated reactor may park in `epoll_wait`. Actor, ROOT, CONTROL and ordinary
scheduler carriers may not wait there.

Backpressure is explicit: partial transfers preserve progress and EAGAIN/
EWOULDBLOCK returns a would-block state that is rearmed through one-shot
readiness rather than retried in a blocking loop.

## Cancellation and actor retirement

Cancellation/timeout/completion compete for one logical terminal winner, but
winning does not imply physical safety.

For a suspended actor operation:

- cancellation may revoke PENDING or READY continuation state before CLAIMED;
- stale actor generation, operation id, source token or code generation is
  rejected;
- a callback that loses terminal arbitration still must leave callback
  ownership;
- epoll registration must be removed;
- the operation frame must reach physical retirement;
- only then may the actor's I/O lifetime pin be released;
- only then may ActorGroup resource reclamation permit `get_done()`.

This explicitly prevents a cancelled or completed Future from freeing actor
memory while a native callback can still touch its operation frame.

## Current source-level status

The LLVM compiler still does **not** admit general guest core/std I/O.
Unsupported operations must remain fail-closed until the compiler has:

- a typed I/O registry for canonical names;
- Future/await lowering to the native continuation store;
- actor/task ownership and cancellation authority;
- bounded operation-slot and wake admission;
- buffer ownership across suspension;
- capability/permission checks before submission.

Do not expose the host epoll classes directly to guest source and do not add a
blocking compatibility path to make source fixtures pass.

## Provider ordering

The first real source provider should be TCP because it establishes the reusable
semantics needed by higher layers:

1. TCP connect/read/write/close;
2. stdio/pipes where descriptors can use readiness;
3. asynchronous DNS;
4. HTTP/TLS built on the network provider;
5. filesystem only where a genuinely asynchronous completion backend exists.

If a target lacks such a backend, that operation remains unsupported.

## Required evidence

The shared `oreslang.async-io/1` contract requires, for a provider:

- canonical API behavior;
- immediate and delayed readiness;
- partial reads/writes and write backpressure;
- EOF and peer reset;
- cancellation before readiness;
- cancellation after readiness but before resume;
- timeout/completion races;
- close with pending operation;
- stale-generation rejection;
- one terminal winner;
- bounded buffering and no busy polling;
- scheduler progress while I/O is pending;
- logical settlement before physical retirement;
- physical retirement before actor done.

The branch contains focused CTest targets for operation lifecycle, epoll
readiness/rearm, continuation retirement, epoll-to-continuation integration and
actor-done physical-retirement fencing. These source tests are not a substitute
for an executing exact-head Clang/GCC/sanitizer run.

A GitHub Actions workflow with zero executed steps is runner-admission evidence
only and establishes neither pass nor failure of this implementation.
