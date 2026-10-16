# Ownership boundaries: audit and admission contract

Status (2026-10-12): required safety checks with backend-specific admission.
This is not a claim of complete lifetime analysis or cross-backend parity.

## Borrow and suspension boundaries

A synchronous call's temporary receiver and argument loans remain live until
all arguments have evaluated and the call completes. Nested evaluation of a
later argument cannot move, mutate incompatibly, or suspend with an earlier
loan active. Check a suspending callee at invocation, after argument loans have
been acquired, as well as checking any pre-existing loans before evaluation.
Apply this rule to direct, qualified, static, instance and first-class calls.

An `async`/generator activation must own its retained values, or have a proven
outlives/continuation contract. A call-site lexical borrow is insufficient.
A newly constructed borrowed yield/result is subject to the same no-escape
rules as a stored borrowed local. Runtime-managed reachability does not grant
ownership or mutable access.

Blocking `await`, channel operations and select without a default are
suspension boundaries. Immediate select and blocking select with a default do
not inherently suspend; operands and arm bodies can still contain independent
suspending operations. Deferred nonblocking callbacks must own/copy/share their
captures; registering a callback does not extend an ordinary lexical borrow.

A sealed critical `handle_state` callback retains its actor-state loan until
callback exit. It cannot cooperate, await, or block on channel/select operations
while the loan is live. Releasing a carrier does not prove the state loan safe.
This synchronous restriction applies to explicit critical callbacks and any
serial-actor implicit critical admission. More general logical-ticket critical
scopes require separate continuation, cancellation and ownership proofs.

## Runtime ownership operations

| Operation | Required admission |
| --- | --- |
| `rt borrow` / `rt borrow mut` | Rooted shared/exclusive loan; no ownership upgrade, untracked escape or suspension |
| `rt take` | Transfer a live owner; reject borrowed inputs and conflicting loans; projected move-only extraction needs place/partial-move/drop tracking |
| `rt copy` | Primitive/Copy behavior or a verified copy protocol; no shallow mutable alias disguised as a new owner |
| `rt share` | Existing safe Copy handle or a verified owning shared capability; no manufacturing of shared mutable aliases |

Java/Graal main currently fails closed for move-only `rt copy` and `rt share`.
This audit does not invent their missing protocol/shared-handle implementations.
LLVM's admitted native scalar and bounded class subset has separate place/drop
tracking. Unsupported forms must fail without emitting executable IR.

## `owned`, `using` and `critical`

Ownership is implicit for by-value source bindings. `owned` remains an ordinary
identifier; it is not a public storage/type keyword. LLVM's experimental
`owned int` qualifier requires its explicit test flag and is not public parity.
See [ownership syntax](OWNERSHIP_SYNTAX.md).

`using` is an upper bound on observable effects. `requires no_alloc/no_block`
is an independent guarantee and must be verified even without a `using`
clause. Aggregate and escaping closure construction contribute allocation;
constructing a closure does not execute its body. Unknown calls cannot establish
these guarantees without trusted summaries. Source-level effect admission does
not certify physical allocation rates or worst-case execution time.

`critical` grants only its specified scoped state/scheduler authority. It does
not grant copyability, escape permission, or suspension-safe lifetimes.

## Release evidence

Require positive and negative tests for loans spanning later arguments,
suspending call invocation, yielded borrows, selected/deferred captures,
projected takes, and requirements without an effect clause. Record exact backend
commits and fixture identities. Separate unsupported admission, executed guest
behavior, host-only tests and toolchain failures. Pending feature branches must
not be described as behavior already shipped on main.

The organization currently has independent Java/Graal and LLVM checkers; the
common-source and platform-conformance repositories define contracts and harnesses.
A pass in one implementation does not establish the other's safety.

## Expanded 2026-10-12 adversarial matrix and release gates

