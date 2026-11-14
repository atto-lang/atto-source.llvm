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
The PR remains open pending real exact-head guest execution and independent review.

## Stage-1 concrete `int` generic AOT specialization (new work)

The native LLVM frontend already type-checks an intentionally narrow scalar
generic ABI: concrete type arguments must resolve to `int`, and generic
instance methods, generic classes and trait witness specialization are not
supported by this slice. Source canonical extension is `.atto`.

`NativeScalarSpecialization.hpp` now derives one deterministic
`$atto_gN$int...` suffix from each verified scalar generic declaration.
The real `LlvmCodegen.cpp` producer uses that suffix for an **internal,
precompiled LLVM function**; ordinary calls still use the existing verified
exact selector map, so two calls to `identity<int>` share the same
`identity$atto_g1$int` function and never require runtime method-name
lookup or JIT compilation. Source-defined `$` identifiers are forbidden;
duplicate output symbols and repeated declaration selectors reject.

**Deliberate limit:** this stage eagerly instantiates at most 256
declarations at the single `int` concrete tuple. It is not yet a
whole-program, *reachable-only* multi-type monomorphization graph and does
not fulfill the full `common-source #265` specialization-closure contract.
The new conservative pre-emission size estimate is a bounded admission check,
not a final code-size proof. Trait witnesses, class methods, per-module linker
ABI and cross-backend parity remain separate implementation gates.

New tests:
- `native_scalar_i64_specialization_plan`: compiler helper constructed
  with the actual typed native `Program` IR types; positive canonical
  symbol plus 16 adversarial rejection checks for forged identity, ABI,\nclosure roots and the pre-emission budget.
- `native_scalar_i64_specialization_guest`: canonical `.atto` source
  compilation, exact **one internal definition / two direct calls** in LLVM
  IR, real Clang-linked guest exit 42, plus the preexisting scalar generic
  positive and negative source corpus.
- `llvm-static-dispatch-aot-exact-head.yml` requires both CTests,
  in addition to the existing nominal direct-call and sanitizer tests.

Evidence remains **NOT_EXECUTED at the updated PR head** until an
authorized run with nonzero executed steps produces full compiler build,
IR, native guest, and negative-test receipts. A no-step Actions failure
is runner admission failure, not a compiler result. Independent reviewer
approval and exact-head merge gates still apply.

## 2026-11-11 bounded scalar specialization closure — production preflight

`freeze_native_i64_specializations` now invokes
`check_native_i64_specialization_closure` **inside the production LLVM
code-generation path, before emitting function bodies**. The checker reconstructs
the generic-to-generic dependency edges from compiler-owned expression and
statement ASTs, including nested branch, initializer and guarded-return
expressions. Repeated calls to the same concrete specialization create one
edge; recursive edges and cycles among already-emitted concrete symbols are
allowed and terminate by visited-node identity.

This intentionally narrow admission uses:
- at most 256 concrete scalar-i64 generic symbols (existing declaration cap);
- at most 131072 inspected AST/statement nodes;
- at most 65536 distinct generic dependency edges;
- an 8 MiB conservative pre-emission estimate for generic bodies
  (256 bytes per emitted generic plus 64 per inspected node).

Every generic declaration is an emission root because this backend currently
emits all generic bodies, including unused declarations. This is **not** an
optimized reachable-only graph or a final linked-binary byte measurement.
It does not authorize unresolved generic trait witnesses, nominal generic
methods, AOT polymorphic receiver devirtualization or actor state access.
The normal type/ownership/effect admission remains mandatory; this additional
codegen check cannot replace those passes.

`native_scalar_i64_specialization_plan` now additionally covers repeated-edge
deduplication, finite two-node recursion, and refusal of a forged manifest root
with no compiler-owned function body, plus a deliberately oversized flat
compiler AST. The active test receipt states 16 negative cases. No new test result is claimed for this code merely because
it is committed.

