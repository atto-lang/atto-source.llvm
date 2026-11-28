# Native Atto `using telemetry.span` — LLVM AOT implementation candidate

**Status: Executed hosted LLVM AOT and synchronous ORC hybrid test coverage.**
This branch is additive to the first Java/Graal compiler slice
[Java #2993](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2993),
the [shared ABI specification #279](https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/279),
and [language specification #44](https://github.com/ores-truffle-oreslang/attolang-language-spec.md/pull/44).

## Admitted native subset

```atto
pub fnc main(): int {
  using telemetry.span("checkout") as span {
    return 42;
  }
  return 1;
}
```

The compiler parses a dedicated `UsingTelemetrySpan` statement, not a
numeric handle or a guest-implementable method call. It requires one
nonempty operation string literal (up to 256 UTF-8 bytes) after macro
expansion and a named lexical alias. An expression macro can provide it:

```atto
define macro operation(Syntax<Expr> value) -> Syntax<Expr> { return value; }

pub fnc main(): int {
  using telemetry.span(operation!(syntax expr "checkout")) as span {
    return 42;
  }
  return 1;
}
```

The compiler validates the expanded literal before emitting the same
span begin/end calls used for a handwritten name. The host can enable
recording at that site after the native binary has been linked.

The opaque alias may not be observed or
transferred by source expressions; it is only a scope marker for now.

The compiler rejects dynamic operation expressions, unknown factories,
aliases that shadow live owners, alias capture/return/assignment/call,
carrier migration and source Task/Future/actor operations in the
enclosing function. This scope form is host-target LLVM only until
separate portable runtime and suspended-scope proofs exist. The
existing source `using` keyword for effect declarations is unchanged.

The LLVM backend emits an actual `ores_native_telemetry_span_begin`
call at entry and an `ores_native_telemetry_span_end` call at exit.
Nested spans are closed LIFO, including structured return edges.
Tokens never enter source-level i64, native actor messages, or ownership
records. Compiler-reserved runtime symbols cannot be source-forged.

## Native C11 runtime, dormant probe semantics

`src/main/cpp/oreslang/runtime/NativeTelemetrySpan.[ch]` implements:
- Initially disabled global atomic enable bit; disabled `begin` returns 0
  without clock sampling or dynamic allocation.
- C11 thread-local 64-slot bounded scope stack; enabled begin timestamps
  with POSIX monotonic time. Saturation returns an inert token and increments
  the dropped count; it never allocates.
- Trust-boundary enable/disable toggle. An active span still closes if tracing
  switches off between begin and end.
- Token 0 closes as no-op; stale/out-of-order/cross-carrier close fails
  closed instead of silently damaging the live scope stack.
- Caller-owned thread-local metrics snapshot for proof/diagnostics. It
  is not a production OTLP exporter, nor a cross-actor trace-context API.

`ores_native_telemetry_set_enabled` is a trusted **host** API: the
native compiler does not expose a guest tracing-control capability.
In this first slice no OS signal/CLI/remote attach is wired. The
`span` alias has no guest `set_attribute` or `add_event` method
yet; LLVM must reject them rather than pretend to record telemetry.

## Verification receipts

**EXECUTED:** Native C runtime and exact source-identical
`tests/native_telemetry_span_runtime_test.c` were compiled and run
locally under GCC and Clang in C11 mode with warnings treated as errors.
Observed both pass. Git blob SHA-1 of all three locally compiled files
was checked against the SHA returned by GitHub for the corresponding
branch blobs:

- `NativeTelemetrySpan.h`: `6f3927b30e83ac4908d041fc7e77302024f5844f`
- `NativeTelemetrySpan.c`: `76d38ae2097dcebc1df4fc8b80a7a296cb6de688`
- `native_telemetry_span_runtime_test.c`: `59ea5f8b30f5505ba9a1ccd3fe9b216a854110bd`

**ALSO EXECUTED LOCALLY:** Carrier-tagged runtime tokens now encode
a bounded, globally assigned carrier identity and a per-carrier serial
counter; identical depth/serial on different threads cannot alias.
`tests/native_telemetry_cross_carrier_test.c` was compiled and executed
with **GCC and Clang**, including a real pthread attempt to close a
foreign token in a fork-isolated negative check; both passed. Exact
blob SHA `31560ffafb4050a3a1f716fea53819ee5369d7ff`.
The cross-carrier test is registered as
`native_telemetry_cross_carrier_nonforgeability` in Linux CTest.

**ALSO EXECUTED LOCALLY:** The exact committed C11 runtime test
passed with GCC and Clang under **ASan+UBSan**. Separately, a hand-authored
LLVM IR smoke fixture was linked with the exact runtime by Clang 17 and
executed; it exited with code 42 and the native host observed one clean
begin/end pair. **Hand-authored IR is not the Atto compiler.**

The hosted compiler and Clang-linked guest passed these CTests locally
with LLVM 19.1.7 and Apple Clang 21 on macOS arm64:

- `native_telemetry_using_scope_aot_guest` executed normal, early and
  nested return plus taken and untaken branches at `-O0` and `-O2`.
  The host checked the guest result and matched begin/end counts and
  zero active spans. Compiler denials cover handle escape, manual
  cleanup, unsupported suspension and invalid macro-expanded names.
- `native_telemetry_live_toggle_aot_guest` linked
  `tests/fixtures/telemetry-hybrid.atto` with the C runtime alone and
  checked recording toggles on the same AOT binary at `-O0` and `-O2`.
- `native_telemetry_hybrid_orc_guest` linked the same Atto guest as its
  baseline and used ORC to compile a new call boundary around the
  compiler-emitted guest IR. The call probe enclosed exactly one
  lexical span in the carrier's LIFO stack. Detaching while both spans
  were live returned new invocations to AOT and kept the ORC code alive
  until the old call closed both spans and returned.

The ORC target is a test host. The exercised path covers synchronous
carrier-local nesting and trusted fixture admission. Production signed
activation, W3C parent IDs, suspension and actor migration require
separate guest execution evidence.

```sh
cmake -S . -B build -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build --target oreslang-native-telemetry-hybrid-orc-test
ctest --test-dir build -R 'native_telemetry_(using_scope_aot|live_toggle_aot|hybrid_orc)_guest' --output-on-failure
```

## Required before declaring Java/LLVM parity

1. Run both executable guest suites against exact PR heads.
2. Reconcile semantic differences: Java currently accepts a nominal
   test-defined `SpanScope` and observer calls; LLVM currently uses
   an opaque compiler-owned AOT span. A common native Atto stdlib type
   must own the public contract across both backends.
3. Prove a bounded WCET and clock/probe budget before allowing
   `using` under `--profile=native-hard-rt`; it currently fails closed.
4. Implement full `ScopedResource` ownership/effect checks, avoid alias
   shadowing, and prove cleanup cannot throw or suspend.
5. Add task/actor-continuation trace-context transfer and explicit
   cancellation unwinds. The native TLS AOT span stack is insufficient
   for suspension or carrier migration.
6. Implement bounded OTLP-compatible native collector/export and
   permission checks, plus genuine live Graal Truffle JIT attachment.
7. Prove five-backend semantics independently for Forge, .NET and Wasm.
