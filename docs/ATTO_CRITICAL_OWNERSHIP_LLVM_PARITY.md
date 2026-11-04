# AttoLang LLVM critical/ownership parity: exact-source admission and native scalar controls

Status: **DRAFT / source changes authored; actual LLVM compiler and guest
executions NOT_EXECUTED until stepful CI** (2026-11-03).

This independent branch is built **directly on the current LLVM `main` tree**
(`040058c1750a049be43949c41f61a87d028e6ddc`). The base already
contains `.atto` source suffix resolution, so these focused compiler
semantic/ownership changes do **not** depend on the currently conflicting
importer PR #1312. The earlier stacked experiment is PR #1392; this
main-based branch supersedes its implementation for merge consideration.
No code or Git tree has been copied outside the origin GitHub organization.

## What this changes

The production native frontend `Parser.cpp` now checks conflicting
callable modifiers before its existing separate `critical` and `suspends`
fail-closed gates. Both orders use the stable Java/Graal compiler
diagnostic `E_CRITICAL_SUSPENDS_DECL`.

- Top-level `fnc` and `routine` declaration positions;
- class member declaration positions (which parse separately from
  ordinary function headers);
- actor method entrypoints before on-message-only parsing;
- lexical comments/newlines separating the two modifiers.

The parser uses a copy of the existing lexer, never modifies lexer state,
never treats `critical-read` as the conflicting pair and never admits
standalone critical/suspending execution into LLVM. The native actor
scheduler, scoped permits and protected-state guest heap still require
independent typed LLVM lowering.

## Sixteen independent CTest acceptance cases

| Kind | Cases | Actual capability tested |
| --- | ---: | --- |
| Critical+suspends, both orders, fnc/routine/class/actor | 5 | compiler exit 1 with `E_CRITICAL_SUSPENDS_DECL` and no IR on stdout |
| Lexical trivia around conflicting modifiers | 2 | same semantic code; comment/newline cannot bypass admission |
| Lone critical/suspends, and lone critical-read | 3 | correctly fail closed with distinct native unsupported diagnostics |
| Non-Copy class / Array sharing | 2 | real ownership checker emits `E_SHARE_MOVE_ONLY` before codegen, with no IR on stdout |
| State-container protected heap | 1 | `STATE_CONTAINER_BACKEND_UNSUPPORTED` remains a hard backend denial |
| Copyable `rt share`, `rt copy`, and `rt take` | 3 | production compiler generates LLVM IR and Clang-linked native executables exit 42 |

**The three native execution tests are conditional on the discovered
Clang toolchain.** The exact-head workflow installs Clang and requires all
14 named tests to exist and execute. The shared `tests/contract.py` harness
requires compiler exit code exactly 1 and expected stderr for negatives,
with no stdout; for positives it requires real IR and a linked/started native
process with the expected exit. No source-free scalar model substitutes for
these receipts.

## Diagnostics and unsupported semantics

| Java/Graal semantic contract | LLVM status of this patch |
| --- | --- |
| `E_CRITICAL_SUSPENDS_DECL` | **Implemented in LLVM source**, execution pending |
| `E_SHARE_MOVE_ONLY` | **Implemented in LLVM source** for non-Copy array/class and owned-field value aliases; native execution pending |
| `E_CRITICAL_READ_MUTATION` | **NOT IMPLEMENTED** for protected LRU objects; native state containers fail closed before body lowering |
| `E_STATE_BORROW_ESCAPE` | **NOT IMPLEMENTED** for protected LRU objects; no sealed-state lexical loan lowering |

The LLVM compiler must not issue one of the latter diagnostic codes merely
because generic syntax is unsupported. A future parity PR must implement
the real semantic check using class method mutability/effect inference,
nominal state owner isolation, guard-scoped borrows, and generation-fenced
read/write runtime permits.

General `define state container` remains
`STATE_CONTAINER_BACKEND_UNSUPPORTED`. No attempt was made to transplant
Java's narrow single-i64 host VM into LLVM without verified ownership,
MIR and runtime equivalence. Original private LRU source/tests remain
untouched and unexecuted by this LLVM patch.

## Reproduction and promotion criteria

From a source-authorized LLVM CI runner on the exact branch/head:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2 --target oreslang-llvmc
ctest --test-dir build -R '^atto_(critical|suspends|protected|copyable)' \
  --output-on-failure --timeout 180
```

Record exact compiler/head Git commit and tree, compiler toolchain versions,
real nonzero GitHub Actions job steps, CTest XML with 16 unique, non-skipped,
passing tests, positive native exit 42, and negative stderr/exit evidence.
The `.github/workflows/atto-critical-ownership-llvm-parity.yml` gate also
requires a clean checkout before and after testing.

The current origin-source Actions admission failure is tracked at
LLVM issue #159 and Java issue #963. A workflow with `steps=[]` is
**runner admission failure**, not negative compiler success or native parity.

### Cross-backend status

Java PR #2499 contains authored `E_CRITICAL_SUSPENDS_DECL`, share-move-only
and restricted scalar protected-state diagnostics. Its authored tests have
not executed because its private source-owned runner also lacks admission.
This LLVM child adds AOT test candidates for the Copy subset, general non-Copy share denial, and the
critical/suspends parser negative contract; neither PR demonstrates
guest parity until the runner executes.

Keep the new main-based PR draft pending exact-head native test receipts,
independent review, and the source-transfer/security incident hold. This
branch is not blocked on #1312's parent ancestry, but real compiler proof is
still required; no private code was transferred to another organization.
