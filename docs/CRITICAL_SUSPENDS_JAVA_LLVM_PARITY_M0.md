# AttoLang critical/suspends — Java ⇄ LLVM M0 parity gate

> Status: **SOURCE-LOCAL CONTRACTS AUTHOR ONLY**. No compiler-backed Java or
> LLVM `critical` guest parity is established by this document or its tests.
> The restricted Java/Graal actor subset is not a general cross-backend feature.

## Immutable source candidates (2026-11-03)

- Java draft `oreslang-source.java#2422`:
  `2173467bc0c1534c41b614b8c2112c66d9a5fe59` (root tree
  `2d1bcbacdd68b7859b80d9a5bc83600b3fc705d7`).
- LLVM draft `oreslang-source.llvm#1277`: source identity must be repinned to
  the final commit/tree after this parity packet is committed.
- The separate LLVM drafts `#1289` and `#1290` contain overlapping
  critical-body and modifier work. They are **not** implicitly merged into
  `#1277`. Reconcile their tests and implementation in an independent review.

## Contract and evidence classes

| Behavior | Java PR #2422 | LLVM PR #1277 | Required next proof |
| --- | --- | --- | --- |
| Reject legacy `async` callable modifier | Authored compiler tests | Authored real-compiler negative harness for both `.atto`/`.ores` | Stepful compiler test on exact heads |
| Reject both `critical suspends` and `suspends critical` | Authored parser tests | Authored compiler negatives including `routine` | Exact diagnostic and zero emitted IR |
| Synchronous `fnc` scalar baseline | Authored parser positive | Two native-IR positive inputs | Real emitted IR + executable native output |
| Standalone `critical` without owned gate | Restricted fail-closed | Explicit fail-closed native compiler diagnostic | No IR, no guest body run |
| Non-suspending body under actor-owned `critical` | Restricted Java AST/effect analysis | **Not lowered to LLVM guest execution** | LLVM MIR/IR, actor ownership and body effect proof |
| Bounded FIFO logical critical tickets | Host `CriticalGate` + tests | Native `CriticalGate` + tests | Exact-head executed host tests and per-actor binding |
| STOP/retirement | Host Future cleanup and gate retirement | FIFO queued cancellation receipts + owner identity | Scheduler cancellation delivery outside the gate lock |
| Scope/ticket ABA fence | Per-gate Java identity/epoch | Overflow-fenced native scope + per-gate token | Stress, restart and stale-ticket rejection |
| Task transfer/typing authorization | Exact Program/Task identity, full typing generation and ownership epoch | `TaskCaptureAdmission` is **host/compiler-side proof only** | LLVM full typed source admission + generation-fenced guest task |
| Guest fixture output `42` | Authored `.ores` fixture, unexecuted exact head | **NOT IMPLEMENTED** | Native LLVM guest build/run and cleanup evidence |
| Guest fixture output `returned` | Authored `.ores` fixture, unexecuted exact head | **NOT IMPLEMENTED** | Early-return cleanup/physical retirement |
| NativeAOT, Wasm, .NET, Cranelift | Not evidenced by these PRs | Not evidenced by these PRs | Distinct backend receipts |

## LLVM source tests added in this slice

- `tests/native_critical_suspends_v1_admission.py`: 13 negative cases
  under each `.atto` and `.ores` extension (26 total), two positive
  scalar LLVM-IR cases, exact diagnostic checks, denial of stdout/IR/object
  artifacts for rejected source, and rejection of crash exit statuses.
- `tests/fixtures/critical-suspends-modifier-negative.atto`:
  committed invalid canonical modifier fixture.
- `tests/fixtures/critical-scalar-baseline.atto`:
  committed supported scalar baseline, **not** a critical guest.
- `tests/critical_gate_test.cpp`: FIFO retirement cancellation receipts,
  idempotent retirement, and 256 release-vs-retirement races; these are
  **native host ticket** tests, not compiled critical-source execution.

Use existing CTest names `native_critical_suspends_v1_source_admission`
and `native_logical_critical_gate`. Run with exact compiler binary SHA,
test source/tree SHA, CTest stdout and nonzero executed-step receipts.
Never substitute a source-free worker canary for private LLVM compiler proof.

## Security and merge holds

Source organization `main` was unprotected when audited, and its hosted CI
jobs repeatedly reported `runner_id=null`, `steps=null`. This is **runner
admission failure**, not a compiler pass, compiler failure, or parity receipt.
Cross-organization export of private source must remain disabled until
independent security approval, protected source-main policy, immutable carrier
workflow/test plan, numeric repository allowlist and exact-tree verification.
This document is not authorization to export source.

**Promotion**: only after Java and LLVM exact-head stepful tests, observed
guest outputs `42` and `returned`, physical cleanup and cancellation
evidence, independent review, and conformance-manifest update. Until then
LLVM critical guest parity remains **NOT_EXECUTED / NOT_IMPLEMENTED**, not PASS.
