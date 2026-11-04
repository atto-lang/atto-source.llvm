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
| `atto test --fuzz` black-box stdin corpus | Native LLVM `fuzz_byte(i64)` + bounded C stdin adapter, configured through merged Rust CLI `[fuzz].command` | **Compiler-backed guest call via host transport**; no AttoLang stdin API or universal corpus coverage |
| Native ASan+UBSan and TSan | Instrumented C/C++ actor suites and deliberate detector controls | Host/native runtime, not all compiled guest memory paths |
| `atto test --race --trace` | Strict bounded Rust offline JSONL analysis | Offline trace, not an authenticated live runtime record |
| Bare `atto test --race` | Fail-closed exit 2 | Live trusted instrumentation **not implemented** |
| Full `.atto` actor scheduler/access coverage | No complete host-owned attested event channel yet | NOT_EXECUTED |
| Compiler-backed guest `ORES_TEST|SUMMARY` | Not established | Scalar OS exit is not an authenticated test receipt |

## Reproducible source-local commands

```sh
cargo test --manifest-path tests/rust-harness/Cargo.toml --locked
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc oreslang-rust-test-harness oreslang-no-gc-ir-audit-build --parallel 2
ctest --test-dir build -N -R '^atto_(llvm|native)_'
ctest --test-dir build --output-on-failure -R '^atto_(llvm|native)_'
bash scripts/test-verify-native-sanitizer-controls.sh
bash scripts/verify-native-sanitizer-controls.sh address undefined thread
```

The dedicated `atto-native-test-parity.yml` workflow pins the exact PR
head and Git tree, checks an inventory of **six** Linux tests, and executes:
a Rust host-only oracle self-test; a real compiler-backed source-mutator;
a linked canonical `.atto` guest returning 42; and invalid source
rejection without partial LLVM IR; and a real compiler-generated `.atto` actor handler linked against the native actor-owned heap with ASan+UBSan; and a byte-level stdin adapter feeding the compiled `.atto` function with a deliberate guest mutant. Zero-step jobs are not test evidence.

The source-mutator generates `.atto` programs in a temporary directory
and computes independent addition oracles in the OS exit domain [0,192].
It emits LLVM IR using `oreslang-llvmc --target=host`, links and executes
at O0 and O2 without the ambient environment, and checks a legacy
`.ores` counterpart. It additionally checks unknown calls, invalid
syntax, deliberately wrong native results, and replayable seeds.
The independent Rust source-mutator tests the scalar native AOT subset. A **separate** native C stdin adapter now forwards bounded bytes into an LLVM-compiled `.atto` `fuzz_byte` function, checks the guest result against an independent property oracle, and rejects a deliberately wrong compiled mutant. The example `examples/atto-native-fuzz-project/atto-test.toml` points the merged Rust `atto test --fuzz` CLI at that built executable. It still does not establish direct `.atto` stdin I/O, coverage-guided fuzzing, async continuations, or live actor race detection.

## Rust CLI and LLVM native stdin integration

The LLVM native adapter uses a narrow scalar ABI:
`fuzz_byte(i64) -> i64`, invoked for each bounded byte from native host stdin.
It checks a simple independent `byte+1` property and deliberately mutated
`byte+2` guest source. A valid `.atto` guest must compile to verified LLVM
IR, pass the Rust no-GC IR auditor, link to the C adapter and execute.
This still does not implement source-level AttoLang stdin I/O.

The real Rust `atto test --fuzz` engine is already merged, but its
opt-in `[fuzz].inconclusive_exits = [2]` semantics require
[the companion test-CLI PR #26](https://github.com/ores-truffle-oreslang/oreslang-test.cli/pull/26).
The native adapter reserves exit 2 for input-capacity and I/O failure;
it reserves exit 77 for an observed scalar-property discrepancy. The
explicit exit policy prevents an adapter admission failure from being
published as a reproducible guest defect. Known sanitizer diagnostics
override the exit policy. Both source PRs require independent review.

The *manual cross-repository acceptance gate* is
`scripts/test-atto-cli-llvm-fuzz-e2e.sh` with a real
`oreslang-llvmc`, `clang`, `oreslang-no-gc-ir-audit` and
installed `atto-test`. It demands **1,000** normal compiled-guest
fuzz cases, a deliberately wrong compiled guest, Rust replay/shrinking,
a one-byte reproducer, and direct native replay of the same discrepancy.
It fails closed on a missing compiler or CLI: neither stubs nor synthetic
compiler receipts are acceptable.

The dedicated workflow separately executes
`scripts/test-native-atto-fuzz-adapter-source-free.sh`, which compiles
a simple C function to LLVM IR and links it against the exact C stdin
adapter. Its six controls were independently exercised locally under
Clang 17, including an ASan+UBSan linked build. This is **host/source-free
adapter evidence**, not a compiler-backed `.atto` guest proof and
cannot replace the six compiled CTest gates.

## Native fuzz builder hardening and evidence boundaries

The native `scripts/build-native-atto-fuzz-target.sh` now enforces a
**separate wall-clock limit** on each compiler, Rust no-GC IR auditor, and
Clang invocation. `ATTO_FUZZ_BUILD_TIMEOUT_SECONDS` must be a canonical
decimal integer **1–120** (default **60**); invalid inputs are rejected
before any compiler invocation. GNU `timeout` is mandatory (`gtimeout`
on systems where coreutils installs it under that name). Missing timeout
support is **inconclusive/fail-closed**, not permission to run unbounded.
A failed or timed-out step cannot replace the previously published
executable.

LLVM IR stdout is capped **while streaming** at 2,000,001 bytes, before
further output can consume unbounded local disk. The builder captures
Bash `PIPESTATUS` for both the compiler and stream limiter; a compiler
SIGPIPE/timeout/nonzero exit is never treated as success, and a payload
above the 2,000,000-byte accepted ceiling fails with an explicit size
diagnostic. The source-free canary checks that this overflow cannot
replace the previously published binary.

The workflow's separate **source-free** builder canary,
`scripts/test-native-atto-fuzz-build-gates-source-free.sh`,
uses *synthetic C-generated LLVM IR* to prove these admission paths,
without claiming any AttoLang parser/compiler execution:

- positive IR/native scalar oracle; two valid-IR but wrong host ABI
  (wrong-arity/void-return) negatives;
- an IR verifier failure, mandatory auditor denial (exit 7), IR stream
  overrun, and **11 timeout mutations** (eight invalid budget strings
  and stalled compiler/auditor/linker);
- identity-preserving binary publication across every negative, with an
  independent outer watchdog and a marker proving bad timeout strings
  did not even launch the synthetic compiler.

The exact committed builder blob
`ab3eb47b80469780eea5774c545dca68fd2949e3` and source-free
builder-canary blob
`b16e3c648a62f926dc06bf134e8c14a377724be7`
were reconstructed and hash-matched against the GitHub copies; the
canary **passed locally with Clang 17**. This is a local source-free
build-gate result, **not** hosted Actions or compiler-backed guest
parity. The compiled `.atto` source-negative harness also now refuses
signal-terminated compiler crashes as valid syntax-rejection evidence.
The Rust harness creates Unix scratch directories with mode `0700`,
protecting temporary generated IR from permissive process umasks.

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
