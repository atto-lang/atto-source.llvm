# Static dispatch versus controlled dynamic dispatch — LLVM audit

Status: **audited native class-method slice; execution not yet verified at this PR head**.
This is an implementation-specific follow-up to the shared AttoLang full-AOT language
closure contract, not a declaration that virtual/interface dispatch is implemented.

## Source and lowering findings

- `frontend/MethodResolver.cpp` binds an instance call with a statically known
  nominal receiver to a compiler-owned `Class.method` symbol and an implicit
  shared or exclusive receiver loan. There is no guest-visible string lookup.
- `backend/LlvmCodegen.cpp` handles `Expression::Kind::Call` by looking up
  `native_function_slot(expr.name, expr.arguments.size())` in the compile-time
  function map and emits `IRBuilder::CreateCall(llvm::Function*, ...)`.
  The native class-method slice therefore emits a direct LLVM call.
- Source declaration admission rejects duplicate method name/explicit arity.
  The resolver additionally rejects duplicate class identities and duplicate
  method slots in linked/compiler-owned IR **before call resolution**, including
  unused duplicate slots, instead of letting declaration order select a target.
- `tests/native_static_dispatch_aot.rs` provides an independent Rust-only
  compiler-to-LLVM-IR assertion for both concrete and overloaded class
  methods with **exact** compiler ABI symbol matching, then links and
  executes both guests at O0 and O2 (expected exit code 42).
  Eight Rust predicate tests reject typed/ordinary indirect calls,
  incorrect overload slots, namespace/suffix spoofing and declaration-only evidence.
- `tests/native_static_dispatch_resolver_test.cpp` links the actual C++
  frontend resolver with no compiler simulation. It rejects duplicate nominal
  identities, uncalled duplicate slots and forged method receivers, while
  preserving legal same-name/different-arity methods; this is a supplemental
  host-level check, not executed guest evidence.
  Existing legacy class-method regression fixtures remain unchanged.

## Semantic rules for future implementations

1. Concrete class receiver with a statically resolved exact method:
   emit a direct call, subject to ordinary override semantics. Do **not**
   devirtualize a truly overridable call merely because the declared type is
   known; prove the runtime target is unique or use a closed-world guard with a
   correct fallback into precompiled targets.
2. Static generic/trait parameter: prefer specialization/monomorphization or
   compile-time witness selection. The witness must preserve type, effects,
   ownership, visibility and capability obligations.
3. Existential/trait-object receiver or runtime-selected implementation:
   an indirect call may be necessary, but every possible implementation must
   be in the compiled closure, with deterministic ABI/slot identities. AOT
   permits precompiled indirect calls; it does not permit runtime method-name
   lookup or generation of new code.
4. ActorRef / virtual actor: dispatch by stable actor identity and generation
   using the runtime's message/admission rules. Do not transform this into a
   direct state-mutating function call or bypass suspension, fairness, quotas,
   isolation or nonreentrancy.
5. Macro-generated specialized calls are compiler-generated syntax, never
   unchecked executable IR. Re-run type/effect/borrow/isolation/capability
   checks after expansion, resolve all reachable specializations before
   linking and erase macro machinery from the guest artifact.
6. Optional JIT can optimize already-admitted calls, but disabling it must
   preserve full execution of the same closed program from standalone native
   AOT or a closed guest Wasm module.

## Acceptance and evidence boundary

Positive: native source class calls execute with direct `@Class.method` IR
at O0 and O2; mutable/shared receiver loans remain sound. Negative:
unknown/private/wrong-arity/duplicate method slots reject before IR, and
unresolved virtual/interface calls never silently turn into name lookup.
For genuinely polymorphic calls add override, base reference, witness,
and adversarial stale-actor-generation fixtures before claiming parity.

Run the new CTests `native_static_dispatch_aot`,
`native_static_dispatch_predicates`, and
`native_static_dispatch_resolver_ir_integrity`, plus the existing
`native_source_class_methods_release` and
`native_source_class_methods_sanitizer` with the actual compiler and clang,
on the **exact PR head**. The isolated exact-head workflow is
`.github/workflows/llvm-static-dispatch-aot-exact-head.yml` (LLVM/Clang 19). Record toolchain, compiler/blob/tree IDs, executed
steps and final native exit codes; a zero-step runner is NOT_EXECUTED. This
patch includes no measured speedup claim and no verified JIT-vs-AOT parity.

## Current CI status (2026-11-10 / 2026-11-11 UTC)

The initial workflow attempts failed with `steps=[]`; a manual retry of
`Rust-only / no Python` also failed with zero executed steps. The dedicated
compiler-backed workflow run `42943943177` likewise reported `steps=[]`.
That run is runner admission failure, **NOT_EXECUTED**, not a Rust/C++/Clang
pass or failure. Later commits require a fresh exact-head executed receipt.
The PR stays draft pending real guest execution and independent review.
