# AttoLang Wasm actor suspension/continuation ABI v0 — explicitly not admitted

**Status:** compiler-negative gate AUTHORED / exact-head execution NOT_VERIFIED. No actual actor guest suspend/resume compiler ABI, continuation scheduler, or ownership lowering exists in this branch.

## Why the gate exists

LLVM [#1551](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1551) introduces a narrow, pure, **synchronous** recursive actor trampoline using private [64 x i64] guest-frame slots. It is not permission to emit `suspends fnc` or `await`. A WebAssembly engine activation is still synchronous and returns immediately; the host has no way to safely preserve lexical scope, owned roots, scheduler eligibility, or resume PC across actor termination and restart.

The compiler CLI now explicitly **rejects** `--experimental-wasm-actor-continuations-v0` and `--experimental-wasm-actor-suspend-v0` with exit status 2 and the diagnostic `wasm32 actor continuations are NOT IMPLEMENTED`. Rejection precedes source compilation and LLVM IR emission and is independent of command-line flag order. Unknown flags still fail closed. Do not silently alias either flag to `--experimental-wasm-actor-recursion-v1` or emit an ordinary `i64` return as a stand-in for a suspended state.

The original four-export legacy scalar actor ABI and the opt-in checked-recursion five-export ABI remain unchanged; no `resume`, `suspend`, `poll` or host callback export is admitted as a new guest continuation entry point.

## Canonical negative source

`tests/fixtures/wasm32-actor-suspended-negative-v0.atto` has Git blob `aebd40549e88b49c906fd3fa2b0569ec7800deec`, **identical** to the Wasm host ticket contract `contracts/actor-continuation-fence-v0/suspended-actor-negative.atto` in [Wasm PR #429](https://github.com/ores-truffle-oreslang/oreslang-source.wasm/pull/429).

Its `suspends fnc deferred(int value)` must not be silently ignored or admitted through either unflagged `--target=wasm32` compilation or the explicit checked synchronous recursion path. Syntax rejection is allowed, as this profile does not yet support executable guest continuations. Rejected source must produce neither IR on stdout nor an artifact in the caller directory.

The companion fixture `wasm32-actor-bounded-recursion-v1.atto` (Git blob `947781cddf09d8e3e08c51cff2b7d8960e2d4750`) remains a **positive** synchronous source-compiled IR baseline. Blocking unsupported `suspends` must not regress existing bounded actor recursion.

## Verification requirements

`tests/wasm32_actor_continuation_negative_v0.py` runs the **actual native compiler executable from exact source HEAD** with:

- Eight flag-order/target combinations across two rejected continuation/suspend flags, each requiring a nonzero status, no stdout IR, exact rejection diagnostic, and no new file.
- Unflagged wasm32 source-suspension negative and checked-recursion-with-extra-suspending-function negative, both requiring nonzero status, no emitted IR/artifact.
- One permitted bounded synchronous actor source that must generate the expected wasm32 target triple, checked iterative push block, and typed status export; it must not emit an engine-recursive helper call.

CTest target `wasm32_actor_guest_continuation_fail_closed_v0` and source-org exact-head workflow `.github/workflows/wasm-actor-continuation-deny-v0.yml` are authored. A hosted PASS requires a real assigned runner, nonzero executed steps, exact head/tree receipt and compiler executable hash. The resulting receipt must classify `compiler_guest_suspension_negative_executed=true` only after actual execution, and `guest_resume_compiler_backed_executed=false` always in v0.

## Proposed future ABI (NOT IMPLEMENTED)

Before admission, source-compiled `suspends fnc` must lower into a typed frame representation with:

- A stable actor identity plus independently minted incarnation/generation fence; a transferable serializable payload must NEVER serve as a resume authority.
- A bounded state machine for PC/return destination/operand locals, ownership-root bitmaps, cleanup and exceptional edges. No native engine call-stack continuation or JavaScript closure capturing guest lexical scope.
- Suspend and resume operations that do not hold `critical` sections, host carrier threads or actor turns across waiting; scheduler double-entry eligibility accounting and exactly-once transition.
- Host-only opaque continuation tickets with one-use claim, cancellation and quota semantics. The Wasm host ticket implementation is **an admission substrate, not a guest continuation**.
- STOP/cancellation, supervisor restart, carrier migration, and stale-generation rejection before touching old frame memory. Repeated adversarial interleavings and negative mutation proof.
- Exact-head independent native LLVM and Java compiler-backed guest traces, Wasm Clang `-O0`/`-Oz` and AOT, fail-closed malformed receipts, and independently reviewed source/compiler/IR/Wasm digests.

Until a new versioned ABI has evidence and approval, this repository must not publish a resumable guest export or call host code to emulate a guest suspension.

Keep [LLVM PR #1551](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1551) and this change draft. Source-org GitHub Actions remains blocked before runner admission under the documented billing/spending-limit class ([conformance issue #315](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/315)); no `steps=null` CI result counts as a compiler pass/failure. No cross-organization source mirror or review waiver.
