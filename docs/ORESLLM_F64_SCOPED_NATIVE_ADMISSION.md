> **Superseded:** the implicit-conversion and class-field limits described here no longer apply. See [native floating point](NATIVE_FLOAT.md).

# OresLLM LLVM FP64 scoped-value milestone (draft)

This change is stacked on draft [LLVM PR #1236](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1236) and is **NOT** a verified native compiler pass, OresLLM training implementation, or backend parity certificate. Never merge this stack ahead of the independently verified base.

## Implementation
- Retain the **resolved** `ScalarType::F64` in native scalar variable and field AST nodes. Previously `validate_scalar_expression` returned the resolved type but left `expr.resolved_type` at its default `I64`; `LlvmCodegen::lower` chooses the load type from that field. Leaving an F64 lexical variable marked I64 could turn a `double` slot into an incorrect i64 load at lowering.
- Admit explicit `val f64`, `const f64` and `let f64` in synchronous ordered/conditional source scopes, preserving strict no-implicit-int-coercion checks.
- Allocate conditional-scoped F64 locals as native LLVM `double` slots, not i64 slots, preserving the existing lexical lifetime start/end and branch cleanup. Reject a non-double initializer before publishing the local slot.
- Refuse a `ScalarType::F64` field read in LLVM codegen until the typed double struct-field ABI and borrow provenance are proved; do **not** allow a default `i64` field load to reinterpret double bits.
- F64 parameters/returns are now admitted only for the restricted pure synchronous scalar ABI described below; class fields, arrays, async/suspension, owned tensors, actor mailbox transport and full FP exception semantics remain fail-closed.

## Typed f64 function ABI follow-up (unverified)

The stacked draft now admits **pure synchronous scalar functions** with by-value
`f64` parameters and `f64` return values. Each f64 parameter has an actual LLVM
`double` formal and stack slot, and an f64 return is an LLVM `double` result,
including guarded and structured return cleanup snapshots. Call sites require
exact LLVM formal/argument type identity; the parser and independent scalar
type checker prohibit implicit int/f64 conversion. Nested calls preserve
the resolved f64 expression type. This is a same-compiler native calling
convention, not yet an established cross-platform C/foreign ABI.

**Explicitly unsupported:** f64 `main` process status/parameters, borrowed f64
parameters, f64 class methods and class fields, generic and sum payload
transport, owned/borrowed aggregate function frames, Task/Future suspension,
actor messages, and full Tensor or Array<f64> data structures. The first
f64 ABI slice rejects these before emitting any LLVM IR. The code generator
separately checks entry/field/return and parameter types if supplied an
unvalidated AST.

Twelve additional positive native source candidates exercise gradient function
arguments/returns, nested calls, mixed int/f64 parameters, returned lexical
values, guarded and structured returns, int result from double argument,
and signed zero across the function boundary. Eight negative guests reject
f64 process entry, borrowed float, incorrect parameter and return types.
The running test inventory is now **32 positive / 21 negative** candidates,
all requiring the actual compiled `oreslang-llvmc`, IR verification, Clang
linking, native exit results and expected no-IR compiler rejections.

### Native acceptance contract correction (2026-11-01)

Two older negative fixtures were **invalidated by the newly implemented f64
function ABI**: one expected all f64 returns to fail, the other expected
by-value f64 parameters to fail. They now exercise real compiler type errors
(bool-to-f64 return and bool-to-f64 parameter), rather than an obsolete
feature ban. Additional coverage includes reordered named arguments, forward
double calls, f64 ternary returns, bool results from double comparisons,
named-argument type mismatches and a guarded return type mismatch.

The shell now prevents a duplicate or malformed fixture ID from replacing a
prior source, IR or executable in its temporary directory. Expected inventory:
**32 positive / 21 negative** unique candidates, no skipped entries. Both
the test inventory and declaration/ABI source are draft-only until the real
native compiler and clang execute against the exact commit.

## Real native acceptance, required but not observed
The existing `native_f64_scalar_e2e` CTest builds real `oreslang-llvmc`, compiles native guest source into LLVM IR, links with Clang and executes the binary. This branch extends it from 14 to **32 positive native-exit-42 guests** and from 12 to **21 negative no-IR source-rejection guests**. Three new positive tests exercise branch-scoped F64, else branch lifetime and mutable branch F64 updates. New negative checks reject implicit int-to-F64 branch initializer and assigning int to `let f64` in a branch. The integer baseline and all prior IEEE, signed-zero, NaN and infinity tests remain.

Required observations: exact private source Git tree and test file SHA, compiler process exit 0, nonempty valid LLVM IR, real executable exit 42 and empty stdout/stderr for each positive, compiler process **exit 1**, expected diagnostic and zero IR for each negative. Run LLVM verifier and sanitizers, then independent numerical comparisons. GitHub Actions `steps=[]` means runner admission failure, **not** an observed CTest failure or pass.

## Next source-level implementation gates
1. Confirm this ABI/scoped slice executes all 32/21 native tests at the exact branch SHA and review correctness around branch joins and lifetime intrinsic ordering.
2. After verifying typed F64 function **parameters/returns**, implement separately checked class fields and constructors (still unsupported).
3. Add bounded owned `Array<f64>` with strict layout/borrow/teardown, typed imports and per-actor zero-GC arenas.
4. Lower canonical OresLLM integrated Tensor → Autograd → NN → Optim FP64 guest with equivalent gradients, 20-step loss reduction and finite-difference/held-out thresholds. No substitution with host JS, Rust, or C neural training.
5. Then separately prove WebAssembly execution, Cranelift JIT, .NET NativeAOT and GraalVM Native Image. Source-free LLVM toolchain CI, shared i64 vectors and nonzero-step host checks are not such proof.

## OresLLM numerical primitive probes (source-authored)

Three additional real-guest candidates now exercise a 1D squared-error analytical
weight gradient, a **finite-difference** gradient using an exactly representable
binary64 step (h=0.5), and an F64 gradient through a scoped mutable local. Each
must compile as Oreslang -> LLVM IR -> Clang-linked native binary and return
native exit status 42; the host-side mathematical identity is not a guest pass.
They do not call Autograd, construct Tensor2 or execute a 20-step optimizer.

The count is now **32 positive / 21 negative**. A compiler rejecting FP64 class fields, owned arrays, tensor transport and
actor/suspension interfaces remains correct for this narrow slice; these
limitations prevent any claim of complete OresLLM execution.

## Confidentiality
Source remains in its originating private `ores-truffle-oreslang/oreslang-source.llvm` repository. No cross-organization compiler export/mirror is allowed until independent resolution/authorization under [platform security issue #22](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/22). PR reviews, runner execution, confidentiality clearance and merge approval are independent gates.
