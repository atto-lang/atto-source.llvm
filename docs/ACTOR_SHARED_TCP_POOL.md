# Actor-shared TCP connection pool and HTTP agent (native host, v1)

Status: **host-only reference implementation**, 2026-11-04. Real POSIX
loopback sockets, not compiled `.atto` guest networking.

Files:
- `src/main/cpp/oreslang/runtime/ActorSharedTcpPool.hpp`: connection slots, leases, revocation.
- `src/main/cpp/oreslang/runtime/ActorHttpAgent.hpp`: HTTP/1.1 request serialization, strict response framing, keep-alive reuse.
- `tests/actor_shared_tcp_pool_test.cpp`: loopback suite (CTest `actor_shared_tcp_pool_loopback`).

## Goal

Many actors reuse a small set of physical TCP connections (HTTP keep-alive),
while each actor stays fully independent: no actor can read another actor's
bytes, use another actor's connection, or break a connection another actor is
using.

## Isolation rules

| Risk | Rule |
|---|---|
| Actor gets a raw FD/buffer and keeps using it | Actors only hold `PoolLease` (DATA: slot, generation, actor id + incarnation). FD access needs `PoolIoKey`, which only `ActorHttpAgent` can create. |
| Lease forwarded through a mailbox, or kept after release | Every call checks lease generation **and** the owning actor id + incarnation under the pool mutex. Every lease change bumps the generation. |
| Restarted actor reuses its old incarnation's lease | Incarnation is part of identity; the scheduler calls `revoke_actor` when an incarnation ends. |
| FD closed while another thread is mid-I/O; kernel reuses the FD number for someone else | A slot is closed only when not `busy`. Revocation calls `shutdown(2)` to wake the reader; the thread finishing the I/O does the `close(2)`. |
| Actor pipelines a hidden second request; its response is read by the next actor | The agent writes the request itself from a structured spec. CR/LF/NUL and framing headers (`Host`, `Content-Length`, `Transfer-Encoding`, `Connection`, `Upgrade`, `Expect`, ...) are rejected. |
| Stray or late bytes left on a connection | A connection is pooled only if the response ended exactly at its framing boundary. Idle connections are probed (`MSG_PEEK`) before reuse; any pending byte or EOF discards them. |
| Ambiguous response framing (smuggling) | Content-Length XOR chunked; differing duplicate Content-Length, bare LF, obs-fold, `101` upgrades are protocol errors and close the connection. |
| Cross-tenant reuse | `PoolOrigin` includes the tenant; connections never cross it. |
| Resource exhaustion by one actor | Fixed slot table, `max_per_actor`, `max_per_origin`, header/body byte limits, connect/IO timeouts. |

`PoolActor` must be the scheduler's authenticated current-actor identity. It is
never taken from guest data.

Retries: at most one, only for idempotent methods, only when a *reused*
connection failed before any response byte arrived (server closed an idle
keep-alive connection), and never after the actor's lease was revoked.

## Evidence (local, macOS arm64, 2026-11-04)

9 tests, real loopback sockets: cross-actor reuse on one physical connection,
forwarded/forged/stale/restarted-incarnation leases, `Connection: close`,
HTTP/1.0, close-delimited bodies, extra and late stray responses, malformed /
smuggling / oversize responses, request-injection rejection, stale keep-alive
retry, tenant partitioning and limits, revocation during a blocked exchange,
8 actor threads x 150 requests with no cross-talk and no FD leak, and a
revocation race stress.

| Build | Result |
|---|---|
| Apple clang 21, ASan+UBSan | pass |
| Apple clang 21, TSan (5 runs) | pass |
| Homebrew LLVM clang, ASan+UBSan | pass |
| GCC 15 (no sanitizer; macOS GCC lacks libasan) | pass |
| CMake + CTest target | pass |

Mutation check: disabling the idle probe, the owner check, the leftover-bytes
check, request header validation, or the CL+TE smuggling check each makes the
suite fail.

## Not done yet

- Not wired to the native scheduler: `revoke_actor` must be called from actor
  termination, and `PoolActor` from the authenticated actor turn.
- Blocking I/O on the calling thread. The epoll reactor integration (actor
  suspends, reactor completes, result delivered by mailbox) is next.
- No TLS, no HTTP/2, no DNS; IPv4 only.
- Mutex-based; not a hard-real-time path.
- Java, .NET, Wasm and Cranelift parity not started for the pool itself.
