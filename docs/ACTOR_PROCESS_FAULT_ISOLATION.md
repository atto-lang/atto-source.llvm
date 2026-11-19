# Native Atto process fault isolation — initial host substrate

## Implemented scope

`native/` contains a C11/POSIX **host demonstration** of a crash containment
boundary, independent from the legacy Oreslang LLVM runtime and independent of
Atto guest-code generation. Each `atto_actor_run_isolated()` invocation creates
an external worker through `posix_spawn()`; the supervisor does not call a guest
function after `fork()` on a multithreaded runtime. A crash is reported as a
structured notice identifying actor, supervisor, and incarnation. A trusted
caller may translate the notice into its own priority supervisor CONTROL
message, but **that mailbox integration is not implemented here**.

The typed results distinguish clean exit, nonzero exit, signal termination,
timeout, failed spawn, and failed wait. A SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, or SIGKILL in one worker does
not terminate its supervisor process. A concurrent healthy worker completes
while a second worker crashes. The worker gets only PATH/LC_ALL in its explicit
environment. The parent waits on that one child PID, not `waitpid(-1)`.

## Worker launch hardening (2026-11-19)

The native host now launches an external worker with a **default-deny inherited
file-descriptor policy**. glibc >= 2.34 uses atomic
`posix_spawn_file_actions_addclosefrom_np(3)`; macOS uses
`POSIX_SPAWN_CLOEXEC_DEFAULT`. FDs 0–2 are explicitly redirected to
`/dev/null`. The host **fails closed with ENOTSUP** rather than launching a
worker when no atomic default-close facility is available. This prevents
unintentional inherited supervisor files/sockets, including descriptors opened
concurrently by other threads. Future authenticated message channels must use
an explicit FD capability allowlist, not implicit inheritance.

The child receives a minimal environment, an empty inherited signal mask, and
restored defaults for common fatal/termination signals. Tests explicitly
exercise leaked FD 123, redirected stdio, parent-blocked SIGTERM and
parent-ignored SIGPIPE, in addition to the original crash/sibling cases.

**This is still a host substrate, not native Atto actor isolation.**
`runtime/actor_runtime.c` currently invokes generated actor code from
`pthread` workers inside a shared process. Native SIGSEGV there is
process-fatal and cannot be made safe by `sigaction`, `recover`, or catching
`SIGSEGV`. Do not mark actual Atto guest crash containment as passed until
compiled actor bodies execute behind the process/tenant boundary, their
mailboxes and generation-fenced CONTROL notices are wired to the supervisor,
and exact-head guest fault-injection passes.

## Explicit non-guarantees

This is **not** a security sandbox or a complete actor implementation. It does
not integrate Atto source actors, mailboxes, ownership/borrow checking,
restarts, exception hooks, actor heaps, arena quiescence, or AOT lowering. Workers can still use their inherited OS identity/privileges and signal
same-UID processes. A production untrusted-worker policy requires independently
enforced sandbox confinement, authenticated IPC, per-worker CPU/memory
quotas (cgroups/job objects), no privilege to signal the supervisor, and
supervisor-controlled resource retirement. It cannot recover from SIGKILL of the supervisor process,
whole-machine OOM, or compromised trusted host code. There is no recovery by
continuing execution in a SIGSEGV signal handler.

The current implementation is intended for POSIX/Linux/macOS hosts; embedded,
Windows NativeAOT, GraalVM, Wasm, and other runtime backends require their own
independent platform-specific implementation.

## Compile and verify

```sh
cmake -S native -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The test spawns an external executable and checks isolated SIGSEGV, SIGBUS,
SIGILL, SIGFPE, SIGKILL, SIGABRT,
nonzero and clean exits, a hung worker killed at a bounded deadline, inherited
environment sanitization, descriptor/stdio closure, inherited signal-state
reset, and a healthy concurrent sibling. Expected child
signals are intentional fault injection; do not interpret an individual child
crash as a failed parent-host test.

## Integration requirements

- Add an authenticated supervisor CONTROL lane to report the notice once.
- Support concurrent receiver quiescence and actor-owned arena retirement
  *after* verified last-use; **never** free actor-owned memory merely because
  `waitpid` reports a worker has died if other actors can hold loans.
- Move from a trusted worker executable to a sandboxed worker with strict
  inherited-FD policy, memory/cpu budgets, mailbox IPC, and control-plane
  admission. Guard fork+exec/posix_spawn semantics on each target OS.
- Generate AOT-compatible Atto worker entrypoints and native actor lifecycle
  tests, then add true guest `.atto` conformance fixtures and exact SHA CI.
