# AttoLang test parity: LLVM native backend

Status: **implementation staged; hosted exact-head evidence pending**.
Do not infer full `atto test --fuzz` or `atto test --race`
implementation from these gates. The compiler/runtime owns source semantics;
there is no duplicate parser or Python source-mutator.

## Capability and evidence

| Capability | LLVM implementation | Evidence class |
| --- | --- | --- |
| Canonical `.atto` and legacy `.ores` | Native import resolver accepts both and rejects ambiguous extensionless twins | Import/source admission |
| `.atto` to LLVM IR, native link, and exit | `atto_native_aot_guest_exit_42` and seeded source mutation | Compiler-backed native guest **only after execution** |
| Deterministic source mutation | Rust `atto-source-fuzz`, seed 1729, 8 cases, O0/O2 and negative oracle | Compiler-backed **source** fuzz, not stdin fuzz |
| `atto test --fuzz` black-box stdin corpus | Rust test CLI `[fuzz].command` | **Not** wired to LLVM source compilation; scalar guest does not demonstrate stdin fuzz |
| Native ASan+UBSan and TSan | Instrumented C/C++ actor suites and deliberate detector controls | Host/native runtime, not all compiled guest memory paths |
| `atto test --race --trace` | Strict bounded Rust offline JSONL analysis | Offline trace, not an authenticated live runtime record |
| Bare `atto test --race` | Fail-closed exit 2 | Live trusted instrumentation **not implemented** |
| Full `.atto` actor scheduler/access coverage | No complete host-owned attested event channel yet | NOT_EXECUTED |
| Compiler-backed guest `ORES_TEST|SUMMARY` | Not established | Scalar OS exit is not an authenticated test receipt |

## Reproducible source-local commands

```sh
cargo test --manifest-path tests/rust-harness/Cargo.toml --locked
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc oreslang-rust-test-harness --parallel 2
ctest --test-dir build -N -R '^atto_(llvm|native)_'
ctest --test-dir build --output-on-failure -R '^atto_(llvm|native)_'
bash scripts/test-verify-native-sanitizer-controls.sh
bash scripts/verify-native-sanitizer-controls.sh address undefined thread
```

The dedicated `atto-native-test-parity.yml` workflow pins the exact PR
head and Git tree, checks an inventory of **four** tests, and executes:
a Rust host-only oracle self-test; a real compiler-backed source-mutator;
a linked canonical `.atto` guest returning 42; and invalid source
rejection without partial LLVM IR. Zero-step jobs are not test evidence.

The source-mutator generates `.atto` programs in a temporary directory
and computes independent addition oracles in the OS exit domain [0,192].
It emits LLVM IR using `oreslang-llvmc --target=host`, links and executes
at O0 and O2 without the ambient environment, and checks a legacy
`.ores` counterpart. It additionally checks unknown calls, invalid
syntax, deliberately wrong native results, and replayable seeds.
This tests only the current **scalar native AOT subset**; it does not
fuzz stdin, channels, async continuations, actor turns or shared heaps.

## Sanitizer/race fail-closed contract

- Compile **and** link the instrumented native executable; ASan+UBSan
  and TSan are separate executables and CI lanes.
- A known native memory overflow, signed overflow, or race must
  trigger its corresponding sanitizer diagnostic. Stripping the
  instrumentation flags must fail detector admission.
- An unsupported TSan mapping/runtime, missing diagnostics, missing
  exact-head job steps, timeout, or incomplete recorder is
  **inconclusive**, not a successful race test.
- A guest-written `ORES_RACE_V1` trace is analyzable offline but is not
  trusted coverage attestation. A complete live recorder requires
  host-only transport, access/synchronization coverage, incarnation
  and generation fencing, dropped-event accounting, and an authenticated
  terminal receipt.
- Managed JVM/Graal, CLR and Wasm guest memory is not automatically
  covered by LLVM host sanitizer flags. Native actor host tests and
  compiled `.atto` guest tests must remain distinct evidence lanes.

## Release gate

Require a stepful exact-head authorized run of Cargo/CMake/CTest plus
real native ASan+UBSan and TSan jobs, clean optimized execution,
independent review, and a trustworthy guest receipt before promotion.
Keep bare `atto test --race` exit 2 until compiler/runtime host-owned
memory and scheduling event coverage is authenticated. No private
source export is authorized merely by a funded Actions carrier.
