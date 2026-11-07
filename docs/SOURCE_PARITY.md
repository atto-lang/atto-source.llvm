# Oreslang Java/GraalVM ↔ LLVM parity contract

**Standing requirement:** Java/GraalVM and LLVM are equal-priority Oreslang backends. The language surface and observable semantics must converge on full parity: syntax, types/ADTs, modules, ownership, actors, supervision, mailboxes, async continuations, scheduling, I/O, standard library, errors and safety guarantees. A feature missing on one backend is an actionable parity gap, never an accepted permanent language divergence.

Java/GraalVM currently serves as an implementation reference for many behaviors: [oreslang-source.java](https://github.com/ores-truffle-oreslang/oreslang-source.java). The intended long-term source of truth is the shared language/runtime specification and cross-backend conformance suite; verified behavior must not depend on whichever backend was implemented first.

LLVM parity roadmap: [issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).

**Evidence status (October 2026):** complete LLVM runtime parity is **not yet verified**. The implementation inventory below is a gap-closure plan, not permission to market LLVM as a reduced Oreslang dialect. Each backend change should be paired with a parity check, fixture and tracked follow-up where needed.

## Compiler fragment currently accepted

- Zero or one `define module M [as] ... end` wrapper.
- Multiple zero- or multi-argument `[pub] fnc`/`routine` declarations,
  with `int` parameters and `: int` return annotation.
- Exactly one `return <expr>;` statement per function.
- Decimal int literals, unary negative, binary add/subtract/multiply, grouping,
  integer parameters, same-module function calls (including forward calls).
- Whitespace, `//` comments and nested `/* ... */` comments.

Semantic check runs before LLVM lowering: rejects unknown variables/callees,
mismatched call arity and duplicate declarations/parameters. Unexpected tokens fail closed.
The LLVM verifier runs before IR is emitted.

## Type-only declaration parity slice (2026-10-23)

The Java compiler follow-up [#1948](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1948)
admits `declare type`, `declare trait` and `declare interface` with
exact same-file/module definition matching, preserving visibility and
generic signature. That **full** syntax is **not yet** lowered by LLVM.

LLVM's source frontend now offers only a deliberate **scalar transparent
alias subset**, e.g.:

```ores
declare pub type Code;
define pub type Code = int;

fnc identity(Code code): Code { return code; }
pub fnc main(): int { return identity(7); }
```

`bool` aliases, explicitly declared forward dependency chains, and use-before-definition in function parameter/return signatures are also admitted. Resolution runs after declaration parsing and **before** scalar checking, ownership, ABI computation, or LLVM IR emission. Self/mutual alias cycles, cycles through chains, and excessive resolution depth are rejected.
`declare type` contributes no LLVM IR or runtime descriptor; a compatible
definition must occur in the same source scope, with the same public/private
visibility. Repeated or unresolved declarations, duplicate definitions,
builtin shadowing, class/actor collisions, module scope leakage and aliases
to unverified types fail closed. `pub define type` remains a compatibility
spelling. This is **not** a general newtype, nominal ADT, data layout or
runtime reflection implementation.

The native subset deliberately rejects generic alias signatures,
`declare trait` / `declare interface`, and recursive/non-scalar aliases.
A function may use `Code` before its definition **only if** a matching
`declare type Code;` signature precedes the function within the same
source scope. Use-before-forward remains unavailable; no cross-file
declaration linking is implied. Java supports additional type-only forms,
so **full source acceptance and execution parity is outstanding**.
The native frontend must not silently reinterpret `trait` as `interface`
or class/actor as a scalar alias.

The test `native_declare_type_scalar_source_parity` invokes the **actual
native compiler** and requires LLVM IR on accepted scalar cases, nonzero exit
and zero stdout IR for every negative. CTest registration alone or an Actions
run with zero executed steps is not evidence that it passed. Next gate:
actual JVM/native execution of the **byte-identical** fixture
`forward-scalar-alias-parity.ores` (Git blob
`093b92f47f845a61cc644c01eee975929af66e0b`) in both backends.
The Java JUnit guest test and native Clang-linked CTest both target the
integer result `7`; these are not validated until runner steps execute.
Retain explicit source/version receipts. Follow Java enum AST issue
[#1269](https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/1269)
and native ADT issue [#314](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/314)
before claiming general enum/Option/Result parity.

## Numeric division fail-closed parity boundary

Java source revision `4fc9148` defines `int / int` with a `float`
result (the real guest runtime evaluates it as `Double`), so LLVM's
i64-only expression emitter must **not** infer an i64 quotient or treat
a lexical rejection as a successful implementation.

The native frontend now recognizes `/` and `%` as operators, then
rejects each with an **operator-specific diagnostic before LLVM IR**
because floating-point result typing, division-by-zero policy, and native
remainder contracts have not yet been lowered or verified. Three
dedicated CTest negative fixtures require nonzero compiler exit and
**empty emitted IR**. This is rejection parity, *not* implementation
or runtime parity. Subsequent work must add typed float IR, promotion,
zero-divisor/overflow semantics, and differential JVM/AOT/LLVM tests.

## Native I/O naming contract (future binding admission)

All source-visible **synchronous-looking I/O** APIs must end in `_blocking`; the runtime must **transparently suspend the calling continuation and release the actor carrier** while I/O is in flight, then resume with a `T` result or an error. They must not block an OS scheduler carrier. **Asynchronous I/O** APIs may, but need not, end in `_async`, and return `Future<T>`. This includes filesystem metadata operations, sockets, DNS, terminal/stdio and HTTP operations that can block the caller. Pure functions and host-internal C++ methods do not acquire suffixes.

The canonical contract, effect-classification rules, alias checks and negative-test matrix live in [oreslang-common-source/docs/NATIVE_IO_NAMING.md](https://github.com/ores-truffle-oreslang/oreslang-common-source/blob/main/docs/NATIVE_IO_NAMING.md). Example future APIs: `fs.read_text_blocking(path)` and `await fs.read_text_async(path)`.

**Implementation gap:** The current LLVM fragment has no imported/native I/O declarations, async lowering, I/O effect metadata or runtime standard library. It must not pretend that spelling `*_blocking` alone makes a builtin supported. When an I/O binding registry is added, reject any binding classified as synchronously blocking I/O unless its source-visible name ends in `_blocking`; check methods and aliases too. Both forms require an actual non-carrier-blocking execution contract. `_blocking` calls must lower to I/O submission plus implicit continuation suspension, with no explicit source `await`; async calls return an explicit awaitable. LLVM must fail closed rather than compile `_blocking` into a host-blocking C ABI call. Add valid and invalid compiler fixtures, capability-denial tests and concurrent carrier tests before declaring enforcement.

## Ownership and borrow checker parity gate

The native LLVM compiler now has a **flow-sensitive i64-only ownership
admission pass** (`types/OwnershipChecker.cpp`). `rt copy`, `rt take`, and
`rt share` compile for trivially copyable `int` values only. They do not
allocate, share heap memory or bypass destructors. Explicit `rt take x` marks
the named parameter `x` moved; any later read of `x` within the same
left-to-right return expression or its function-call arguments is an error,
even though `int` otherwise supports implicit copies. The LLVM emitter
uses explicit left-to-right operand lowering to preserve the checked order.

The LLVM compiler now admits **non-escaping primitive i64 borrows** through
`borrow int` / `borrow mut int` function parameters and `rt borrow x` /
`rt borrow mut x` arguments. These lower to typed native pointers referencing
caller-owned, parameter-local stack slots. They are checked for immutable-vs-
exclusive alias conflicts, use-after-`rt take`, moving while loaned, and
source-order call nesting. Borrowed i64 parameters can themselves be
reborrowed for nested synchronous calls: shared parameters allow shared
reborrows only, while exclusive parameters allow shared or exclusive
reborrows, with active-loan conflict checking. LLVM forwards the existing
caller slot pointer rather than materializing a new owner. All accepted
function returns remain i64, and loan pointers cannot be stored or returned
by this source fragment. Loan lifetime is bounded by each direct call:
it ends when that call returns.

This is **not** support for general native references. Reborrows of heap
or field projections, borrowed temporaries, object/struct projections,
destructors, heap provenance,
mutable reference writes, actor-owned arenas and passing loans across
suspension are not available. A `borrow mut` parameter is currently an
exclusive read capability; no mutable assignments are admitted. The borrowed
parameter pointer ABI is internal to this restricted compilation model,
not a validated external C/FFI interface.

Until the backend has explicit heap ownership provenance, lifetime scopes,
reborrow/alias exclusion, transitive copy contracts, projection tracking,
suspension and actor-boundary rules, all **non-scalar** references and all
async/channel/actor operations must **fail closed** with a nonzero compiler
exit and **no LLVM IR**. Negative tests enforce that rule; positive
LLVM/Clang tests verify scalar borrow lowering; full ownership parity
requires the complex-object, actor and suspension cases below.

A future complex-object ownership fixture must demonstrate full-stack type checking,
lowering, IR emission, and runtime behavior in addition to differential
Java/LLVM conformance; simply accepting the parser grammar is insufficient.

## Cross-implementation parity tracker (current verified scope)

| Java capability | LLVM status |
| --- | --- |
| Lexer, modules, functions | Partial i64-only subset; full syntax and module parity still required |
| Function calls, expressions | i64 arithmetic and same-module calls |
| Type/ownership checking | Name/arity, i64 explicit moves and scoped nonescaping scalar loans; heap references and async unsupported |
| LLVM IR verification | Implemented |
| Compile and execute native guest | Clang can link i64 main on supported host |
| General target-specific ABI/object emission | Not implemented |
| Actors/supervisors, async frames, channels | Not implemented |
| Static select and reusable SelectPlan / DynamicSelectPlan | Not implemented; fail-closed fixtures and [parity contract](SELECT_PLAN_PARITY.md) |
| Cycles/Ref/WeakRef, borrow checking, arenas | i64 shared/exclusive argument-loan checking; no heap/struct/actor borrow checking |
| HPC/HFT and isolation | Not implemented |

**Unreconciled:** Java `int` width and overflow, possible runtime faults,
name mangling/linkage, operator semantics, hot reload and actor isolation. Arithmetic
currently lowers to LLVM signed i64 operations **without** `nsw`/checked traps; resulting
wraparound is provisional. Division deliberately remains unsupported to avoid undefined
zero-divisor and signed-overflow behavior. Native `main` process codes only test small
positive results.

### Tests

`ctest --test-dir build --output-on-failure` checks valid/invalid syntax,
LLVM IR fragments and, if Clang is available, native compilation + execution.
A workflow failure with zero allocated runner steps is not an executed test.
Record both source revisions in differential fixtures; claim verified parity for a feature only after matched behavior and negative cases pass.

Next priorities: imported Java lexer golden cases, unambiguous checked integer semantics,
structured control flow, typed return analysis, precise ownership and actor ABI,
before introducing scheduler or pinned-frame execution.


## Executed LLVM i64-only module contract slice (2026-10-10)

The native parser now admits **same-source, compile-time** `define contract`
declarations with public module `conforms` constraints in the existing
`int`-only function subset. The front-end rejects missing or private required
functions, wrong argument arity, mismatched borrowed-value/exclusive argument
modes, duplicate contract methods, duplicate conformance declarations,
and unknown contracts **before** emitting LLVM IR. The compiled contract is
type-only: no actors, heap-allocated interface objects, dynamic method tables,
runtime reflection or implicit ownership conversion are added.

The first private-funded native proof ran the real C++ compiler against LLVM
17.0.6 and Clang: the positive fixture generated verified LLVM IR, and an
explicit test-only C shim invoked the module-qualified `Math.main` code to
verify the actual process exit result `42`. Eight negative fixtures rejected
the source with no emitted LLVM IR. All **18 exact source Git blobs** were
validated before building:
[funded LLVM native conformance](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38102461292).

The public `RouteContract.ores` source is pinned here as a negative test with
the reviewed Git blob `5bfd8552d4c6cc5bd369f43d81a7004ba01c2d45`.
It contains `String` request/response signatures, which are **not** supported
by this LLVM i64-only compiler. Importing a contract from another source unit
also deliberately fails closed because there is not yet a matching native
cross-unit import graph and ownership-aware type environment.

**Keep this feature's parity gate open until end-to-end behavior is verified.** Next LLVM work must add a
typed source graph, full `String`/record ABI and safe memory ownership,
cross-file contract resolution, JVM/LLVM shared conformance fixtures,
native actor/HTTP transport execution, and full CTest/ASan gates. No native
SDK or executable release is admitted by this focused test.
