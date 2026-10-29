# Browser native AOT / C ABI v1 handoff

Status: **single scalar export prerequisite implemented; browser ABI is NOT implemented**.

The Qt browser host header is
`ores-truffle-oreslang/oreslang-web-browser.ores/include/ores_browser_ui_abi_v1.h`.
The compiler must produce a native library that implements the five symbols
with their **exact C ABI type/signature**. Exporting a guest Oreslang
`int` symbol with the right name is insufficient: LLVM currently lowers
guest `int` to `i64`, while the ABI requires `uint32_t` / `int32_t`.
Never cast a 64-bit function signature to a 32-bit C function pointer.

| Export | Required C type | Minimum ownership behavior |
| --- | --- | --- |
| `ores_browser_abi_version` | `uint32_t (void)` | Return version 1 |
| `ores_browser_open` | `int32_t (const OresBrowserConfig*, OresBrowserHandle**)` | Fresh trusted Oreslang session; validate struct size/version; own handle |
| `ores_browser_dispatch` | `int32_t (OresBrowserHandle*, const OresBrowserCommand*)` | Typed bounded command; authenticate tab id/generation; actor-owned mutation |
| `ores_browser_snapshot` | `int32_t (OresBrowserHandle*, OresBrowserSnapshot*)` | Bounded, zero-padded, revisioned, consistent snapshot |
| `ores_browser_close` | `void (OresBrowserHandle*)` | Quiesce actors, join and retire handles with generation fencing |

### Executable P0 prerequisite

`tests/fixtures/browser-aot-scalar-export.ores` and
`scripts/verify-browser-aot-scalar-export.sh` prove the **current**
scalar-only ABI through these steps (when a real LLVM compiler and runner
execute the script):

1. Pin clean exact source commit and compiler SHA256.
2. Compile a real `.ores` `pub fnc` through `oreslang-llvmc --target=host`.
3. Verify `define i64 @ores_browser_aot_scalar_probe()` in generated LLVM IR.
4. Clang links it into a native position-independent `.so`.
5. Strict C11 `dlopen`/typed `dlsym` executes the compiled guest function
   and verifies its result.
6. Assert none of the five browser ABI exports are present in this canary.
   Their absence is intentional until the correct ABI is implemented.

The canary does not allocate browser handles, emulate tabs in C, run an
Oreslang actor, or imply browser functionality. A separately labeled synthetic
IR probe can validate the C harness itself, but **cannot** satisfy step 2.

### October 29 host audit and runner status

The source-native LLVM test still has no executed Actions steps
(latest known browser AOT run
[#40957352865](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40957352865):
`runner_id=0`, zero steps). **No guest compiler verdict exists.**

The host acceptance program is covered by
`scripts/test-host-export-harness.sh`. It compiles synthetic LLVM IR
(not Oreslang), then asserts:
- Valid exported `i64` probe is callable.
- Wrong return value, absent symbol, forged browser ABI symbol, relative
  library path, and corrupt library are all rejected.

Local compilation and each positive/negative test executed. The tested
synthetic-host script was verified byte-for-byte against Git blob
`4d07e8d341fbd2ef9090cd5e0a2bc0ef446e2d4d`.

The real-compiler acceptance script now requires a native ELF executable
(`readelf`), a pinned compiler SHA256, exact clean checkout, strict Clang
shared-library linking, and a **defined** text export on the built ELF
(`nm -D --defined-only`). It rejects extra browser symbols both in the
generated LLVM IR and in the linked library. A shell-script fake compiler
was confirmed to fail this ELF gate locally. The successful standalone
host harness checks only synthetic inputs and is not source execution.

An ELF format check and SHA pin **cannot independently prove the compiler
binary's provenance**: a deliberately malicious replacement ELF can still
spoof an export. Production certification requires a separate trusted,
verified build-from-exact-source attestation and actual native execution.

The private alternate Actions candidate
`usa-acc/usa-acc-e2e` executed its source-free C/Clang canary
[run #40956752596](https://github.com/usa-acc/usa-acc-e2e/actions/runs/40956752596)
with runner ID `1000001855` and four successful recorded steps.
**No LLVM source was copied there.** The independent source-specific
[approval issue #1172](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1172)
is open; any cross-organization private tree transfer remains blocked.

### Required next compiler/runtime slices

1. Introduce an *explicit* external C ABI declaration/export facility
   preserving exact width, calling convention, structure alignment and opaque
   pointer ownership. Reject unsupported C signatures without emitting LLVM IR.
2. Compile actor `state` and inline `handle_state(critical |s| -> ...)`
   with effect rows, static borrow verification, no escaping mutable loans,
   and no suspension/reentrancy within the state loan.
3. Connect `BrowserSupervisorActor` to trusted native entry glue.
   Keep tab IDs, generations, selected tab, revision and all security policy
   solely in compiled Oreslang. Qt/C++ is an FFI/OS adapter, never a fallback.
4. Implement all five exports, atomically validate/copy bounded commands,
   reject stale/double-closed handles and malformed snapshots, and make
   teardown race-safe.
5. Run the **actual** browser `tests/abi/backend_integration.c`, Qt 6 desktop
   smoke, independently-sandboxed renderer processes, multi-origin isolation,
   native notification/worker policies and sanitizer/memory ownership tests.

### CI evidence rules

The workflow `browser-native-aot-export.yml` builds `oreslang-llvmc` from
its Git checkout, records the compiler binary SHA256, and executes the test.
That CI-computed digest is an **execution fingerprint**, not independent release
attestation of a compiler build. Require run-attempt/runner/step validation.
A zero-step job is **NOT_EXECUTED** even if GitHub reports `failure`.

The source and browser repositories are private. An unrelated organization may
not receive either Git tree until explicit independent source-owner/security
approval binds a specific immutable tree to a particular private destination.
A source-free Actions runner canary does not grant that permission.
