# AttoLang native AOT tracing policy gate — partial LLVM slice

**Status:** internal host-only policy primitive + source/CMake test. **No LLVM-compiled Atto tracing guest, W3C context or OTLP exporter is implemented or verified by this change.**

`NativeTraceProbePolicy.hpp` provides a per-runtime-domain atomic `OFF/BOUNDARIES/CALLS/FULL` mode with a compiled coverage ceiling, release-published generation, and sealed/hard-real-time refusal. It is header-only, does not read clocks, allocate guest objects, invoke callbacks or perform I/O. It is not a runtime JIT and cannot create code sites that were removed by AOT. Host control-plane authentication, runtime plumbing and disabled-path allocation/cycle measurements remain separate requirements.

`tests/native_trace_probe_policy_test.cpp` tests disabled default, mode ceiling, transitions, sealed/stripped binaries, independent domains, and concurrent reads/toggles. It is registered in both normal CMake testing and `-DORESLANG_PROXY_SLAB_HOST_ONLY=ON`, allowing a minimal LLVM-free host test.

The equivalent Java/Graal internal assumption gate is in [PR #2989](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2989), and the public conformance contract is in [common-source PR #278](https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/278).

## Next actual guest gates

1. Add the `using telemetry.span(...) as span { ... }` syntax to the LLVM frontend and keep transport-borrow `using` semantics separate, with verified no-throw RAII/unwind and no mutable scope escape.
2. Define the versioned `TraceContext`/probe ABI and instrument actual LLVM function entry/exit, actor delivery and async/task await in the generated native code.
3. Implement the same native `.atto` telemetry scope API that Java is introducing, including no-op behavior and actor-owned bounded collection.
4. Connect real native HTTP receive, actor mailbox context propagation, and outbound HTTP `traceparent` injection to a bounded asynchronous Atto exporter, then verify captured OTLP/JSON spans.
5. Compile and run the *same byte-pinned guest* that passes Java/Graal. Record exact source/IR SHA, compiler command, guest exit, raw HTTP wire, OTLP receiver data, nonzero CI steps and no silenced failures.
6. For hybrid/JIT targets only: a **future governed** ORC JIT could compile a new instrumented version of a function from retained verified typed MIR, publish it through a generation-fenced dispatch cell and reclaim only after frame quiescence. Sealed AOT and hard-RT targets must use precompiled hooks or redeploy; no executable memory patching or monkey patching.

A local independent GCC 14 C++17 compile/run of the gate logic passed. It is **not an exact-source CMake/CTest or LLVM guest receipt**. No five-backend parity claim is made.


## W3C wire-context native ABI extension

`src/main/cpp/oreslang/runtime/NativeTraceParentV00.hpp` now defines
a small, allocation-free `NativeTraceParentV00` (16-byte trace ID, 8-byte
span ID and flags) with strict fixed-width version-00
`parse_traceparent_v00`, `format_traceparent_v00`, nonzero-ID validation and
`trace_child_context` (caller must supply its own securely minted child ID).
It is **not** the existing `NativeLogicalTrace` execution/continuation ancestry;
they serve distinct purposes, and no pointer or actor payload enters the wire
context. This helper does **not** generate secure entropy and does **not**
support arbitrary versions, `tracestate`, or export.

`tests/native_traceparent_v00_test.cpp` is registered in the
standard and LLVM-free host-only CMake profiles. An **isolated exact-GitHub-blob host compile/test** passed for header blob
`643e7e3a5ac826a516ae8cbbee0f7f19274163dd` and test blob
`f6c1e612e9493ceb1b99b50681e9e05783da5c85` under GCC 14.2 and
Clang 17 C++17 with `-Wall -Wextra -Wpedantic -Werror`, `-UNDEBUG`,
AddressSanitizer and UndefinedBehaviorSanitizer. Both compiled binaries
exited 0. **This is NOT an exact-head LLVM repository CMake/CTest run**
and does not execute Atto language source.
The LLVM-native Atto guest cannot yet parse `using telemetry.span`, execute
native source-level HTTP/actor propagation, or emit OTLP; full conformance
remains **NOT_IMPLEMENTED**.
