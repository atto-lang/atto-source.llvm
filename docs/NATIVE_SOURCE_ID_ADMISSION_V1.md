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


## Ordinary native compiler manifest intake (new experimental CLI)

The native LLVM compiler now accepts an opt-in *canonical package-relative*
`--source-log-ids=examples/telemetry/log-info-ids.json` argument alongside
`examples/telemetry/log-info-verified.atto`. The manifest bytes are read
through the compiler's existing bounded, regular-file and opened-file-identity
checking source reader and are compared to the exact CLI source-unit path.

A new independent C++17 backend header
`NativeSourceLogManifestV1.hpp` performs strict schema admission for the
checked-in JSON fixture: at most 128 KiB of **ASCII JSON with no escaped
strings**, exactly `schema` / `source_unit` / `routines` / `sites`, bounded
scalar callable signatures, valid nonzero `rid_` / `sid_`, unique routine
selectors and unique site IDs, exact parent routine ID and unique
(function, literal) site pairs. Unknown/duplicate fields, traversal/alias
source units, invalid kinds, stale sites, and unsupported encodings fail
closed. Unicode messages, repeated identical literal callsites, actors,
Wasm, Cortex-M, strict hard-RT, tree-shaking, and JIT are intentionally
outside this first backend-owned slice.

The ordinary compiler codegen call now receives the checked routine/site
inventory and **rechecks exact AST site coverage** before generating LLVM
function metadata and native logging calls. The new manifest option cannot
be combined with non-hosted and unverified optimization/real-time profiles.
Diagnostics redact the full manifest path as well as the source path.

For an SDK-backed compiler runner, the pending real guest build begins with:

~~~sh
cmake -S . -B build-llvm -DCMAKE_BUILD_TYPE=Debug
cmake --build build-llvm --parallel --target oreslang-llvmc
./build-llvm/oreslang-llvmc \
  --source-log-ids=examples/telemetry/log-info-ids.json \
  examples/telemetry/log-info-verified.atto > build-llvm/verified-log.ll
ctest --test-dir build-llvm \
  -R 'native_(guest_log_info_verified_llvm_ir|source_log_manifest_admission)' \
  --output-on-failure
~~~

**Real local host proof (not LLVM SDK/full Atto compiler):**
the exact checked-in C++ manifest header Git blob
`5c2ac63f3d96185e2e0969b553edf9fb93f4a016`,
test Git blob `64b236f3db06f2616ef38dfda6262be6f0cc1578`,
and JSON fixture Git blob `19ebece9eb7d485a626e61cabdd92e958f9f9e70`
were checked against an independent local reconstruction and passed
GCC -O2 and Clang -O1 ASan/UBSan with `-Werror -UNDEBUG`.
The test rejected 20 malformed, ambiguous, forged or mismatched inputs.
It is registered in both source-ID host-only and full SDK CTest modes.

### Source-free native LLVM IR linking ABI floor

A separate, **hand-authored and source-free** LLVM IR object
`tests/native_static_source_log_ir_abi_v1.ll` was compiled using Clang
`-x ir` and linked to the exact production C telemetry runtime plus
`native_static_source_log_ir_abi_harness_v1.c`. An actual native executable
returned 42, emitted the expected two static site-ID receipts, retained
one lexical native span token, and passed OFF→ON→OFF→ON logging.
It passed with Clang AddressSanitizer and UndefinedBehaviorSanitizer enabled,
and a local CMake host subset passed 2/2 for manifest admission + linked
source-free ABI. Both new source-free fixtures match exact committed Git
blobs. The repo CMake host-only and full SDK modes register this ABI test
only on supported x86_64 Linux with an available Clang driver.

**This is NOT AttoLang-emitted LLVM IR.** Its source-free linkage cannot
replace the still-unexecuted end-to-end Atto parsed source → compiler-generated
LLVM IR → linked native guest → runtime receipts test. A source-controlled
manifest is also not cryptographically authenticated provenance, a JIT proof,
or a native hard-real-time guarantee.


### Actual Atto compiler → native linked guest acceptance gate

The full LLVM SDK CMake branch now registers
`native_log_info_compiler_linked_guest_full_aot` on Linux x86_64 when a
Clang driver is present. This invokes the **ordinary** `oreslang-llvmc`
binary on the reviewed source file and `--source-log-ids` JSON manifest,
assembles the **compiler's own output IR**, compiles the production C
runtime and a lifecycle-only hook (no competing C `main`), and links a
native executable. The hook enables recording before the guest main and
drains from the same native carrier after main returns. Admission requires
**process exit code 42** plus both expected immutable site IDs and matching
lexical scope tokens.

Test sources: `tests/native_guest_log_info_full_aot.cmake` and
`tests/native_guest_log_info_full_aot_hook_v1.c`. The exact hook and
script Git blobs were independently reconstructed and tested locally with
synthetic IR standing in for the unavailable full Atto compiler. That
script-plumbing smoke returned 42 and passed the event assertions, and the
ABI script/hook SHA matches the checked-in files. **The actual
`oreslang-llvmc`-emitted linked guest test has NOT BEEN EXECUTED**, because
the LLVM SDK and source-organization funded runner are unavailable. Do not
promote the synthetic stand-in to full native AOT compiler evidence.

