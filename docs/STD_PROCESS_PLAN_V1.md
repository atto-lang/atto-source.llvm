# Native std.process design — LLVM backend


## Public API naming: asynchronous by default

In AttoLang, non-parking asynchronous operations are the **default** and
their names have no `_async` suffix: `spawn`, `collect_output`, `wait`,
`terminate`, and `kill`. The call sites may use `await child.wait()`
and `await child.collect_output()`. An intentionally synchronous/blocking
variant, if ever exposed, **must** end in `_blocking`. This is a public
semantic contract, not evidence that this backend implements OS process
launch or guest execution. Browser WebAssembly and synthesizable HDL remain
unsupported unless a separately authorized host provider exists.


## Public library path contract (2026-11-22)

Atto's preferred public import is `std/process`; `atto.std/process`
is the explicit, disambiguating built-in import. Apply the same rule to
`std/process_policy` and `atto.std/process_policy`. Do **not** publish
`ores.std/process` or `ores.std/process_policy` as new import names.
The two accepted spellings must resolve to the same compiler-owned module
identity and source, not two copies or an unchecked filesystem path.
The `atto.std/*` root must be reserved against local-package shadowing.

This backend's current relative fixture imports and source-only policy
**do not establish that either hosted import alias is implemented** in its
compiler. Resolver registration, positive/negative guest tests and an
exact-head execution receipt remain required before calling namespace
parity implemented.


`stdlib/std/process_policy.atto` implements guest-side, allocation-free
preflight arithmetic for child launch shape, stdio, actor budgets, bounded
capture, monotonic deadlines, termination escalation and exit success.
This is **not** an executable spawn implementation or host capability grant.

## Required native implementation

- Expose `await process.spawn(Command)` returning an owned
  `Result<ChildProcess,ProcessError>` via a narrow native runtime ABI.
- On POSIX, prefer `posix_spawn()` with explicit fd actions, close-on-exec,
  bounded argv/env, permission-checked executable/cwd and explicit process
  group ownership. No ambient shell or implicit PATH search. If any fork+exec
  fallback is indispensable, keep the forked child path async-signal-safe and
  exec immediately; never return into the multithreaded Atto runtime.
- `stdin/stdout/stderr` are nonblocking, bounded, backpressured channels.
  Integrate read/write and process-exit watches with the existing actor
  reactor; `wait()` never parks a carrier thread.
- Reap exactly once. Own a child-generation ticket, not just a PID. Avoid
  PID-reuse races. Linux pidfd may optimize but must not be required on
  every native target. Implement platform-appropriate exit observation.
- Owner cancellation or scope teardown closes pipes, requests termination,
  applies bounded escalation and reaps. Detached children require explicit
  authority and supervisor/reaper responsibility.
- Apply RUN permission at the native spawn boundary, independent of guest
  preflight, plus actor-local capability attenuation, command and env scope
  checks, budgets and cwd/file authority.
- Return structured spawn, timeout, cancellation, wait and output errors;
  never conflate an OS launch failure with an ordinary nonzero child exit.
- Keep this I/O-hosted module out of freestanding/embedded/hard-RT profiles
  unless such a target explicitly provides process capability.

## Current evidence

`tests/fixtures/std-process-policy-v1/main.atto` exercises only the
pure .atto policy (expected exit 42). No real guest compiler execution,
native child creation, AOT, async I/O or lifecycle test is yet verified.
Status: **SOURCE ONLY / NOT EXECUTED**.