**Runner blocker:** on the current source repository, `main` reports
`protected=false`. The private carrier contract does not include an
independently reviewed test plan authorizing LLVM PR #1663. Therefore a
cross-organization temporary source mirror is **not authorized**. Current
source-org Actions jobs fail with `runner_id=0` / `steps=[]`.
An admin must restore source-local admission or complete the protected
source + approved carrier governance before a new exact-head compiler run.
Earlier guest receipts apply only to their recorded older heads.

## 2026-11-11 compiler-owned concrete direct-dispatch image (LLVM follow-up)

The production `LlvmCodegen.cpp` path now calls
`freeze_native_i64_dispatch_image(source)`, rather than consuming only an
unstructured selector-to-suffix map. The frozen image contains:

- one `NativeI64DirectTarget` per admitted generic declaration, with its exact
  compiler selector, emission suffix, lowered ABI arity and ordered concrete
  `int` type tuple;
- the bounded, verified `NativeI64SpecializationClosureEvidence`, including
  deterministically sorted generic-to-generic direct-call dependencies;
- exactly one image entry per selected implementation; unmatched selectors
  receive no specialization target.

The LLVM declaration pass consumes these target records, emits only
`llvm::Function` direct symbols for this scalar slice, checks source ABI arity
and concrete type tuple size, rejects a symbol collision even when LLVM would
otherwise silently uniquify the name, and requires that the number of emitted
concrete specializations matches the frozen image. Existing type, ownership,
effect and capability admission must still run before this pass.

`native_scalar_i64_specialization_plan` now also verifies the frozen target
identity, non-generic absence, ordered two-parameter tuple and exact dependency
edges for both a two-call diamond/reuse and a two-node recursive cycle. The
C++ unit test is wired to the same CTest and source exact-head workflow as the
real LLVM compiler/guest gate.

**Capability boundary:** this is an actual production compiler metadata
handoff for the already-supported scalar `int` generics, **not** a general
typed witness resolution engine, virtual override table, per-actor handler
admission, or five-backend common MIR. Static dispatch must continue to be
validated independently for the full language and for each backend.

**Verification boundary:** No real source-org job on this new head has supplied
executed compiler/guest steps. Running source-only or isolated host tests cannot
promote this to native guest AOT parity. Keep head/tree IDs, emitted IR, Clang
linked guest exit codes, SHA-256 receipts and independent reviews tied to the
exact head used for any promotion.

## 2026-11-11 frozen direct-call ABI audit — non-generic budget isolation

The native LLVM scalar-`int` dispatch image now freezes the entire currently
represented direct-call ABI, rather than relying on selector and arity alone.
Each `NativeI64DirectTarget` contains parameter names, scalar representations,
borrow/borrow-mut/value modes, generic slot identity, class/sum metadata,
the explicit return type and void flag, return class/sum representation,
declared effect bits and declaration presence, and export/internal flags.

The production `freeze_native_i64_dispatch_image` factory rejects unresolved
aliases, invalid/unrecognized effect bits and compiler-generated C u32
entrypoint trampolines for scalar generic targets. LLVM lowering calls
`verify_native_i64_direct_target` again and independently verifies the
physical LLVM parameter types against the frozen borrow modes and return
`void`/`i64` shape before constructing the direct-call declaration.

The bounded generic dependency traversal now examines *emitted generic
functions only*. Nongeneric source call sites and actor handlers use the
ordinary compiler verification pipeline; they must not consume this distinct
generic closure/size quota or mint generic dispatch roots. Regression coverage
includes a deliberately oversized nongeneric function in a module with one
valid generic, plus 11 new forged ABI mutations (labels, borrow modes, parameter
and return types, suffix, effects, exported flag, unknown effect bits, and
unresolved return aliases). The native specialization C++ test reports 27
adversarial negative cases; added test bodies must still actually execute.

This audit is intentionally bounded to the currently implemented native
scalar-int backend. No separate capability/witness table is introduced by
these metadata checks, and they do not establish dynamic overriding or
isolated-actor message-dispatch semantics. Real native `.atto` guest
execution, sanitizer evidence, exact-head root tree, compiler SHA256, and
independent code review remain required.

## 2026-11-11 module-qualified linker-symbol and hidden-owner ABI hardening

