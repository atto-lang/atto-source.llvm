# Core logging / OTel boundary — LLVM

**Audit date:** 2026-10-16. **Status:** documented target only; implementation parity **not established**.

Shared design: [core logging and userland OTel contract](https://github.com/ores-truffle-oreslang/oreslang-common-source/blob/audit/core-logging-separation-2026-10-16/docs/CORE_LOGGING_AND_OTEL.md).

## Verified repository status

The compiler is an experimental source-to-LLVM subset. Actor instrumentation tests exist, but neither general userland `oreslang-otel` imports nor parity with Java's `CorePerf` and `CoreDebug` CLI diagnostics are demonstrated.

## Required backend behavior

- Core/internal standard-library diagnostics are controlled by host CLI/configuration, disabled by default, and independent of explicit userland `oreslang-otel` imports.
- Future canonical core flags: `--core-log-level=off|error|warn|info|debug|trace` and `--no-core-log`. These are **not currently declared implemented for this backend**; do not advertise them as usable.
- Never perform guest message formatting, string interpolation, allocating event construction, filesystem/network export, or telemetry callbacks in a scheduler/actor/borrow checker critical path. Disabled paths must not sample the clock or evaluate an expensive payload.
- Keep guest stdout and functional errors unaffected by diagnostic settings. No guest payload, headers, credentials or arbitrary dynamic fields in core events.
- Version schemas; record numeric IDs and timestamps in bounded primitive buffers; export to host stderr only off the hot path.

## Implementation and test gates

Implement a native preallocated primitive flight recorder and explicit CLI parsing in the host/compiler (not in guest LLVM IR). Reject unsupported stdlib logging and otel imports at compile time. Use monotonically sampled integer phase durations and fixed numeric event IDs; defer stderr encoding. Do not change the scalar-only admitted grammar or claim actor/OTel parity. Add enabled/disabled/saturation tests and Clang/CTest regressions.

Do not mark parity green until a compiled/linked artifact has passed negative admission tests, CLI configuration-precedence tests, saturation and concurrent publication tests, and disabled/enabled performance tests. Record the exact source tree and CI run.
