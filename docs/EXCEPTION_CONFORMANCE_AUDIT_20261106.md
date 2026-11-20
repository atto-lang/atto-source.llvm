# Exception conformance audit and legacy scalar throw denial — 2026-11-06

This audit separates compiler source, host-component execution, and executable guest evidence. AttoLang was formerly Oreslang; historical repository and executable names are retained.

## Concrete source fix

LLVM main `16e986c4c7c9613d281958107f8584b1468d3d7d` admitted a terminal `try { throw 43; } catch (err) { return err; } finally {}`. The primitive i64 payload violates the Error-only exceptional operand contract, even though caught locally.

The parser now rejects the legacy throw/defer-leading exceptional scope before constructing its exceptional AST/IR. Existing nonthrowing return/try/finally support is preserved. Internal caught-throw AST/codegen structures remain dormant for a future typed implementation; this change does not repurpose their ABI. Twelve existing cases now require the Error-specific compiler denial, including the two formerly positive scalar fixtures; three new .atto cases cover scalar, string and null. These compiler-negative gates require exit 1, the intended diagnostic and empty stdout via tests/contract.py, and no longer depend on finding Clang.

The restriction is implemented in the compiler because guest code cannot establish its own nominal typing or authorize IR emission. A future canonical std/error resolver and typed exceptional CFG can replace this rejection only after actual source execution proof.

## Exact-input executed evidence

Locally fetched source files were checked against Git blob SHA-1 (including Git blob header), then compiled and executed with GCC/G++ 13.3.0 on Linux. This was a partial source checkout, not full-repository CI.

| Suite | Exact input commit | Release | ASan + UBSan |
| --- | --- | --- | --- |
| native_exception_policy_test.cpp + ExceptionPolicy.hpp | LLVM main 16e986c4c7c9613d281958107f8584b1468d3d7d | exit 0 | exit 0 |
| native_supervisor_panic_test.c + NativeActorScheduler and headers | LLVM main 16e986c4c7c9613d281958107f8584b1468d3d7d | exit 0, NATIVE_SUPERVISOR_PANIC_PASS typed=1 finalized=1 exclusive_parent=1 | same marker, exit 0 |
| native_nominal_error_admission_test.cpp + NominalErrorAdmission.hpp | LLVM draft #1404, 215cddd839cd08b37a4f225a8a70cb4603963d90 | exit 0 | exit 0 |

The policy suites are host tests. The CONTROL test simulates completed cleanup and explicitly calls publish_failure; it does not compile throw/raise/panic, prove automatic failure escalation, or attest a parent identity embedded in a guest notice. Nominal admission consumes compiler proof objects, not real resolved guest Error expressions.

Release flags: C11/C++17, -O2, -Wall -Wextra -Wpedantic -Werror; CONTROL additionally -pthread. Sanitizer flags: -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer, same warnings. Sanitized executions used ASAN_OPTIONS=detect_leaks=0: the initial leak-enabled run could not operate under this environment's tracing. No leak-check pass is claimed.

Verified input blobs:
- `tests/native_supervisor_panic_test.c`: `36eb8463882941dc295b1ea42cec4700664ba9e3`
- `src/main/cpp/oreslang/runtime/NativeActorScheduler.c`: `76f8160873c6af0df0dbb13741183600c16550bf`
- `src/main/cpp/oreslang/runtime/NativeActorScheduler.h`: `9cf2a76ac30fd0d9b8620facfb1b887475ee1e90`
- `src/main/cpp/oreslang/runtime/NativeExecutionAuthority.h`: `3aede9229c490888f5b3a986f1a6081dc2ac4243`
- `src/main/cpp/oreslang/runtime/NativeExecutionContext.h`: `6e3689c5d8a909a5a828d37288b365d126fc3bfa`
- `src/main/cpp/oreslang/runtime/NativePhysicalTurnGate.h`: `bc7d4bd89dde58474e453931e29b0043b567d714`
- `src/main/cpp/oreslang/runtime/ExceptionPolicy.hpp`: `130773625d81e03a462b10c2ab4460bde58305a6`
- `tests/native_exception_policy_test.cpp`: `0d11db0041ea5a059ade23927194b3e6b9d765b0`
- `src/main/cpp/oreslang/types/NominalErrorAdmission.hpp`: `b27d2172833d900f0aa10d40c0ff8b9435026142`
- `tests/native_nominal_error_admission_test.cpp`: `79c229088154628024fc375482a59561c3c0589c`

## Five-backend review

