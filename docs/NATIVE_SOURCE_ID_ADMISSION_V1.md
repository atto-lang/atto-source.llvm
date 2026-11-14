# Native source-ID admission v1 (LLVM backend)

**Status:** host-native metadata admission **plus a new opt-in LLVM compiler IR routine-ID attachment candidate**. The compiler path is not yet executed in this session; it is **NOT** runtime log-site probe insertion, linked guest AOT execution, LLVM ORC JIT or five-backend tracing parity.

This LLVM/C++ implementation is intentionally independent of Java/Truffle's AST and the Rust/Cranelift compilers. It validates the versioned common observable ID format while keeping backend-specific descriptor tables and codegen policy.

## Supported now

- Parse checked-in `rid_` / `sid_` lowercase 128-bit hexadecimal identifiers into fixed 16-byte native values.
- Reject wrong-kind IDs, uppercase/malformed/zero IDs without partially modifying the output parameter.
- Validate an already-resolved, bounded static site descriptor table with unique local indices and site IDs.
- Keep parsing and admission in a cold path; do not allocate/format ID strings on an ordinary log hot path.
- A standalone C++17 test can compile without LLVM libraries.

## Exact host-only test command

~~~sh
cmake -S . -B build-source-ids -DATTO_SOURCE_IDS_HOST_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-source-ids --parallel
ctest --test-dir build-source-ids --output-on-failure
~~~

Independently, the header and test compiled and executed in the authoring environment using Clang C++17 `-O1 -fsanitize=address,undefined` and GCC C++17 `-O2` with `-Wall -Wextra -Wpedantic -Werror -UNDEBUG`; each printed `LLVM_SOURCE_ID_HOST_METADATA_PASS`. This is a native **host-only** test, not a compiler guest execution proof.

## Compiler-owned LLVM IR metadata candidate (new draft code)

`LlvmCodegen.hpp` now exposes an opt-in `CheckedSourceRoutineV1` list.
`emit_verified_llvm_ir` validates its exact scalar callable coverage against
the LLVM backend's **own parsed `Program`**, accepts only well-formed `rid_`
IDs, rejects duplicate selectors and unknown functions, and attaches
`atto.source.routine_id.v1` metadata to the actual LLVM function declarations.
Actor/generator/class coverage is denied, rather than silently omitted.

A new internal `oreslang-llvmc --selftest-source-routine-ir` invokes the
**real native parser and LLVM IR code generator** on the Atto scalar
`pub fnc main(): int { return 42; }` fixture, expects the exact generated
routine metadata, and rejects wrong-kind, unknown-selector and duplicate IDs.
The full LLVM CMake branch registers this as the
`native_source_routine_id_compiler_ir` CTest target. With a compatible LLVM
SDK, run:

~~~sh
cmake -S . -B build-llvm -DCMAKE_BUILD_TYPE=Debug
cmake --build build-llvm --parallel --target oreslang-llvmc
ctest --test-dir build-llvm -R native_source_routine_id_compiler_ir --output-on-failure
~~~

**Execution status:** LLVM development libraries and `llvm-config` are not
available in the local test environment, and GitHub source-org jobs currently
fail before test steps. Thus this compiler codegen path is **committed but
UNEXECUTED**, not a passing LLVM guest proof. The available local results are
the independent native C++ descriptor tests and sanitizers.

**Scope:** the caller-supplied list is not yet loaded from an authenticated
checked-in package registry. LLVM metadata is not a live event or an
object-level authenticated source map; this work explicitly does **not**
claim `log.info` lowering, optimizer-stable runtime probes, trace propagation
or actual binary code-version retirement.

## Needed before implementation promotion

1. Link canonical package-relative source units, macros, overloads and checked-in source registry to LLVM frontend's **own typed MIR**.
2. Emit fixed descriptor indices and metadata into the actual compiled native object; prove source-map sidecar is paired with the deployed executable/build ID.
3. Independently insert native AOT probes into LLVM IR, verify all exceptional/cancellation/unwind exits and retained logical identities after optimization/inlining.
4. Implement authenticated `none/boundaries/functions` policies and OFF→ON→OFF→ON for *precompiled* probes. Disable all unapproved patching in hard real-time/strict sealed AOT.
5. Verify separately any real LLVM ORC compiler-produced variants; host function-pointer switching or a dormant-probe toggle does **not** prove JIT compilation.
6. Reuse OTel semantic event formats and actor/continuation trace context from common contracts, but do not import Graal, .NET, Forge or Rust compiler code.

