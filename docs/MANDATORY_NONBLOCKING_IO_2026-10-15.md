# Mandatory native nonblocking I/O — 2026-10-15

Strict core I/O is unconditional. The native compiler admits no unsupported
filesystem/network/HTTP/stdio binding and emits no partial LLVM IR. The optional
`--core-io=native-async-only` assertion does not enable a mode: it restates the
default. A blocking/off/unknown policy or duplicate assertion is a usage error.
Tests exercise no flag, the assertion, all unsupported operations under both,
and downgrade attempts. No blocking-compatibility option exists.

The existing Linux TCP kernel uses SOCK_NONBLOCK, MSG_DONTWAIT and MSG_NOSIGNAL.
`try_accept` now independently checks the listener's O_NONBLOCK state before
accept4, whose flags only affect accepted sockets. The Linux regression clears
the listener's flag, requires immediate EINVAL, then restores it.

23 focused CTests passed on LLVM 20/macOS, plus the function-effect bitset gate.
Linux transport execution requires a Linux runner; an authorized private
fallback passed admission in led-dynamo/leddy-e2e. Exact-tree snapshots and job
receipts are stored in the org-root audit-reports/2026-10-15-mandatory-async.
These focused checks are not full CTest or origin merge-check approval.

The native socket substrate is not guest I/O/continuation lowering. Until a
typed registry, buffer ownership and completion wakeup path are implemented,
guest I/O continues to reject. There is no hidden worker or Java fallback.
The Java/Graal branch similarly defaults to mandatory enforcement and now
returns structured Oreslang test records rather than writing synchronous stdio.

The all-backend 22-case source corpus is pinned in tests/mandatory-async-io.json.
CTest native_shared_mandatory_async_source checks both default and explicit
strict assertion paths without emitting partial IR for unsupported operations.
