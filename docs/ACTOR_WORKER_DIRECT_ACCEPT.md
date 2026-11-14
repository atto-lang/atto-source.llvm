# Worker-owned TCP listeners and direct actor `accept`: executable admission contract

Status (2026-10-24): **kernel host primitive and regression staged in this PR; 
source-level actor ownership, `std/net` lowering, and Java/LLVM parity are NOT implemented by this PR.**

## Goal

Four persistent, trusted actor workers should be able to wait for TCP clients
without any supervisor accepting a connection and then sending its descriptor
or a mailbox message to choose a worker.

Two valid implementations exist, with distinct semantics:

1. **Independent listeners (`SO_REUSEPORT` on Linux).** The supervisor
   provisions worker capabilities; each worker owns its own nonblocking
   listener socket bound to the same host:port. The kernel chooses a listener
   at TCP connection setup. Each actor's `await listener.accept()` suspends
   *that actor continuation*, not its scheduler carrier. No socket descriptor
   is shared between actors or passed per connection.
2. **Shared listening socket.** An explicitly shared listener capability may
   have multiple actor accept waiters. One VM I/O reactor owns its readiness
   registration and performs a bounded `accept4` operation; a bounded
   single-winner broker assigns each accepted connection to exactly one
   authenticated actor owner. A reactor callback cannot execute guest code.
   A duplicate `epoll_ctl(ADD)` per worker is **not** sufficient for this
   design; it needs one source of truth for cancellation and FD ownership.

For v1, prefer option 1 on supported platforms, with failure when
`SO_REUSEPORT` is absent. Do not silently fall back to supervisor dispatch.
Provisioning/binding a socket may be done by privileged control code, but
each actor performs its own accept/read/write after admission. Exact binding
API details are subject to source ABI and security review.

## Intended Oreslang source (not yet executable)

```ores
// Conceptual sketch; the actor method, module and errors require admission.
define actor TcpWorker as
  // One trusted worker's listening capability, never mailbox-sendable.
  // Exact type and capability-transfer syntax to be finalized.
  fnc run(listener: TcpListener) -> void
    loop
      let connection = await listener.accept()
      // The accepted connection is actor-owned, not supervisor-owned.
      // Parse and forward using source std/net on this actor.
    end
  end
end
```

**Do not treat this as implemented grammar or an importable stdlib surface.**

## Required actor/runtime invariants

- **Authority:** only trusted actors with scoped NET bind/listen permission can
  own listeners. Source `ActorRef` cannot forge the capability. A listener
  belongs to one exact actor incarnation, not merely an actor ID or thread.
- **No raw FD crossing:** binding and accepted socket descriptors remain in
  the host's opaque capability registry; no raw integer handles enter guest
  actor heaps or messages. `rt take` moves actor capabilities only after
  single-winner admission, never aliasing read/write ownership.
- **Carrier nonblocking:** actor source `await accept` creates a bounded
  generation-tagged readiness operation. No worker parks in `accept4`,
  `epoll_wait`, monitor wait, or `Future.get`.
- **Cancellation/stop:** if cancellation wins, the readiness registration is
  revoked and any accepted but unpublished FD is closed. If accept wins,
  ownership publishes exactly once to that actor. Stop fences further
  accepts, closes its listener after outstanding readiness operations are
  retired, then allows actor DONE and actor-arena reclamation.
- **Bounds:** per-listener outstanding waits, backlog, admitted descriptors,
  buffers and completion slots have explicit limits; load-shedding cannot
  strand Futures. Cross-worker fairness is best effort, not strict round robin.
- **Untrusted actors:** cannot bind or listen without a separately proven
  sandbox, network permission enforcement and FD authority broker.
- **Port reuse and reload:** every participant opts into `SO_REUSEPORT`
  before bind. Reload drains old actor sockets before reclaiming state;
  generation fences prevent old listeners routing new work into retired code.

## Evidence boundaries

- `NonblockingTcpListener::loopback_reuseport` opts in before `bind`.
- `native_worker_reuseport_accept_test.cpp` creates four real Linux
  listening sockets on one port, establishes 24 real TCP connections, checks
  one accepted FD per connection, verifies empty nonblocking reads of the
  accept queue, and rejects mixed/non-`SO_REUSEPORT` binding.
- This is a **host-only kernel regression**. It does not compile a `.ores`
  actor, prove actor identity/STOP, exercise Java NIO, test throughput,
  guarantee distribution among all workers, or certify production TLS/HTTP.

## Remaining acceptance tests (not yet present)

1. Compile and execute a `.ores` fixture with four trusted actors, each
   accepting directly from its own listener, on Java/GraalVM and LLVM.
2. Verify concurrent network connections each map to one exact actor owner,
   never through a supervisor mail or dispatcher; record accepted counts by
   worker, without requiring even hashing/distribution.
3. Race actor cancellation and parent/supervisor shutdown against
   connection readiness, descriptor creation, ownership publication and
   `await` continuation; prove no FD/operation/arena leaks or double-close.
4. Reject unauthorized actor, foreign context, stale actor incarnation,
   forged capability, exhausted quotas, and unsupported `SO_REUSEPORT`.
5. Execute exact-head CMake/CTest + sanitizers and JDK25/Graal tests with
   nonempty build/test steps, then benchmark on identical pinned machines.
