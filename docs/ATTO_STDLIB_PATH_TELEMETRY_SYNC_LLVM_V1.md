# LLVM-owned Atto std/path, std/telemetry and std/sync — v1

The three sources in `stdlib/std/*.atto` belong to the LLVM compiler repository.
They are **backend-local**; they do not import the Java or .NET implementations.

## Supported source-level policies

- **path:** separator classification, safe UTF scalar and conservative Windows scalar checks, lexical component/dot/dotdot categories, checked depth change with 256-component budget, drive prefix, 4096-scalar input budget.
- **telemetry:** W3C version-00 *decoded fields* checks, OTel-style sampling decisions, timestamp windows, dormant precompiled AOT-probe admission, bounded batch admission and saturating drop counter.
- **sync:** lock acquisition/queue predicates, writer-priority reader admission, permit overflow-safe release, barrier and epoch transition checks.

## Non-claims and security boundaries

These files alone do **not** implement normalized path serialization,
symlink/root confinement, Unicode casefolding, trusted W3C wire parsing,
context propagation, OTLP networking, mutex/RWLock atomicity, lock ownership,
scheduler wakeups or actual semaphore/barrier scheduling. The necessary trusted
compiler/runtime backends must be implemented and tested independently.
No blocking APIs were added. No host language code was placed in these `.atto` files.

`tests/fixtures/stdlib_path_telemetry_sync_v1.atto` is a 24-branch
proposed guest behavior oracle. It is not a PASS until an exact-source
LLVM compiler imports all three modules and runs the guest. Unsupported
imports should fail explicitly rather than fall through to host behavior.

Related independent implementations: Java
`atto-source.java#1756` and .NET `atto-source.net#209`. No
byte-identity or cross-backend guest-parity claim is made.