| Backend | Pinned source or draft | Observed boundary / unresolved gap |
| --- | --- | --- |
| Java/GraalVM | main 67ff8ad05b710229029c8becde896dde8a5dd726; #1316 c471222d2c5767af923861c132f70dd4b029f121; #2423 31305fdd92eaf1c72306f2b57658aebf215710f3 | Main Parser rejects source throw/raise/panic/recover. Draft #1316 checks only non-void payloads and locally recovers panic, conflicting with Error-only and panic CONTROL requirements. #2423 remains draft supervisor admission, not production guest supervision. |
| LLVM | main 16e986c4c7c9613d281958107f8584b1468d3d7d; #1404 215cddd839cd08b37a4f225a8a70cb4603963d90; #1206 | Main scalar caught-throw admission conflict addressed here. #1404 is a host nominal proof policy with source effects rejected; #1206 has a narrow numeric ErrorSlot throw/catch/panic-promotion design, not general recover/actor/async unwinding. |
| Wasm | main db67e9802467bec3750c6f38550e945b8a524154; #154 1a24294478158e9f82e31216c9d59e39d0f6a60e; #195 | Pooled actors do not establish typed DATA/CONTROL or source exception routing. #195 depends on pinned LLVM numeric lowering. Worker traps/termination are not guest Error identity. |
| Forge | main 3f351125ab8ab84d23b70bb84fd297f22dfccf51; #154 bb9b354261e8e964dbf31a362e29ed8b48f3ea0d; #146 | Bounded numeric ErrorSlot draft consumes throw and promotes unhandled numeric failure to panic. No general recover, arbitrary Error object, async/actor propagation. Host finalizer receipts are not production CONTROL delivery. Forge is independent of Cranelift. |
| .NET | main 51665b058b46e060759ce1ed804ebb72b6a5d97b; #192 | Main documents a scalar NativeAOT subset. No verified general guest exceptional operands, unwind/recover or actor supervisor CONTROL. Historical Task/Future AOT evidence is unrelated to exception conformance and cannot certify current main. |

Issue context: LLVM #143 (typed effects/unwind), Java #476 (supervision) and #963 (runner admission), Wasm #152, Forge #155, .NET #245 and #116. This was a focused keyword/recent-PR review, not an exhaustive backlog census.

## Exact-head CI checked

All listed jobs were fetched directly and had `steps=null`; none is an executed-test failure or pass:

- Java supervisor #2423 head: [41710334054](https://github.com/ores-truffle-oreslang/oreslang-source.java/actions/runs/41710334054), job 126176422915.
- LLVM Error admission #1404 head: [41776294806](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/41776294806), job 126397335318.
- Wasm actor #154 head: [40932244445](https://github.com/ores-truffle-oreslang/oreslang-source.wasm/actions/runs/40932244445), job 123653415317.
- Forge numeric #154 head: [41159153954](https://github.com/ores-truffle-oreslang/oreslang-source.forge/actions/runs/41159153954), all five returned jobs.
- .NET main: [Linux 42179701113](https://github.com/ores-truffle-oreslang/oreslang-source.net/actions/runs/42179701113), job 127766189974; [Windows 42179701088](https://github.com/ores-truffle-oreslang/oreslang-source.net/actions/runs/42179701088), job 127766189814. Additional self-hosted facts job was skipped.

No five-backend exact-commit guest exception pass is established. No private source was mirrored across organizations.

## Smallest read-only acceptance checks

1. Pin source/compiler/fixture SHA and run scalar return-43 positive controls through each real compiler so missing tools cannot masquerade as rejection.
2. Compile throw/raise/panic with int, string, null, fake Error, structural/Any/nullable and borrowed payloads. Require exact intended diagnostic, exit 1 and no emitted IR/CIL/Wasm. Supported canonical Error/root/descendant cases need separate positive proof; unsupported backends report UNSUPPORTED, never PASS.
3. On an admitted guest path, run one trace fixture: matching throw catch, unmatched throw -> raise, raise bypasses catch -> direct deferred recover, unrecovered raise -> panic, LIFO defer/finally once. Panic and cancellation must bypass catch/trap/recover.
4. One actor panic with its DATA queue full: cleanup once, exactly one immutable supervisor CONTROL notice with actor/parent/kind and incarnation fencing; no child-carrier inline callback, no actor-local panic interception. Repeat once across await; record unsupported lowering honestly.
5. Inspect raw logs/test reports for nonzero executed steps and actual test names/results. Retain source tree, binary/fixture hashes, event trace and intended diagnostics; do not reuse a stale head or source-free model receipt.

The new parser/CMake/15 compiler-negative gates still require a real exact-head LLVM build and CTest. LLVM development libraries and a full source compiler were unavailable locally. The six successful host-suite/profile runs above do not validate this new parser patch. Keep this PR draft until those source gates and independent review pass.
