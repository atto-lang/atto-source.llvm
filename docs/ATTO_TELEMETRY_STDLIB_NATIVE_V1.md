# LLVM native Atto telemetry stdlib source candidate

`stdlib/telemetry.atto` is an independently maintained **LLVM-backend** Atto value/policy module. It is not an OTLP exporter, a compiler intrinsic provider, or a verified linked guest library.

- `Traceparent v00` helper checks decoded ID/flags nonzero and range only; trusted wire adapters must validate hex/width/delimiters and `tracestate`.
- `SpanSnapshot` is a **non-authoritative** value; guest-constructed copies cannot mint span or tenant capabilities. Its `snapshot` factory must stay **off** the hard-real-time hot path unless allocation is proven absent.
- The native C/C++ runtime owns fixed-size actor/tenant trace rings, event-drop accounting, exact-once scope tickets, clock reads, actor envelopes and continuation context, and asynchronous exporter admission.
- `ring_admissible` deliberately uses no additive `count + 1` or `required_bytes + used_bytes` that could overflow.
- Runtime probe activation is limited by its compiled ceiling. AOT compiled with `--instrument=none` lacks site probes. LLVM ORC dynamic JIT is a separately governed, opt-in mechanism and is **not implemented** by a `.atto` helper.
- OpenTelemetry `is_recording` differs from a sampled propagation flag; `RECORD_ONLY` is a valid decision.

## Target scoped use (NOT guest-verified)

```atto
use std.telemetry as telemetry;

pub fnc serve(): void {
  using telemetry.span("http.server") as span {
    span.set_attribute("http.request.method", "GET");
  }
  return;
}
```

No guest-defined `span` or `close_scope` is provided because the compiler/runtime must own an unforgeable scoped resource. Integrate with [Atto spec #167](https://github.com/atto-lang/atto-spec/pull/167), [common instrumentation #122](https://github.com/atto-lang/atto-common-source/pull/122), and the native LLVM runtime-trace work [#785](https://github.com/atto-lang/atto-source.llvm/pull/785).

### Required evidence before merge
Actual LLVM compiler import/parse/typecheck, linked `.atto` source execution, nested using/return/await/cancel/actor-restart semantics, fixed ring overfill/drop tests under ASan/UBSan, native Clang O0/O2 and hard-real-time overhead, W3C/OTLP receiver capture, exact SHA/stepful CI and independent review. This PR offers **source-only candidate code** and claims none of those executions.
