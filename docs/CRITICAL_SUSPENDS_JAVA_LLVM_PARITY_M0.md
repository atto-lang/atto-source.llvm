# AttoLang critical/suspends — Java ⇄ LLVM M0 parity gate

> Status: **SOURCE-LOCAL CONTRACTS AUTHOR ONLY**. No compiler-backed Java or
> LLVM `critical` guest parity is established by this document or its tests.
> The restricted Java/Graal actor subset is not a general cross-backend feature.

## Immutable source candidates (2026-11-03)

- Java draft `oreslang-source.java#2422`:
  `892c3220dbc22ae7138d3141e13ea46404430db4` (root tree
  `01644d97450817417569cdc359826f9cc04b09ee`).
- LLVM draft `oreslang-source.llvm#1277`: source identity must be repinned to
  the final commit/tree after this parity packet is committed.
- Reconciled **selected** changes from LLVM `#1289` (non-yielding callback
  effects, logical mutex retirement and scalar copy-out compile negatives)
  and `#1290` (stable modifier diagnostics and compiler tests) into `#1277`.
  Neither source PR is closed or blindly merged. An independent review must
  still reconcile any other semantic conflicts and check the complete diff.

## Contract and evidence classes

| Behavior | Java PR #2422 | LLVM PR #1277 | Required next proof |
| --- | --- | --- | --- |
| Reject legacy `async` callable modifier | Authored compiler tests | Authored real-compiler negative harness for both `.atto`/`.ores` | Stepful compiler test on exact heads |
| Reject both `critical suspends` and `suspends critical` | Authored parser tests | Authored compiler negatives including `routine` | Exact diagnostic and zero emitted IR |
| Synchronous `fnc` scalar baseline | Authored parser positive | Two native-IR positive inputs | Real emitted IR + executable native output |
| Standalone `critical` without owned gate | Restricted fail-closed | Explicit fail-closed native compiler diagnostic | No IR, no guest body run |
| Non-suspending body under actor-owned `critical` | Restricted Java AST/effect analysis | **Not lowered to LLVM guest execution** | LLVM MIR/IR, actor ownership and body effect proof |
| Bounded FIFO logical critical tickets | Host `CriticalGate` + tests | Native `CriticalGate` + tests | Exact-head executed host tests and per-actor binding |
| STOP/retirement | Host Future cleanup, retire-if-quiescent and gate retirement | FIFO cancellation receipts; retired owner **retained until physical unwind**; atomic retire-if-quiescent | Scheduler cancellation delivery outside the gate lock; exact actor generation |
| Scope/ticket ABA fence | Per-gate Java identity/epoch | Overflow-fenced native scope + per-gate token | Stress, restart and stale-ticket rejection |
| Task transfer/typing authorization | Exact Program/Task identity, full typing generation and ownership epoch | `TaskCaptureAdmission` is **host/compiler-side proof only** | LLVM full typed source admission + generation-fenced guest task |
| Guest fixture output `42` | Authored `.atto` and `.ores` fixtures, unexecuted exact head | **NOT IMPLEMENTED** | Native LLVM critical guest build/run and cleanup evidence |
| Guest fixture output `returned` | Authored `.atto` and `.ores` fixtures, unexecuted exact head | **NOT IMPLEMENTED** | Early-return cleanup/physical retirement |
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
  idempotent retirement, 256 release-vs-retirement races, and another
  256 request-vs-atomic-empty-retirement races; these are **native host
  ticket** tests, not compiled critical-source execution.

Additional reconciled source-only test gates:

- `tests/native_critical_suspends_modifiers.py` and CTest
  `native_critical_suspends_modifier_codes`: compiler diagnostics
  `E_CALLABLE_MODIFIERS`/`E_CRITICAL_UNSUPPORTED`, with negative IR denial.
- `tests/critical_handle_state_contract_test.cpp`: reject Suspend, Io,
  Block and unverified FFI effects inside critical bodies; accepted
  state-read/write effects do not confer scheduler authority.
- `tests/logical_mutex_test.cpp`: stop/callback race, no early active-ticket
  release **even after retirement**, no new callback after STOP.
- `tests/critical_scalar_copyout_compile_test.py` and CTest
  `native_logical_mutex_scalar_copyout_failclosed`: two valid void/scalar
  C++ returns, five forbidden reference/pointer/wrapper/view/aggregate
  returns. **Host-only**, not native guest ownership proof.
- `tests/fixtures/source-actor-send.atto`: canonical extension in the
  existing `source_actor_compiled_native_lifecycle` / sanitizer E2E
  suite. It must compile source to LLVM IR, link Clang native actor runtime,
  execute with exit 43, and verify physical actor STOP receipts. This
  is a real **actor guest baseline**, not a critical-body fixture.
- C++ gate now keeps a retired physical owner until release, even though
  `holds()` denies new guest entry. This fixes an early-quiescence bug:
  before the change, STOP cleared the owner and could suggest state/arena
  reclamation was safe while a callback remained in flight.

Use CTest names `native_critical_suspends_v1_source_admission`,
`native_critical_suspends_modifier_codes`, `native_logical_critical_gate`,
`native_logical_mutex_scalar_copyout_failclosed`, and the native actor
E2E suite. Run with exact compiler binary SHA,
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


## Native C++17 runner proof — source-free, actually executed

The independent [carrier PR #45](https://github.com/gha-indie-worker/gha-indie-worker-docs/pull/45)
ran source-free [workflow #41765967238](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/41765967238)
with **7/7 executed steps**, GCC 15.2.0 and Clang 21.1.8,
both using C++17 with ASan/UBSan; `SOURCE_FREE_CANARY=1` and
`PRIVATE_SOURCE_TRANSFER=0` were confirmed by logs.

This verifies only availability of a native **compiler and sanitizer
toolchain** on the alternative organization. The run did **not** checkout,
compile, copy, inspect or execute any private LLVM/Java source. The
source-org's exact-head LLVM compiler/native and Java compiler suites still
failed at runner admission (`steps=null`) and remain **NOT_EXECUTED**.
No private mirror is authorized by this receipt.
