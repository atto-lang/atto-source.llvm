# Native LLVM actor-bound f32 host preflight v1 (NOT compiler lowering)

This draft implements a **host-only admission preflight** for the proposed shared `ores-truffle-oreslang/oreslang-common-source` f32 ABI contract, draft PR #256.

- `NativeF32ActorHostAdmissionV1.h` (Git blob `0e256965523ef93432728f0c5cfc2410f6e0b156`) uses the **existing supervisor physical-turn authority**. It checks the scheduler-owned actor execution domain and generation against the runtime context, and calls `ores_execution_owns_running` to reject stolen thread claims, STOPPING actor turns and stale generation/sequence.
- `tests/native_f32_actor_host_admission_v1_test.c` (Git blob `21c7e20a00f08ca45d5c68d399e908ff1a5629f7`) exercises valid host preflight, 32x32 bounds, quota, zero/mismatched axes, wrong/forged actor scope, cross-thread stolen claims, stop, restart and stale physical turn.
- `scripts/test-native-f32-actor-host-admission-v1.sh` compiles and runs this standalone test under GCC/Clang × O1/O2 × ASan/UBSan on the **source organization's own runner**; no network, compiler mirror, imported OresLLM native code or guest source checkout required.

## Local exact Git-blob-verified execution

The following **four** real repository source files were reconstructed from the checked GitHub default branch / this PR and verified byte-identical via `git hash-object`:

| File | Git blob SHA |
| --- | --- |
| `src/main/cpp/oreslang/runtime/NativeExecutionAuthority.h` | `cfd0e7f1d8fbd125a0ca20c7907432fc0407635c` |
| `src/main/cpp/oreslang/runtime/NativeExecutionContext.h` | `6e3689c5d8a909a5a828d37288b365d126fc3bfa` |
| `src/main/cpp/oreslang/runtime/NativeF32ActorHostAdmissionV1.h` | `0e256965523ef93432728f0c5cfc2410f6e0b156` |
| `tests/native_f32_actor_host_admission_v1_test.c` | `21c7e20a00f08ca45d5c68d399e908ff1a5629f7` |

**Executed:** GCC **14.2.0** and Clang **17.0.0** × `-O1/-O2`, C11 `-Wall -Wextra -Werror -Wpedantic -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer`, ASan leak detection and UBSan fail-fast enabled. **4/4 passed**, each emitting:

```text
ORES_F32_NATIVE_HOST_PREFLIGHT_V1_PASS|foreign_thread=DENIED|stopped=DENIED|shape=CHECKED|guest=NOT_EXECUTED
```

The **exact committed standalone shell runner** (Git blob `595f3a8b8fc90006b814fbe4fbdf44c169580baf`) was also reconstructed byte-for-byte, passed `bash -n`, and **executed locally** against the four Git-blob-verified source files. It completed all four GCC/Clang O1/O2 ASan+UBSan matrix configurations with `ORES_F32_NATIVE_ADMISSION_MATRIX_PASS` for each. It has **not** executed in GitHub-hosted CI. Source CI admission has historically failed with zero steps.

## Security / maturity boundaries

**PRECONDITION ONLY**: `ORES_F32_HOST_PREFLIGHT_ONLY` is not a `GEMM_OK` result, a guest authority grant, or native production code execution. The caller still must maintain a pinned turn until the broker returns; this point-in-time authority check is **not** an in-flight reservation. Actual runtime integration must close STOP/cancellation races by pinning a lifetime lease across the full C broker operation, check broker-owned exact shape and generation, atomically account physical output bytes, and rollback unpublished output exactly once.

This PR does **not** register a compiler intrinsic, lower AttoLang `rt borrow` or `rt take`, import or execute `ores-llm` native C code, prove Native Image/LLVM AOT or hard real-time safety, or establish tenant isolation. No full private compiler source was mirrored to another organization. Keep **DRAFT** until real compiler-emitted guest and ownership-negative evidence exists.

## Stage 2: atomic physical-turn pin with STOP/teardown barrier (2026-11-10)

`NativeF32ActorTurnLeaseV1.h` now defines a **host-private** lifetime pin coupled to the existing `OresNativeExecutionAuthority`. Admission rechecks RUNNING physical-thread ownership and sequence **while holding authority mutex then the lease mutex**, and increments the pin before releasing authority lock. This makes STOP's authority-state transition mutually exclusive with final pin acquisition: the prior preflight's point-in-time TOCTOU gap is closed *for callers that use this new pinned API*. Each pin has a monotonic serial, rejecting duplicate/stale/copied and foreign-carrier returns. The supervisor closes admission, waits for active pins to return, and only then may reclaim actor-owned memory. Callers must externally join/fence waiters and retain the authority through lease destruction.

The **exact committed** `scripts/test-native-f32-actor-host-admission-v1.sh` was reconstructed byte-for-byte (blob `775b38f957f61cd6f56ad3c0b9bc60b2eb9d2d91`) and executed locally, linking these exact Git blobs:

- `NativeExecutionAuthority.h`: `cfd0e7f1d8fbd125a0ca20c7907432fc0407635c`
- `NativeExecutionContext.h`: `6e3689c5d8a909a5a828d37288b365d126fc3bfa`
- `NativeF32ActorHostAdmissionV1.h`: `0e256965523ef93432728f0c5cfc2410f6e0b156`
- `NativeF32ActorTurnLeaseV1.h`: `60730da754b9edd1e8486c1f37d346352c8894fe`
- preflight test: `21c7e20a00f08ca45d5c68d399e908ff1a5629f7`
- new STOP/lease race test: `fefc6b67def07c9757d04d1c0ff93d8fdf2f4eb7`

GCC 14.2 + Clang 17 × O1/O2, ASan+UBSan and fail-fast: **all 4 configurations passed both executables**. The adversarial new test proves copied-claim denial, concurrent STOP before physical return, no early quiescence, post-STOP and stale-after-restart rejection, double pin return, invalid shapes/quota, and 1,024 serial-fenced admission/release cycles.

**Remaining integration obligation:** source compiler/ActorRuntime must mint this lease from actual scheduled turns and ensure native broker `begin/commit/finish` callbacks use the same live authority/lifetime semantics. The external OresLLM CPU production C adapter lives in [CPU PR #30](https://github.com/ores-llm/ores-llm-ml-kernels-cpu.atto/pull/30); it is **not** linked into the LLVM compiler or source guest here. LLVM AOT/Graal Native Image execution and ownership compile failures remain NOT_EXECUTED.
