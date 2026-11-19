# Core logging / OTel boundary — LLVM

**Audit date:** 2026-10-16. **Status:** compiler-side native CoreLog implemented and selectively tested; runtime/actor/userland parity **not established**.

Shared design: [core logging and userland OTel contract](https://github.com/ores-truffle-oreslang/oreslang-common-source/blob/main/docs/CORE_LOGGING_AND_OTEL.md).

## Verified repository status

The compiler remains an experimental source-to-LLVM subset. A native host-only `CoreLog` recorder now supports fixed numeric events, severity selection, bounded 8192-entry storage, cold stderr JSONL export, and compiler-failure instrumentation. General userland `oreslang-otel` imports and Java `CorePerf`/`CoreDebug` runtime parity are **not** demonstrated.

## Required backend behavior

- Core/internal standard-library diagnostics are controlled by host CLI/configuration, disabled by default, and independent of explicit userland `oreslang-otel` imports.
- Implemented **compiler CLI** flags: `--core-log-level=off|error|warn|info|debug|trace` (default off), `--no-core-log` and `ORES_CORE_LOG_LEVEL`. CLI precedence is last explicit flag wins; invalid final values return usage status 2. These do not configure compiled guest actor logging.
- Never perform guest message formatting, string interpolation, allocating event construction, filesystem/network export, or telemetry callbacks in a scheduler/actor/borrow checker critical path. Disabled paths must not sample the clock or evaluate an expensive payload.
- Keep guest stdout and functional errors unaffected by diagnostic settings. No guest payload, headers, credentials or arbitrary dynamic fields in core events.
- Version schemas; record numeric IDs and timestamps in bounded primitive buffers; export to host stderr only off the hot path.

## Implementation and test gates

Implement a native preallocated primitive flight recorder and explicit CLI parsing in the host/compiler (not in guest LLVM IR). Reject unsupported stdlib logging and otel imports at compile time. Use monotonically sampled integer phase durations and fixed numeric event IDs; defer stderr encoding. Do not change the scalar-only admitted grammar or claim actor/OTel parity. Add enabled/disabled/saturation tests and Clang/CTest regressions.

Do not mark parity green until a compiled/linked artifact has passed negative admission tests, CLI configuration-precedence tests, saturation and concurrent publication tests, and disabled/enabled performance tests. Record the exact source tree and CI run.

## Exact-source native implementation verification

- [GCC and Clang sanitizer test](https://github.com/ores-dummy-org-oreslang-stack/ores-dummy-org-oreslang-stack-web-server.ores/actions/runs/38979321782): fixed ID/long values, strict severity parsing, concurrent atomic publication, bounded 8192-event saturation and numeric-only output
- [GCC/Clang translation-unit syntax](https://github.com/ores-dummy-org-oreslang-stack/ores-dummy-org-oreslang-stack-web-server.ores/actions/runs/38979509721) for the actual `main.cpp` compiler entrypoint
- [CMake-built real LLVM executable with CLI execution](https://github.com/ores-dummy-org-oreslang-stack/ores-dummy-org-oreslang-stack-web-server.ores/actions/runs/38980000856): 17 exact source/fixture blobs mirrored into a funded **private** test repository; scalar `42` fixture emitted verified LLVM IR, invalid CLI level was rejected, disabled precedence suppressed export, and missing-source failure emitted the numeric `compiler.failed` event to stderr. The separate GCC and Clang ASan/UBSan tests also passed.

**Scope and remaining gates:** The above tests use exact 17 selected Git
blobs, not the complete LLVM Git root tree. No actor-runtime instrumentation,
guest imports, real userland OTLP, LLVM-side throughput/latency budget, or
native actor supervision parity is proven by these compiler-only tests.
