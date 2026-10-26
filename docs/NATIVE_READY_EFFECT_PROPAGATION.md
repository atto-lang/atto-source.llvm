# Narrow readiness effect propagation review receipt

Base: PR #928 head `5560ec59065ed77beb0800d9bdae2221e1c20fe5`.

The existing compiler records `throws ActorTerminated`, but its readiness
bridge asserts READY. This change adds an explicit i32 status and i64 result
output to the readiness runtime projection. Startup states are mapped to
effect tags (return=0, ActorTerminated=1), not exposed as guest scalar results.
Failure preserves result storage and never runs the guest continuation.

Only the compiler-owned, scalar, zero-argument main gets a private hidden i32
effect output. It stops/joins the actor on either outcome, then returns status
to the OS wrapper. The wrapper checks status before interpreting the scalar
exit result. An unhandled ActorTerminated prints a diagnostic and exits 1,
without a C assertion or guest continuation execution. Guest calls to this
special entrypoint reject rather than silently discarding the effect.

No general catch/trap, inter-function throws ABI, payload, arbitrary Future,
or cleanup/unwind lowering is claimed. Const-only tokens and DONE are unchanged.
DONE still observes physical terminal ACK before resumption and reclaims the
opaque actor/heap after its resumed continuation returns.

## Local validation

The exact base runtime sources plus this patch were compiled with GCC using
C11, pthreads, assertions, and -Wall -Wextra -Wpedantic -Werror:

| Check | Result |
| --- | --- |
| HasFuture C++17 admission matrix | PASS: 9 admitted / 951 rejected |
| Stop-before-READY runtime: all three races + successful control | PASS |
| Same readiness runtime, ASan/UBSan | PASS with ASAN_OPTIONS=detect_leaks=0 |
| Same readiness runtime, TSan | PASS |
| DONE physical-ACK / ancestry / queued-abandonment runtime | PASS |
| Same DONE runtime, ASan/UBSan | PASS with ASAN_OPTIONS=detect_leaks=0 |
| Default LeakSanitizer teardown | ENVIRONMENT ERROR: cannot access /proc task data |
| LLVM compiler build, IR contracts, real compiled-source AOT | NOT_EXECUTED: LLVM development toolchain unavailable |
| Rust AOT harness compilation | NOT_EXECUTED: rustc unavailable |
| Java/GraalVM parity | NOT_EXECUTED |

The Rust AOT harness now requires the checked readiness call and OS effect
branch for all three source forms. On Linux, linker wrapping holds constructor
admission, requests stop, and releases admission before the real readiness
call. It requires process exit 1, ActorTerminated diagnostics, no post-READY
messages, no constructor call, and actor reclamation. Existing release and
ASan/UBSan CTests invoke this harness; these authored guest tests still require
real compiler execution. Non-Linux hosts retain success AOT coverage and the
portable C failure/race tests.

Review/execution is required before promotion. Local runtime tests do not
establish generated LLVM correctness, exact-head Actions success or parity.
