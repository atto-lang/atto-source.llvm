# AttoLang JS ⇄ LLVM scalar parity gate (M0)

This draft adds two independently labeled native-execution matrices for **the current native LLVM compiler**. It is intentionally not a claim of full Java/Graal, LLVM, Wasm, Cranelift or .NET parity.

## Canonical source identities

| Scope | Corpus path | Git blob SHA-1 | SHA-256 |
| --- | --- | --- | --- |
| Shared scalar50 | `contracts/atto-parity-v1/shared-scalar50-v1.tsv` | `a948dbaf594916a6ee07f65d49a0cf5ae398ca7b` | `210dbe7363e5388e4f82c4de9fe7f8136ee6a7dca698ce72165e006fd5ecb0db` |
| JS checked-i64 50 | `contracts/atto-parity-v1/js-checked-i64-candidates.json` | `9f3e54586a71ae4e5525e308b26c61ea70bc7232` | `5fd8ed522dfaaead8dc1b2d780159a4f156482124b7515d0a788245fc594ebc8` |

The first corpus is byte-identical to the JavaScript `shared-scalar50-v1.tsv` fixture, which itself pins the Java shared source corpus.
Both native matrices now recheck Git HEAD/tree and SHA-256 identities of the source
corpus, the compiler binary and Clang after every guest finishes, **before**
writing an executable-evidence receipt. The checked-i64 native matrix additionally
executes two compiler-backed source-sensitivity controls: `40 eq 41` must exit
43, and `40 eq 40` must exit 42; a source-insensitive constant-42 compiler
cannot pass. These are additional guards, not proof of sandbox isolation. The second is byte-identical to the checked-i64 candidate manifest in [JS PR #111](https://github.com/ores-truffle-oreslang/oreslang-source.js/pull/111). These are test fixture bytes, not a compiler/runtime source mirror; no cross-organization transfers were made.

## M0 — 50 ordinary scalar source programs

`atto_shared_scalar50_native.cmake` first rejects any changed manifest hash, wrong ID order, invalid source profile, missing case or unrepresentable process exit. Its *native* mode then:

1. Writes each exact source field to `*.atto` without adding an extra newline.
2. Runs the actual built `oreslang-llvmc` to produce bounded LLVM IR.
3. Uses Clang to link that IR into a native executable.
4. Executes all 50 native binaries, requiring the exact expected process exit and empty stdout/stderr.
5. Writes a successful receipt only after all 50 native executions; source HEAD/tree and compiler/Clang SHA-256 are captured.

The separate *host-only* validation test performs no compiler/guest execution and **must not be promoted as parity proof**.

## M1 — 37 full-width signed-i64 values and 13 checked overflow traps

`atto_checked_i64_native.cmake` pins and validates the JS checked-i64 corpus.

For each of the 37 expected values, it retains the original expression bytes but wraps the expression in AttoLang `if ((expr) eq (expected)) { return 42; } fi`. This lets the compiled LLVM guest compare **the full signed-i64 value**, without losing information by truncating a native process exit status to eight bits. The resulting native source differs from the JS program and is separately hashed; the original JS source is also hashed.

For 13 expected overflows, the **complete original source program bytes** are compiled unchanged as `.atto`. The IR must contain checked-overflow and `llvm.trap` instructions. The guest must terminate with a recognized native trap signal, rather than an arbitrary nonzero exit. No source-free oracle, host-only trap simulation, compiler diagnostic, skip or timeout is counted as an executing native guest result.

This v1 gate supports Linux x86-64 / equivalent Unix trap reporting only; other platforms need reviewed trap normalization. It does not cover every statement, generic instantiation, actor or scheduler semantic.

## Running the gates

On an authorized source checkout with LLVM libraries, Clang and CMake installed:

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target oreslang-llvmc --parallel 2
ctest --test-dir build --output-on-failure -R '^atto_(shared_scalar50|checked_i64|i64_original)_'
```

Run the dedicated exact-head workflow `.github/workflows/atto-scalar50-native.yml` for PR-head checkout, compiler and Clang identity, and both JSON receipts.

## Evidence classification and release blockers

- Locally, the scalar50 manifest is verified by SHA-256 and a host-only CMake validation. Tampering and a missing compiler were correctly rejected. A fake-compiler/Clang experiment exercised the harness but **is not LLVM execution evidence**.
- The LLVM source organization currently fails GitHub Actions jobs **before a runner executes any steps** (see [runner issue #159](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/159)). Do not interpret a zero-step failure as a compiler failure or a parity pass.
- LLVM PR #1363 must remain draft pending a stepful exact-head compiler/guest run, per-case receipts, a review of any actual failures, and independent approval.
- Full Java/LLVM compiler, ownership, actor, structured-concurrency and backend parity remains out of scope for these scalar gates.

The chosen fixed test source programs avoid the legacy remainder `%` operator; `.ores` remains supported for compatibility while `.atto` is used for these new native guest files.

## Local host-only verification, 2026-11-03

An independently reconstructed 3,040-byte scalar50 canonical TSV produced
SHA-256 `210dbe7363e5388e4f82c4de9fe7f8136ee6a7dca698ce72165e006fd5ecb0db`.
CMake v3.31.6 validated all 50 manifest rows, and the CMake extraction
round-tripped all 50 source fields unchanged into `.atto` files. A single
expected-result mutation made the pinned digest fail with exit code 1.
A synthetic (not canonical) 50-case JSON fixture passed the checked-i64
manifest validation logic with 37 value and 13 overflow classifications;
this does not establish that the exact checked-i64 JSON was executed in CMake.
Local Clang 17 and CMake verified the signal status `Illegal instruction`
for a synthetic C `__builtin_trap()` process and checked both expected
true/false control statuses using synthetic C, **not** Oreslang code.

The local environment has no LLVM development CMake configuration and lacks
the private native compiler checkout, so no actual `oreslang-llvmc` build or
native AttoLang guest execution took place locally. Source-org Actions
continue to fail with zero executed steps; do not mark guest parity passed.

## Receipt laundering protections

Both native test drivers remove their designated prior success receipt before
compiler work begins. The source-org CI job also removes the two well-known
receipt paths immediately before CTest, uploads artifacts only after the
complete CTest and independent JSON audit succeed, and treats missing reports
as an error. A failed or skipped native job **cannot** claim parity by uploading
an old receipt from a reused workspace. The scripts reject report destinations
outside their dedicated workdir. The source, corpus, compiler and Clang
identities are re-read after all executions and before any new receipt is
written.

A local CMake 3.31.6 control confirmed the declared report path comparison
rejects off-workdir paths, and a synthetic stale receipt was removed by the
same CMake file removal operation. These are local **harness checks only**.

Local Clang 17 also executed two hand-authored LLVM IR programs: signed-i64
minimum plus zero returned 42 after full-width equality, while maximum plus
one through `llvm.sadd.with.overflow.i64` triggered an `Illegal instruction`
trap. This validates the host LLVM IR / Clang behavior **without** exercising
`oreslang-llvmc`; do not cite it as compiled AttoLang source parity.

## Dual optimization execution and receipt comparison

The source-local LLVM CTest harness now compiles the same compiler-emitted IR
with both Clang `-O0` and `-O2` and executes both binaries:

| Native matrix | Source cases | Guest processes |
| --- | ---: | ---: |
| Canonical byte-identical scalar50 | 50 | 100 |
| Checked-i64 values and overflows | 50 | 100 |
| Equality true/false controls | 2 | 4 |
| Unchanged original-source full-width i64 values | 37 | 74 |
| **Total executions** | **139 case/mode groups** | **278** |

An LLVM optimizer-dependent value difference, missing or non-signal overflow,
missing O2 guest result or a dishonest process-count receipt fails the native
gate. The workflow independently binds the two receipts to the source HEAD
and Git tree, checks the built compiler SHA-256, recomputes all 100 canonical source
hashes and the 37 generated equality-wrapper hashes, then separately verifies
37 **unchanged original-source** programs through their exported native i64 ABI.

Run synthetic offline JS/LLVM comparison regressions with:

`node --test tests/atto_js_llvm_receipt_comparison.test.mjs`

When two **separately acquired** exact-head JS and LLVM receipts exist, compare
them without transferring private compiler source or using cross-org tokens:

`node scripts/compare-atto-js-llvm-receipts.mjs /path/to/js-receipt.json /path/to/llvm-checked-i64-native-receipt.json`

The comparator validates both manifests, 50 per-case IDs, expected/observed
semantics, signed-i64 value wrappers, O0/O2 trap classifications, source SHA-256
and compiler-source receipt identities. Its output is deliberately
`RECEIPT_CONTENT_MATCH_ONLY`, with both `execution_attested:false` and
`cross_backend_parity_verified:false`: two plausible JSON blobs are **not**
independent runner attestation, even if their 50 case contents match.

An isolated V8 test adapter ran all 14 authored synthetic comparator cases
(1 admissible shape and 13 mutations): **14/14** passed. This was neither a
Node 22 CI job nor LLVM-native guest execution. A local CMake 3.31.6 check of
the **actual pinned** checked-i64 corpus (SHA-256
`5fd8ed522dfaaead8dc1b2d780159a4f156482124b7515d0a788245fc594ebc8`)
accepted 37 value cases and 13 overflow expectations. Hand-authored LLVM IR
probes linked and ran with Clang 17 at both O0/O2: `INT64_MIN+0` returned 42
and `INT64_MAX+1` trapped. These are host/toolchain probes only.

The source-org GitHub Actions runner still fails at zero steps. Until an
authorized source runner compiles and executes all **278** guest processes,
actual LLVM parity is **not verified**. A successful comparison report
must only be promoted after independently authenticating both workflows'
nonzero executed steps and exact input head/tree/compiler receipts.


## M2 — execute unchanged full-width i64 AttoLang source directly

The compiler source establishes this ABI: an exported
`i64 @__oreslang_guest_main()` contains the native guest computation, while
a separate `i32 @main()` adapts its result to the OS process-exit contract.
The OS exit value cannot represent every signed-i64 result.

The new `atto_i64_original_native_guest` CTest compiles the **complete,
unmodified** JS/AttoLang source for each of the 37 successful full-width cases.
It emits LLVM IR, compiles that IR to ELF object code with Clang at O0 and O2,
uses `objcopy --redefine-sym main=atto_original_exit_wrapper` on the object
to avoid a duplicate C `main`, and links against the fixed
`tests/atto_i64_original_guest_probe.c`. The oracle calls the actual exported
guest i64 function and compares the full signed result to a strictly parsed
decimal CLI expectation. The AttoLang source is **never rewritten**.

Each case must exit 42 at both optimization levels; wrong results return 43.
Missing guest symbols, invalid expectations, linker failures, signals, timeouts,
guest stdout/stderr, and changed compiler inputs all fail the native gate.
A third native receipt records 37 original-source cases, **74 executions**,
per-case source/IR hashes, toolchain hashes, HEAD/tree and O0/O2 results.
The 13 checked-overflow programs remain in the direct native trap matrix.

This gate is currently **Linux ELF only** and depends on source-local
`oreslang-llvmc`, Clang and GNU/LLVM objcopy. A local *synthetic*,
hand-authored LLVM IR object linked with objcopy and the C oracle: `INT64_MIN`
matched the decimal expectation at O0/O2 (exit 42), and the deliberately
wrong `INT64_MAX` expectation returned 43. That probes the calling method,
**not** AttoLang compilation. All 278 compiler-generated native executions
remain unverified until GitHub Actions runs real steps.

All three LLVM receipts must be independently reconciled to one source
HEAD/tree and matching compiler binary hashes. No stale or synthetic receipt
is sufficient to claim verified backend parity.