Suggested first full-SDK proof command:

~~~sh
cmake -S . -B build-llvm -DCMAKE_BUILD_TYPE=Debug
cmake --build build-llvm --parallel --target oreslang-llvmc
ctest --test-dir build-llvm \
  -R 'native_log_info_compiler_linked_guest_full_aot|native_guest_log_info_verified_llvm_ir' \
  --output-on-failure
~~~

The new C++17 manifest parsing test and source-free linked native IR ABI
floor are independently **executed and passing** locally, including exact
Git-blob verification. The whole compiler-to-guest proof remains an
**unexecuted acceptance gate**. Missing actor/JIT/W3C/OTLP tests remain
blocking for any larger tracing parity or hard-real-time claim.


### Physical manifest/source identity admission (post-audit hardening)

`NativeSourceLogPathsV1.hpp` is a backend-owned, standalone C++17
cold-path validator. With `--source-log-ids` enabled the ordinary CLI
now requires both source and manifest to resolve to *the literal physical,
package-relative paths* supplied to the compiler. It rejects symlink
aliases, `..` and `./` spellings, absolute paths, nonregular files, missing
inputs, and source/manifest hardlink aliasing. The regular-file source
reader then independently binds the opened file identity and read budget.
This strict path mode is deliberately opt-in and does not change normal
source import/symlink behavior.

`tests/native_source_log_paths_v1_test.cpp` exercises one positive path
and 11 negative cases, and is registered for host-only and full SDK
CTest on Linux. The exact production header Git blob
`2f10ce88259792ebf2783c69b852d33f6f469295` and test blob
`8f545d533b700fe0d4df67b6e1b1c84147b89c71`
were independently reconstructed and SHA-verified. Both GCC C++17 -O2
and Clang C++17 -O1 ASan+UBSan passed
`LLVM_SOURCE_LOG_PATHS_HOST_PASS:11` with `-Werror -UNDEBUG`.

This test proves ordinary path admission decisions, not atomic
filesystem snapshots against privileged concurrent mutation or approved
private-source export. The compiler SDK and actual Atto linked guest
remained unexecuted at the time of this report.

### Imported function provenance and diagnostic audit

The LLVM static-source-ID emitter now denies both declared imports and
compiler-flattened imported function names, including
`__ores_module_1.foreign`. ModuleLoader can flatten imports into
renamed native functions, leaving `Program.imports` empty. Without
checking the lowered symbols, a root-source ID could be attached to a
function that originated in a different .atto source unit. An exact
compiler IR negative selftest was added, but **has not run** without
the LLVM SDK.

The source/manifest physical-path gate also now catches
`std::filesystem::filesystem_error` and replaces it with a generic
path-independent diagnostic. The exact committed header/test Git blobs
(`95c41436d9e2234399ab256fe7ebf933b8a7a5f0` /
`aece9b1696c3556a7e75ba8b07db775ea0d2117f`)
were compiled and executed under GCC and Clang ASan+UBSan,
`-Werror -UNDEBUG`; both returned
`LLVM_SOURCE_LOG_PATHS_HOST_PASS:12`. The added check verifies that a
missing private input path is absent from the returned compiler error.
This host proof does **not** replace the still-unexecuted compiler AOT
and linked Atto guest tests.


### 2026-11-14 continued audit: exact typed source signatures and decoded controls

The native LLVM source-routine gate no longer treats function name + value
arity as sufficient proof of a manifest signature such as `main(int):int`.
It now independently checks the lowered callable's **return type, parameter
types and value/borrow modes**, plus rejects unresolved type aliases,
generic/method ownership and non-int return types. Five negative compiler
IR fixtures cover bool, borrowed, void and unresolved-alias collisions.
**These compiler fixtures have NOT executed** without an LLVM SDK.

The native parser now calls `NativeTelemetryLiteralV1.hpp` for both
`log.info` and `using telemetry.span`, rather than using only ASCII
control-character checks. The production header rejects decoded UTF-8 C1
controls (U+0080–U+009F) as well as C0/DEL and enforces 96-byte log and
256-byte span limits. This matches independent Java's
`Character.isISOControl` behavior for the covered C0/C1 range.

The **exact committed** header blob
`edbce85e60c0d297a37c2a1533c374bf17fda117` and 10,000-vector
test blob `4510257bd37352f1f533c84a4a908a396ab7ee9e`
passed under GCC C++17 -O2 and Clang C++17 ASan+UBSan,
`-Wall -Wextra -Wpedantic -Werror -UNDEBUG`. The physical path
suite was expanded with nested real paths, symlinked parents, FIFOs and
lexical aliases; exact test blob
`ac6bc8c94c29405344085c2e8483c082c2c9cd22` passed **16
negatives** with both compilers. An isolated CMake/CTest build covering
these two exact production validators passed **2/2 on GCC and 2/2 on
Clang ASan+UBSan**. The repository's host-only and SDK CMake targets
now register the native literal property test.

These tests prove real cold-path host validation only. The actual
`.atto` compiler-generated IR and linked guest, optimized AOT/JIT,
actor/span propagation and source SHA-linked CI remain unexecuted.
