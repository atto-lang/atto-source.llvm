# Native supervisor STOP-only source plan (opt-in, not general guest LLVM parity)

This is a deliberately narrow parity slice corresponding to Java's separate
`compileStopOnlySupervisor` pathway. The normal native `parse_program()`
and LLVM executable source compiler **continue to reject** every
`define supervisor` declaration. Nothing here silently makes a source actor
or arbitrary supervisor hook executable.

## Exact admitted form

```atto
define supervisor Root as
  @Implements
  pub on_panic(f: Failure): void {
    self.stop(f);
    return;
  }
end
```

The explicit API `compile_native_stop_only_supervisor(source)` validates one
standalone supervisor, exactly one typed `on_panic`, and exactly one
`self.stop()` or `self.stop(f)` followed by `return;`. It returns an
immutable host-owned `NativeSupervisorStopOnlyPlan`. The separate
`NativeSupervisorStopOnlyHooks` turns a parent CONTROL lane's authenticated
terminal Panic into a single host-sink STOP request, using the native
`SupervisorFailureLane::Failure` including actor incarnation and
Throw/Raise-to-Panic origin.

This is a **host-installed decision plan**, not LLVM IR lowering. A trusted
runtime must still reserve the real child, establish physical retirement before
the failure is published, own the supervisor's logical turn, and independently
verify an uncertain external STOP effect before calling reconciliation.

## Fail-closed boundaries

- `async on_panic`, `await self.stop`, and post-await continuations are denied
  until an actual pinned CONTROL continuation and receipt protocol exist.
- `Failure.child` targeting is denied until a verified logical-child registry
  identity exists in the native Failure ABI. Arbitrary ActorRef targeting,
  duplicate STOP, restart, escalation, other hooks/effects and general statements
  are not part of this slice.
- Imports, modules, linked source, multiple supervisors, arbitrary annotations,
  extension methods, non-ASCII source tokens and reserved identifiers cannot
  become executable via this standalone admission path.
- A byte budget, bounded token list, bounded comment nesting, NUL rejection
  (including comments), and exact end-of-input rule prevent a second embedded
  declaration or text/lexical confusion.

## Evidence boundary and runnable gates

`tests/native_supervisor_stop_only_lexical_test.cpp` is a C++17 source-policy
boundary test. `tests/native_supervisor_stop_only_source_test.cpp` joins the
compiled source plan to the **production** native parent CONTROL lane and tests
exactly-one STOP plus uncertain quarantine. Both are CTest targets. The exact
source-head script `scripts/verify-supervisor-native-stop.sh <40-hex-commit>`
requires a clean checkout and executes all three C++ tests under Clang
ASan/UBSan and GCC O2 before emitting a PASS marker.

A local Clang sanitizer and GCC O2 run with the exact Git blob for
`NativeSupervisorStopOnly.hpp` passed 8 accepted and 35 rejected source
cases in an expanded *parser-only* harness. The adapter was separately
typechecked and exercised against an interface-compatible **local stub**;
that is **not** a production native runtime execution or a guest codegen pass.

Do not mark full native source supervision or cross-backend guest parity as
implemented. The conformance `sourceStopPlan` evidence flag remains false
for LLVM pending production exact-head execution, and `guestSourceParity`
remains false. Source-org Actions jobs are currently failing before steps,
and cross-org private-source copying remains under security incident hold.
