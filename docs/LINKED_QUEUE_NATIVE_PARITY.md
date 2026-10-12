# Native linked-queue parity gate

Status (2026-10-12): native LLVM is **not** equivalent to Java/Truffle for
`IntLinkedQueue`. This is a deliberately fail-closed, exact-source test gate.

Queue source provenance: [oreslang-linked-queue PR #4](https://github.com/ores-truffle-oreslang/oreslang-linked-queue/pull/4),
merged as `2df46251bb959f9e821e073141012ba03863346a`.
The six snapshots under `tests/fixtures/linked-queue/` are copied as identical
Git blobs from that commit. Keep those inputs intact when extending the LLVM
frontend. They use Oreslang class/import/Array/Option/ownership syntax, not
C++ or Java shims.

## Evidence and current limitations

- Exact-source Java compiler/execution: [CI #38316842944](https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38316842944) passed all five positive queue programs, five compiler-rejection fixtures and 18 ownership regressions.
- Native LLVM 18 source compiler build and all six exact-source fail-closed checks:
  [CI #38317849525](https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38317849525).
  A passing result here only establishes rejection without IR. It does **not**
  establish LLVM queue execution, borrow safety or runtime parity.
- Target missing slice: native-owned `Array<int>` and bounded class fields,
  checked `new Array<int>()` / `add` / `get` / `set`, deterministic
  `Option<int>`/queue entry snapshots and write-exclusive `mut self` calls.
  Memory allocation, teardown, overflow and partial initialization must be
  safe on both normal execution and failure paths.
- Retire this rejection gate only together with compiler-generated verified IR,
  successful Clang execution of the same five positive Oreslang sources,
  explicit native rejection of the five ownership negatives, and ASan/UBSan.

Do not replace the Oreslang queue with a C++ surrogate and claim parity.


## Fail-closed audit limitations and review protocol

The native compiler currently rejects the queue at **parser admission**, not
at checked `Array<int>` or class lowering. This distinction matters: it is
not evidence that LLVM enforces queue ownership, nor that it can execute it.
Do not silently weaken these expectations to accept any nonzero exit:

- The fixture Git blob identities are checked before running a compiler.
- A supported scalar Oreslang program must compile to verified LLVM IR,
  successfully link with Clang and exit with the exact native status 42.
- Each queue fixture must return status 1, produce **zero bytes** on stdout,
  and match the pinned per-fixture parser diagnostic.
- The harness's separate adversarial unit tests force generic crashes,
  process misuse, stubbed success, partial/whitespace stdout, corrupted input,
  and unrelated diagnostics to fail.
- On compiler improvements, a changed error location or admission stage
  intentionally turns this regression red; update the diagnostic only after
  identifying and reviewing the new first unsupported construct.

A passing CTest still proves only safe parser rejection and a separate scalar
native execution path. LLVM support requires source compilation and actual
instrumented execution of the queue fixtures before retiring this test.
