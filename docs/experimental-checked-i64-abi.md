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

A green fixture/reference oracle by itself does not prove that the
compiler/guest tests have run or that actor cleanup is safe.