The real LLVM emitter now uses `plan_native_aot_function_symbol` from the
compiler's frozen scalar-i64 dispatch image. This is the single name builder
for module-qualified functions, overloaded source arity, internal concrete
generic suffixes, and the target-specific `main` rewrite.

Before creating an LLVM declaration, it rejects zero/overlong/NUL-containing
symbol identities, attempted `llvm.*` intrinsic shadowing, and collisions
with known unqualified host/runtime symbols (including `abort`, `write`,
string/record helpers, actor runtime calls and reserved entrypoints). A
qualified user method such as `payments.write` is distinct from the host
`write` declaration. Native, wasm32 and Cortex-M legitimate entrypoint
rewrites are explicitly preserved.

The production emitter checks `module.getNamedValue(symbol)` **before**
creating a declaration, retains its emitted-symbol uniqueness set, and
verifies the LLVM function name afterwards to detect unexpected LLVM global
uniquification. Runtime/import linkage is still subject to ordinary module
validation; this hardening does not authorize unresolved cross-module
imports or promise linked, multi-module typeclass dispatch.

In the same implementation, the frozen target's explicit source ABI is
checked against the physical LLVM signature with **exact** argument count:
value/borrow parameters, compiler-added class-array region pointers, and
the two owned-result hidden slots are accounted for separately. An
incorrect/missing/extra hidden ownership parameter now aborts compilation
instead of being overlooked by a lower-bound argument-count check.

The host C++ specialization-plan test now covers two distinct qualified
modules, overloaded source arities, wasm/native/Cortex-M entry symbols,
unqualified runtime shadowing, permitted qualified names, `llvm.*`
intrinsic spoofing, embedded NUL identities and an invalid zero-sized
overload family. The case receipt is 37 adversarial rejections in the
C++ suite; **committed tests are not executed evidence**.

The implementation remains confined to the already-checked scalar-int
specialization slice. Real LLVM/Clang builds, source guest exit codes,
sanitizer checks, tree/binary hashes and independent reviews are still
required on this exact PR head before merge.

## 2026-11-11 compiler-bound scalar generic call witness (LLVM follow-up)

The frozen scalar-int direct target now records its **exact** emitted full
linker symbol alongside the verified module/overload selector and ordered
concrete type tuple. The codegen symbol planner rederives the symbol and
rejects any difference before LLVM declaration emission. This prevents a
forged same-suffix symbol in a different module from being accepted as the
same compiled target.

The production `RegionMoveLowering` call path now additionally retains
compiler-created `llvm::Function*` pointers for all frozen scalar generic
targets and their full emitted symbols. When lowering an ordinary admitted
generic `Call` with this context, the emitter checks:

- call selector and source argument count equal the frozen target;
- selected callee pointer equals the exact declaration object in the ledger;
- emitted and frozen full symbols agree, with no same-suffix alias;
- internal linkage, source formal names and explicit arity match.

An imported runtime symbol or another compiled function with a matching
suffix cannot substitute as the selected generic callee through this path.
The ledger is a compiler-owned lowering structure, not a runtime registry.

The C++ specialization-plan test contains positive and adversarial metadata
witness cases (selector, arity, module spoof, emitted symbol, formal labels,
external-linkage rejection). The test's reported adversarial-case count is
45; this is **a declared test count, not executed passing evidence**.

`tests/native_scalar_generic_source_execution.py` additionally contains
`nested-generic-direct-chain`: `outer<int>` must invoke `leaf<int>`
with exactly one internal LLVM definition and one direct call for each
concrete symbol, and the Clang-linked native guest must exit 42. It does
not rely on an interpreter or JIT to compile `leaf` at runtime.

**Boundary:** This is concrete scalar-int call-target evidence, **not**
typeclass witness selection, virtual override devirtualization, or a
multi-type specialization solver. The remaining callers not lowered through
`RegionMoveLowering` still rely on the separately checked function-selector
and declaration maps; no new claim of universal call-site witness coverage
is made. Real compiler/guest execution and independent review remain gates.