The Java/Graal compiler candidate [ownership PR #740](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/740)
contains `OwnershipFiftyProgramBoundaryMatrixTest`: 50 distinct Oreslang
programs covering five callable kinds, `await`, `rt cooperate`,
blocking `readch`/`writech`, static/dynamic `select`, synchronous and
asynchronous generator `yield`, nonblocking registration, ending lexical
loans, and `rt copy`/`rt share`/`rt take`. Each negative requires
successful parsing first and the intended ownership diagnostic; positive
fixtures must be admitted.

[Critical/effects PR #739](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/739)
adds 12 distinct parser-checked effect programs. Distinguish **suspension**
from **potential waiting**: a blocking channel read/write or static select
without a default must contribute `block` as well as `suspend`. Nonblocking
registrations, and a static select with an unconditional default arm, do not
contribute `block` merely by their syntax. Dynamic blocking select is
conservatively treated as potentially waiting. `requires no_block` must
reject potential blocking even without `using`; `using suspend` is not
permission to block. A `_blocking` suffix is a naming convention, not a
trusted, independently verified effect summary.

**Do not call this a data-race-freedom certificate.** Compiler admission
alone cannot prove that FFI/native aliases, unsafe host callbacks, actor
shared-state handles, generated continuation frames, and the underlying
scheduler preserve exclusivity. A complete claim also needs a sound
interprocedural alias/effect analysis, drop and cancellation proof, runtime
object-graph isolation, and executable cross-backend checks.

Reduction-budget/reentrancy obligations for LLVM and GraalVM:

1. Every potentially unbounded guest execution path must reach an **actual**
   continuation-lowered, yield-safe scheduler boundary within the configured
   work budget, or fail admission; ordinary safepoint polling is not preemption.
2. The live-loan set at each boundary must be empty of conflicting state and
   native/FFM loans; permitted continuation-owned locals must stay pinned to
   their saved lifetime/identity, even across carrier migration.
3. A budget-triggered yield must release the physical carrier only after
   continuation publication, preserve logical actor single-flight, and never
   replay completed external effects; cancellation must retire each loan once.
4. Untrusted actor fuel/CPU/wall budgets must remain monotonic across resume,
   not reset per carrier dispatch. Unknown `*_blocking` callees require a
   trusted effect summary or rejection, not optimistic assumption.
5. Run the same source through Java/GraalVM JVM, Native Image and LLVM guest
   execution, recording exact blob IDs, positive and negative diagnostics,
   sanitizer evidence, and deterministic preemption/interleaving schedules.
   LLVM fail-closed unsupported-channel diagnostics are useful but are **not**
   proof of LLVM execution parity.

The new source-branch Actions jobs have not yet produced executable results.
They must be run on an authorized private runner/mirror before any of these
new tests can be marked passed. Do not treat pre-runner failures as failures
of Oreslang programs, nor publish private source into a public mirror.

## Follow-up security audit: delegated yields and deferred cleanup

A generator `yield*` delegates values carried by an iterator handle. It is
**not** sufficient to check that the iterator itself is owned: its element
type must also be free from borrowed components, recursively through generic
wrappers such as `Option<borrow T>`. Until explicit generator yield lifetimes
are implemented, the Java checker rejects such escapes. Keep a positive
`Iterator<int>` delegation control so safety checks do not silently prohibit
ordinary delegation.

A `defer` registration has a different effect meaning from merely constructing
an ordinary closure. The cleanup body executes on function exit, including
failure paths, so `using` and `requires` checks must include the deferred
body's potential blocking, allocating, IO, and suspension effects. Treat an
unknown deferred callee as unverified, rather than eliding it. The Java effect
PR now checks this explicitly.

The Java ownership PR also adds a bounded-exhaustive model that combines
ordinary/state/native loans with reduction charges, actual carrier handoff,
fuel continuity, cancellation, and effect replay fences. The model includes
an unsound mutation (permit yielding while any loan remains) that must be
detected. **This is a model test, not an LLVM or GraalVM runtime preemption
proof.** Source-level actor loop code generation, unknown blocking FFI, and
state-loan capability resumption remain separate implementation gates.

## Further hardening: borrowing must survive representation changes

The Java checker now rejects `as?` on a borrowed value because
`Option<Owned>` would otherwise discard the lease. Checked `as T` cannot
be stored under an owned binding when its runtime source remains borrowed;
this includes explicitly declared `T` bindings and inferred declarations.
Conditional expressions merging borrowed and owned branches, or joining
borrowed paths without trackable provenance, also fail closed until
path-dependent borrow sets/lifetimes exist. Genuine owned casts and Copy-only
`rt copy`/`rt share` operations remain legal positive controls.

Cross-branch integration must run the full type-checker, not only the standalone
`OwnershipChecker`: the critical/effect checker branch is stacked over the
feature baseline, whereas the ownership matrix is based on main. The exact
combined Git tree and guest runtime must be validated before either branch is
treated as cross-feature-safe.

## Post-audit 2026-10-12 closure: operands and synchronous sinks

The Java/GraalVM hardening branch now rejects a newly constructed
`rt borrow` used as the operand of `await`, channel read/write, a static
select arm, or dynamic select-plan registration. Checking existing lexical
loans before operand evaluation is insufficient: the operand may itself
produce a temporary borrowed view. The compiler must check both the
pre-evaluation live loans and the evaluated value's ownership provenance
before any blocking operation or deferred registration. These are
**compiler-pass regressions** until funded CI confirms them; they are not
LLVM runtime parity.

A synchronous `stdio.print/println` is potentially blocking, even though
its observable effect also includes `io`. `requires no_block` must reject
these operations absent a separately verified nonblocking implementation.
Explicit `using io` does not implicitly grant `block`: an admitted
unrestricted console-writing function needs both effects. Any direct
`stdio.*` call in a sealed synchronous critical state callback is
conservatively rejected while its actor-state loan remains active. Future
verified off-carrier I/O requires a distinct effect summary and loan-release
proof; naming a function `*_blocking` supplies no evidence either way.

A full data-race-freedom claim remains blocked on live-source compiler
execution, native/FFM alias coverage, and reducer-inserted continuation
proofs. Existing source-repository Actions attempts terminated without test
steps; do not claim the new programs passed.