References: [source identity specification #62](https://github.com/ores-truffle-oreslang/attolang-language-spec.md/pull/62), [common native instrumentation #279](https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/279).

## Literal-only compiled `log.info` slice (2026-11-14 draft)

The native LLVM frontend now recognizes `log.info("fixed literal");`
both inside an existing `using telemetry.span("name") as span {...}` and
in straight-line scalar function statements. Dynamic expressions, multiline
or control-character payloads, messages over 96 bytes, duplicate identical
literals in one function and unmatched manifest site rows fail closed.

`CheckedLogSiteV1` is an opt-in compiler input that must exactly match the
native parsed (function selector, literal) inventory. It binds a stored
128-bit `rid_` and distinct 128-bit `sid_` for each callsite; for this
first v1 slice, reusing the same message string twice in one function is
intentionally unsupported rather than ambiguously re-keying. The IR emitter
lowers each verified statement to a **real guest LLVM call instruction**
for `ores_native_telemetry_log_info` and attaches metadata on that instruction
with both static IDs. The four fixed 64-bit ID lanes are emitted as constants,
not allocated per execution.

The C11 backend runtime extends the existing native lexical span scope with a
32-entry **thread-/carrier-local**, allocation-free event ring. It copies
only the static IDs, at most 96 literal bytes, and the current native scope
token. The disabled path is an immediate no-op; the full ring drops events.
A same-carrier host drain erases retired events. This scope token is **not**
a W3C trace/span ID and is not yet safe across suspension or actor-carrier
migration. LLVM codegen therefore rejects async, actors, task/future and
Wasm/Cortex-M inputs in this mode.

Two reviewed fixtures are under `examples/telemetry/log-info-verified.atto`
and `examples/telemetry/log-info-ids.json`. The compiler currently requires
the opt-in C++ checked-site vector; the normal CLI **does not yet load this
manifest**. A new compiler selftest
`oreslang-llvmc --selftest-native-log-info-ir` parses real .atto syntax,
checks two compiled callsite IDs, and rejects missing, wrong-kind, duplicated,
stale and foreign site identities. It is registered under CTest name
`native_guest_log_info_verified_llvm_ir`.

**Executed native-runtime tests:** exact Git blob
`1bb720b06bc897df0d36399f8a143a94edf626db` (C runtime),
`ca5bedbf00fb2542729c3296f8d401bf47847cba` (header),
and `c572530721719a07ddfaf26eb4bee2401ead121a` (C regression test)
were independently reconstructed, SHA-verified, compiled and executed locally
using GCC -O2 and Clang -O1 with ASan+UBSan, -Werror and -UNDEBUG. Both emitted
`LLVM_STATIC_LOG_RING_HOST_PASS`. These are **HOST NATIVE RUNTIME tests**,
not executed LLVM guest compiler tests. The compiler IR selftest, linked
guest object, real OTel IDs and hard-RT proof remain **UNEXECUTED**.

After an LLVM SDK/authorized runner is available:

~~~sh
cmake -S . -B build-llvm -DCMAKE_BUILD_TYPE=Debug
cmake --build build-llvm --parallel --target oreslang-llvmc oreslang-native-static-log-ring-test
ctest --test-dir build-llvm -R 'native_(guest_log_info_verified_llvm_ir|literal_guest_log_ring_runtime)' --output-on-failure
~~~

The ordinary compiler CLI, source registry loading, separate artifact SHA
attestation, native executable linking and actor-aware OTel context are the
next required gates. No complete backend parity claim is made.


### Concurrent native-carrier regression

`tests/native_static_log_carriers_v1_test.c` (exact Git blob
`cbbf7aeb23c285cf79dece44c2716f53d553a6f2`) adds a Linux
`pthread` proof: eight real native carriers each attempt 1,500 static log
writes under separate nested TLS state. Every carrier retains exactly
32 events, reports 1,468 bounded drops, drains only its own messages,
and obtains a distinct nonzero span token. The host/main carrier sees
zero events and cannot drain another carrier's TLS queue.

The exact committed test was compiled together with exact committed
`NativeTelemetrySpan.c` and `NativeTelemetrySpan.h` and passed under GCC
C11 -O2 and Clang C11 -O1 ASan+UBSan, all with -Werror/-UNDEBUG:
`LLVM_STATIC_LOG_RING_CARRIERS_HOST_PASS:8x1500`. CTest targets are
`static_log_eight_carriers_host_only` and
`native_literal_guest_log_eight_carriers_runtime` for the respective
host-only/full-LLVM configurations.

These are pthread host-carrier tests, **not** Atto actor-message execution,
actor carrier migration, JIT generation or bounded hard-real-time proof.



### Deterministic 50,000-operation native runtime model

Added `tests/native_static_log_model_v1_test.c` (Git blob
`09730e88e630a0f22b727ac8b8154ec4263b18b9`), a deterministic
50,000-operation FIFO reference model for recording toggles, invalid
fixed-size source IDs/messages, capped queues, drops, drains and
stale/empty pending counts. The **exact committed test** passed with
GCC C11 -O2 and Clang C11 -O1 ASan+UBSan, `-Werror -UNDEBUG`.
The exact production C runtime/header blobs were also verified.
The test has independent full-LLVM and host-only CTest registrations.

A fresh local **CMake/CTest 3/3 PASS** comprised the literal ring,
8 × 1,500 carrier isolation and 50,000-operation model. This verifies
native **host runtime behavior only**; LLVM compiler IR, actual linked
Atto guest object, actor scheduler migration and OTLP remain untested.
