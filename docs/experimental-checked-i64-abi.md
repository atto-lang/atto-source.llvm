# Experimental checked signed-i64 ABI (host only)

The default LLVM backend still emits **wrapping** `i64` arithmetic. That
behavior is incompatible with the candidate checked arithmetic contract in
the Java, .NET and Forge backends, and is tracked under Java
[parity issue #1068](https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/1068).
**Do not mark regular LLVM or Wasm numeric arithmetic verified.**

This opt-in is a *narrow executable bridge*, not yet general Oreslang
`throw`/`raise`/actor supervision or native function parity.

## Prototype invocation

Source file:

```ores
fnc checked(int a,int b):int {
  return (a+b)*3;
}
```

Emit LLVM IR for **host only**:

```sh
oreslang-llvmc --target=host --experimental-checked-i64 sample.ores > sample.ll
```

One extra C-compatible LLVM export is generated:

```c
/* One i64 per original value parameter; final argument is the output. */
int32_t __oreslang_checked_scalar_v1(int64_t a, int64_t b, int64_t *output);
```

Return codes:

| Code | Meaning | Output memory |
|---|---|---|
| 0 | All operations succeeded | Exactly one full-width signed-i64 value written |
| 1 | Any addition, subtraction, multiplication or negation overflowed | Unmodified |
| 2 | Null output pointer | Unmodified |

The generated companion body uses LLVM's `llvm.sadd.with.overflow.i64`,
`llvm.ssub.with.overflow.i64` and `llvm.smul.with.overflow.i64`
intrinsics. Unary negation is `0 - operand` using the checked subtraction
intrinsic. A failure branches directly to a status return, **never**
`llvm.trap()`, a process abort, or a regular wrapped result. Intermediate
operations are each checked before subsequent expressions execute.

## Admission boundaries

The flag accepts precisely one top-level `fnc checked(...):int` with
0–4 unique by-value `int` parameters and one pure integer expression.
Admitted expression nodes: signed-i64 constants, named parameters,
unary minus, and binary addition/subtraction/multiplication, depth below
64. Every local, owned value, class, actor, call, effect declaration,
guard, statement, cleanup, suspend, generic, module, Wasm and Cortex-M
program is rejected. There is consequently **no actor or owner to clean
up** in this experimental path; it does not imply that general native
arithmetic has safe unwind semantics.

Without the opt-in flag, this companion symbol is absent and default
LLVM lowering remains unchanged. Even with the flag, the normal
`checked` function's existing i64 ABI is not changed; C consumers
must deliberately call the `__oreslang_checked_scalar_v1` export.
A pointer must refer to writable, valid i64 memory: only null is checked,
not arbitrary invalid foreign pointers.

## Required tests

`tests/native_checked_scalar_experimental.py` must be executed against
a real `oreslang-llvmc` commit and Clang in CTest. It tests addition,
subtraction, multiplication, unary negation and intermediate overflow
at runtime, safe values, null-output handling, failure atomicity, and
unsupported-program admission.

**Current status:** implementation committed but exact-head native compiler
CI is not yet funded/executing, so the experimental ABI is **unverified**
as a compiled Oreslang feature. Source-free LLVM intrinsic stress tests
on an alternate private runner show toolchain viability only.

Before changing the default arithmetic contract, design a typed failure
channel through all native call frames, integration with actor supervisors,
owned arena cleanup across unwind, and deterministic error serialization.
The experimental status ABI cannot simply be substituted for that work.

## Whole-module checked-function graph work admission

The separate `--experimental-checked-i64-functions` (trapping) and
`--experimental-checked-i64-result-functions` (recoverable result-v2)
profiles admit a host-only acyclic graph of pure signed-i64 functions.
Each function is limited to 2,048 expression AST nodes and 64 expression
levels, and the graph to 32 call levels and 64 definitions.

**Additional denial-of-service limit:** for each possible entrypoint,
the compiler now computes a conservative *transitive expression-work*
count across the entire call graph. Its cost is the number of nodes in
the caller expression plus the cost of every nested call occurrence,
including repeated calls to the same callee. Any entrypoint requiring
more than **65,536 expanded expression-node evaluations** is rejected
before LLVM IR is emitted, using subtraction-guarded accounting that
cannot overflow `size_t`. This addresses acyclic graphs like
`f0(a) = f1(a) + f1(a)`, ..., whose cost doubles at every level while
every individual expression and the maximum stack depth stay small.

This is a static admission bound **for the experimental pure-function
subset**, not a CPU time quota or watchdog, and does not cover the
default compiler. The compiler does not charge host/LLVM overhead,
cancellation, scheduler work or resource-owning native execution against
this estimate. Both profiles include positive bounded-fanout and
negative exponential-fanout real-compiler regression fixtures.
The guest harnesses now test threshold-adjacent fanout graphs: 13 levels
have **49,149** expanded nodes and are admitted, while 14 levels have
**98,301** and must be denied with the *work-budget* diagnostic.
A 32-level linear call chain is admitted; 33 levels must be denied with
the *depth-budget* diagnostic, independently of the work budget.

A separate, independent **source-free** model was exercised in private
carrier [run #38957531939](https://github.com/voxletra/voxletra-e2e/actions/runs/38957531939)
under GCC and Clang with ASan/UBSan, 20,000 deterministic random DAGs and
10 directed cases **per compiler**. It compared budget admission against a
distinct unsigned-128-bit oracle. This is evidence for the algorithm,
**not** an execution of the actual Oreslang compiler or any permission to
mirror private source. The host exact-source CTests remain blocked.

The two real guest harnesses additionally compile the **13-level**
admitted fanout graph through the actual Oreslang frontend, link its
emitted LLVM IR with Clang at **-O0 and -O2**, and execute positive and
overflow cases. In the trap mode, `entry(1)` must be 8192,
`entry(-1)` must be -8192 and `entry(INT64_MAX)` must signal a genuine
overflow trap. In result-v2 mode, exact results, status=1 and untouched
output on overflow, and null-output status=2 are checked. The trap-mode
negative admission harness also refuses any partial LLVM IR on stdout.
These are **added, not yet executed** Oreslang compiler tests.

A separate independently authored C++ checked-fanout ABI *double*
(not Oreslang-generated LLVM) ran on funded private-hosted GitHub Actions
at [ClipTown run #38984007416](https://github.com/cliptown/cliptown-e2e/actions/runs/38984007416):
GCC and Clang at **-O0 and -O2** both passed ASan/UBSan host checks and
real SIGILL overflow traps (four configurations). The exact carrier
commit was `40398788ca70b68263709b0ff3f65034ea3b6bfb`; actual
runner ID `1000005492` and six completed job steps were verified.
The preceding attempt on Voxletra [#38983871653](https://github.com/voxletra/voxletra-e2e/actions/runs/38983871653)
failed at **runner_id=0, zero steps**, and does not count as a test.
Only synthetic code moved to the second carrier. No confidential
compiler or actor Git tree was exported.

A green fixture/reference oracle by itself does not prove that the
compiler/guest tests have run or that actor cleanup is safe.

## Release CTest inventory admission hardening

The exact-source LLVM CI **must fail** before expensive numerical guest tests
if any of the following three registered CTests is absent, disabled,
expected-to-fail, silently skipped, or pointed at a different executable
harness: `numeric_experimental_checked_i64_status_abi`,
`numeric_experimental_checked_pure_function_graph`, and
`numeric_experimental_checked_result_function_graph`.

The `tests/ctest_manifest_gate.py` release check now makes all three
mandatory, verifies the expected Python guest harness path,
`--compiler oreslang-llvmc` and `--clang` arguments, and rejects
unsafe/missing CTest timeouts. New adversarial tests reject missing cases,
no-op substitutions, wrong compiler/script options and inverted/skipped
tests. The exact-source workflow runs the inventory gate **before**
the selected CTest run, so an incomplete targeted CTest selection
cannot be reported as a green compiler result.

An independent **source-free** admission model (not the actual Oreslang
manifest parser) passed six hosted adversarial unittest methods in
[ClipTown run #38991528146](https://github.com/cliptown/cliptown-e2e/actions/runs/38991528146),
SHA `bf150926e75e6de86d9ed4ed7f7adb0f65b01893`, with allocated
runner ID `1000005498`, all hosted steps passing, and archived
three-day evidence artifact ID `12085561480`. The same run exercised
the independently authored bounded-fanout host model using GCC and Clang
at -O0/-O2 with sanitizers and genuine SIGILL traps. No private
compiler-source Git tree was transported across organizations.

**Limitations:** the newly edited Python manifest gate, its real CTest
configuration, full Oreslang guest compiler, and all other backend runtime
suites have **not** executed successfully on source-org Actions.
Synthetic tests cannot promote compiled parity or authorize private export;
Java security issue #962 and platform-conformance incident #22 remain
unresolved.
