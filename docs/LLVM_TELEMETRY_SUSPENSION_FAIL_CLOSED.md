# LLVM AOT telemetry scopes — fail-closed suspension gate

This is an incremental test-only hardening on top of
[LLVM #1716](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1716).
The existing native `ores_native_telemetry_span_begin/end` stack is
`_Thread_local` and uses LIFO tokens bound to a physical carrier. It is not
a logical AttoLang `suspends` task/actor frame.

The canonical Java tracing continuation path in
[Java #3037](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/3037)
preserves one logical invocation scope across `await` and `rt cooperate`;
that must NOT be inferred from LLVM's working synchronous scope tests.

The expanded `tests/native_telemetry_using_aot.sh` adds compiler-negative
sources for `suspends fnc` with `await` inside a scope, `rt cooperate`
inside a scope, and nested spans containing an `await`. For each, native
compiler exit must be nonzero, stderr must contain a diagnostic, and stdout
must contain no partial LLVM IR. Synchronous source tests, -O0/-O2 native
execution and toggle receipts remain unchanged.

Milestone after this test gate: compiler-authenticated actor/task generation;
per-logical-task scope state with a bounded quota; separate immutable W3C
context; scheduler-controlled suspension/resumption that does not move
carrier-local mutable frames; terminal cleanup on cancellation, failure,
supervisor STOP and actor restart; emitted LLVM `-O0` and `-O2` guests
whose terminal receipts prove exactly one begin/end per invocation.

A source rejection is **not** evidence of LLVM async parity. Full source
tree + executed guest conformance, and independent review, remain required.
