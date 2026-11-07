# AttoLang GPU SAXPY — next acceptance gates and expanded test matrix

**Status: source-level tests authored. Hosted execution and physical hardware
proof remain NOT_EXECUTED until exact-head jobs have nonzero executed steps.**

This matrix belongs to stacked LLVM draft PR #1487 (three-buffer physical
adapter) on top of draft PR #1466 (closed `.atto` source-to-OpenCL compiler).
Test source is **not** device evidence. The generic `gpu task`/CUDA-class
feature set remains unimplemented and cannot be claimed from SAXPY results.

## P0: runner admission and real native compilation

Run in the **trusted original source repository** at exact immutable PR head,
not a synthetic PR merge SHA. In a real compiler-equipped worker:

```sh
head_sha="$(git rev-parse HEAD)"
tree_sha="$(git rev-parse HEAD^{tree})"
test -z "$(git status --porcelain --untracked-files=no)"
cmake -S . -B /tmp/atto-gpu-saxpy-build -DBUILD_TESTING=ON
cmake --build /tmp/atto-gpu-saxpy-build --parallel 2 --target \
  oreslang-llvmc oreslang-atto-gpu-saxpy-source-v1-test \
  oreslang-opencl-saxpy-provider-v1-test oreslang-opencl-saxpy-gpu-probe
ctest --test-dir /tmp/atto-gpu-saxpy-build --output-on-failure \
  -R '^atto_gpu_saxpy_'
node --test tests/atto_gpu_hardware_receipt_gate_v1.test.mjs
echo "$head_sha $tree_sha"
```

Check actual C++17 -Wall -Wextra -Wpedantic -Werror compilation and CTest
assertions, then run the dedicated ASan/UBSan host-only workflow at this same
source head. Reject empty `steps=[]`/`null` evidence. If the trusted
source-org runner is blocked, **do not publish test-passing status**.

## Expanded deterministic host/source test coverage

| Suite | New checks | Proof class |
| --- | --- | --- |
| `atto_gpu_saxpy_source_v1` | 34+ independent token/operator/type/effect/source-shape mutations; raw NUL/trailing declarations; comment-only semantic invariance; exhaustive small 3D grid product/limit sweep (576 configurations, 546 admitted in the controlled model) | Native source parser/launch reference, **not GPU** |
| `atto_gpu_saxpy_provider_host_v1` | Each of the three physical allocation failures and fresh generation after recovery; program preparation/release failures; all X/Y upload, kernel and download event-null/throw fault points; driver poll exceptions and failed event statuses | Fake native driver, **not GPU** |
| `atto_gpu_saxpy_provider_host_v1` | Four cancellation stages; pending event retains quota, exact event count 1/2/3/4, forbids output after cancellation; false quiescence witness rejects | Fake native driver, **not GPU** |
| `atto_gpu_saxpy_provider_host_v1` | 16 eight-way concurrent queue-admission rounds (only one winner); 16 concurrent cancel/poll rounds; cross-provider stale/foreign operation rejection | Native contention on host fake driver, **not GPU** |
| `atto_gpu_saxpy_provider_host_v1` | Nine f32 vector-length cases up to 1,024 elements, byte/element parity, three physical buffer releases, quotas and generations, immutable artifact-field mutations | Host fake-device numeric oracle, **not GPU** |
| `atto_gpu_hardware_receipt_gate_v1.test.mjs` | Ten early rejection controls for fake compiler/probe arguments, missing/mismatched PR SHA, invalid hosted attempt, foreign repository identity, missing CMake compiler/fixture | Source-free host evidence integrity, **not GPU** |
| `atto_gpu_saxpy_physical_opencl_v1` | Real `oreslang-llvmc` emitted source passed as artifact bytes to the native driver; physical OpenCL events; f32 bitwise oracle; no-compatible-GPU explicitly SKIPPED | Only this lane can establish hardware evidence when actually executed on verified GPU |

All counts are **test cases/checks authored**, not passing assertions or
executed jobs. The 576-grid reference sweep includes both expected accepts
and rejects and does not authorize target GPU hardware geometry by itself.

## P1: hardening required before production admission

1. Replace fail-closed process termination on active provider destruction
   with supervisor-owned operation lifetime, durable quarantine, async
   shutdown and matching generation/tenant fences.
2. Introduce shared, independently authenticated tenant/device resident,
   staging and event quotas. Per-provider quotas are not isolation.
3. Tie X/Y/output to actual language `rt borrow`/`rt take` owner admission
   and atomic physical triple-buffer claim/terminal transitions, with stale
   generation, cross-tenant, and actor-restart negatives.
4. Make the OpenCL provider worker pool and device event notifications
   nonblocking for AttoLang actor carriers; wire `gpu task` to `Future<T>`
   with exactly-once completion/cancellation ownership.
5. Run a real authenticated hardware probe and negative mutations on an
   inventoried GPU worker; record immutable source head/tree, executed job
   steps, compiler/toolchain/device IDs, event chain, result oracle,
   run-attempt and independent review. A self-printed device name or hashed
   source file is not cryptographic device attestation.

## P2: CUDA-class capability only after the P1 invariants

Implement first-class GPU MIR; device-safe generics and function effects;
typed address spaces (global, shared/local, constant); 1/2/3D launches;
barriers/atomics/subgroups; multi-queue streams/events; async copies and
event graphs. Add deadlock/divergent barrier negatives, bounds mutations,
aliasing races, queue/backpressure tests and parity across NVIDIA/AMD/Intel
where independently executable.

**Review rule:** keep #1466 and #1487 drafts until verified source-org
compiler runs, a meaningful independent security review, and required
physical-device evidence exist. Source-only, host-fake, and hardware results
are separate evidence classes.
