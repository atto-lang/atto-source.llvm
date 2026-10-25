# Critical handle_state and using — shared Java/Graal and LLVM language contract

Status: **static/lower-level parity in progress; NOT executable guest actor-source
parity**. Both compilers must eventually accept the same source programs, infer
the same callback effects and enforce the same scheduler/borrow rules.

## Selected language contract

```ores
define actor Counter as
    state { count: int = 0; }

    pub increment(): int using state.read, state.write {
        return self.handle_state(critical |s| -> {
            s.count = s.count + 1;
            return s.count;
        });
    }

    pub read(): int using state.read {
        return self.handle_state(critical |s| -> s.count);
    }
end
```

- `handle_state` is the **sealed, runtime-defined, intrinsically critical**
  function. User code may NOT define or override it. No redundant
  `critical self.handle_state(...)` prefix is required; the *intrinsic*
  itself is already marked critical in the compiler's function/effect metadata.
- The sole argument **MUST** be an **inline** `critical |state| -> ...`
  with exactly one state-access parameter,
  lexical actor affinity and no capability escape. Unmarked `|s| -> ...`
  MUST fail compilation. An unrelated `critical` lambda does not thereby
  acquire actor-state authority.
- `using` is a **reserved signature keyword**, not a reusable variable.
  It declares an **upper bound** on the effects performed by the function,
  including the invoked critical callback. A read callback requires
  `state.read` only; a write requires `state.write` (and `state.read`
  when it reads). Do not automatically add state.write for read-only
  callbacks. `using` does not grant a capability.
- The callback inherits the enclosing effect-row upper bound, and its
  transitive effects must be checked; the runtime intrinsic is
  *effect-polymorphic*. The compiler must preserve the distinction between
  `throw`, `raise`, `panic`, `io`, `suspend`, `block`, `ffi` and
  `alloc`. Unverified external calls must fail closed under an explicit
  `using` bound.
- `critical` is a scheduler-owned logical single-flight **admission
  property**, **not** a native mutex held across I/O. Same-continuation
  synchronous nested critical callbacks are permitted; an inner scoped
  state loan fences the outer loan while active.
- The callback may perform I/O, `await`, `select`, or an explicitly
  declared blocking call, **if** its `using` row allows the corresponding
  effects and the target profile admits the operation. A suspension releases
  the logical lane. State-backed mutable borrows are retired/fenced before
  suspension; after resumption the continuation must reacquire a
  generation-checked scheduler ticket and fresh valid state access.
  Callback-wide atomicity **does not** span an `await` that yields.
- `requires bounded, no_alloc, no_block` is an independent verified
  real-time guarantee, not an effect or magic annotation. Unsound admission
  without target proof must fail closed. Do not weaken existing `pure`,
  `nlex`, `trap`, cancellation or supervisor-only PANIC contracts.

## Required compile-negative cases

1. `self.handle_state(|s| -> s.count)`: missing critical callback.
2. `self.handle_state(critical || -> ...)`: missing state parameter.
3. `self.handle_state(critical |a,b| -> ...)`: extra state parameter.
4. `self.state.count` / returning or capturing `s`: state capability escape.
5. `using state.read` but `s.count = ...`: understated effect row.
6. Duplicate/unknown `using` entries and variables named `using` or
   `critical`: reserved/invalid.
7. `await` with live mutable state loan or without `using suspend`:
   no unverifiable readmission.

## Implementation and parity gate

- Java/Graal source branch extends `LambdaExpr` with a `critical` flag,
  parses `critical |s| -> ...` and `critical (s) -> ...`, and enforces
  it at the sealed actor-state verifier, type-checker and conservative
  declared-effect checker. `critical` and `using` are lexer-reserved.
  Existing runtime-backed callback/continuation lowering is **not** implied.
- LLVM native branch provides a typed `CriticalHandleStateContract`
  admission policy and tests effects, critical flags, and suspended-loan
  fencing. Its current small i64 compiler still **rejects** actor
  `state` declarations; host policy conformance is not LLVM guest lowering.
  LLVM reserves `critical` and `using` in its frontend. Exact shared
  guest fixtures and full native compiler execution remain release gates.
- Runtime parity requires generation-fenced scheduling, continuation-aware
  state loan proof, a single-flight parent/actor execution lane, and
  identical emitted source-level behavior across Graal and LLVM. Neither
  backend should claim this is complete on the basis of parser/ABI tests.

This contract intentionally does not change actor `on_start`, `tear_down`,
`on_throw`/`on_raise`, supervisor-only PANIC or response-output
capabilities. HTTP SSR and JSON writers are I/O effects and cannot inherit
state loan authority or use an untrusted raw socket/FD.


## Post-audit hardening (2026-10-11)

1. **Write-only state effects:** `s.count = 42` has `state.write` only;
   `s.count = s.count + 1` also has `state.read`. Java's
   `DeclaredEffectVerifier` now distinguishes these; exact-source smoke
   covers both. Other backends must preserve this same distinction.
2. **Java host state-loan escape:** `ActorPersistentI64Map.critical`
   rejects a returned `Access`, nested collection/array wrapping a loan,
   deferred callable, future or unverified mutable holder. Result traversal
   has a bounded node/depth budget to keep critical execution finite.
   The *source* compiler must still reject side-effect-based leakage such
   as storing a loan into an external capture. This does not imply FFM
   reclamation or source `handle_state` lowering.
3. **LLVM state-loan scope bridge:** `CriticalStateLoan.hpp` binds the
   already compiled `CriticalHandleStateContract` effect admission to the
   existing `ActorStateScope.h` real nested loan tokens. RAII unwinds on
   exception. A scheduler-side `ores_actor_state_ready_to_suspend` denies
   suspension with any live state-backed loan; different continuations may
   not enter an active outer scope. IDs and generations must come from a
   trusted scheduler: the C ABI does not mint unforgeable tickets.
4. **Fail closed while lowering is incomplete:** Java's
   `OresEvalRootNode.initializeActor` refuses to materialize a source
   `state` declaration as an ordinary `OresObject` field. LLVM's native
   compiler likewise rejects actor state source before executable lowering.
   Neither of these guards is a claim that guest state source runs.
5. **Verification:** funded exact-source tests pass for Java parser/effects
   and host critical map against a deliberately mocked deterministic arena,
   and for LLVM native state-loan admission under GCC + Clang sanitizers:
   https://github.com/akrion-sim/akrion-sim-e2e/actions/runs/38168319428
   Full Java `TypeChecker`, native GraalVM memory arena, LLVM native source
   codegen, live scheduling, `await` lowering, and true end-to-end cross
   backend guest tests are separate release gates. No claim of parity for
   these unimplemented stages.

### Acceptance remaining

- Lower source actor `state` into runtime-owned generations, never a normal
  mutable object field, on *both* backends.
- Type-check effect-polymorphic `critical` inline lambda invocation; prove
  that no state-backed alias can escape to closures, FFI or `await`.
- Give `using` callbacks transitive effect rows across imported and
  dynamic calls, matching `pure`, `nlex`, `trap` and exception effects.
- Resume suspended critical callbacks only with scheduler-approved fresh
  generation-fenced loans and no OS mutex held across suspension.
- Run exactly the same source fixtures through Graal and LLVM on verified
  current HEADs; require full CI, native-image tests and concurrency fuzzing.


## October 11 executable scalar-state milestone

**Java/Graal source evaluator now has synchronous actor-state lowering.**
`OresEvalRootNode.initializeActor` stores declared typed state in a
`SealedActorState` separate from ordinary OresObject fields and initializes
defaults before on_start / READY. Inline `self.handle_state(critical |s| ->...)`
executes against a generation-checked ephemeral Access; nested synchronous
callbacks fence the outer loan. Scalar callback returns cannot leak Access.
ActorRuntime retires this storage after all actor turns quiesce.

Both backends' scalar state substrates support int/bool/string values with
finite schemas (1–256 keys), 64 KiB UTF-8 strings per key and 1 MiB aggregate
string bytes. Java has a source evaluator implementation; LLVM's
`ActorOwnedScalarState.hpp` is a real native runtime using
`CriticalStateLoan` and strict effect admission, **not yet guest LLVM codegen**.

Java TypeChecker now infers concrete results from expression-bodied callbacks
and blocks with explicit final returns instead of silently propagating Unknown.
More complex control-flow return joins, arbitrary structs/maps, transitive
effect inference and compiled source continuation lowering remain unavailable.
In particular, critical handle_state on a suspendable Java source task is
explicitly rejected; no OS mutex is retained across await.

The exact shared `tests/fixtures/critical-state-counter-runtime.ores`
declares an actor processing three messages, expecting output 1, 2, 3.
The Java parser/typechecker gate accepts it; the LLVM narrow parser
intentionally rejects it at the precise unsupported actor-state lowering
boundary. Native C++/Java deterministic actor-turn scalar tests are not a
substitute for the full Java/Graal JUnit + native-image test or LLVM guest
codegen test. Both are required before executable backend parity is claimed.

Release gates: full Java/Graal guest execution CI; LLVM actor state source
lowering; scheduler-owned await/loan retirement and continuation ID proof;
live restart/cancel/panic/teardown races; FFM/native arena budget accounting;
identical actor source/output/failure fixtures through both backends.


## Serial actors: implicit critical callback admission (2026-10-11)

`serial` is an actor-wide **scheduling** attribute, orthogonal to actor kind
(shared/private/untrusted), not a synonym for the `critical` keyword:

```ores
define serial actor Counter as
    state { count: int = 0; }
    pub increment(): int using state.read, state.write {
        return self.handle_state(|s| -> {
            s.count = s.count + 1;
            return s.count;
        });
    }
end
```

The same serial actor may explicitly write
`self.handle_state(critical |s| -> ...)`. The sealed `handle_state`
intrinsic still owns state admission, loan fencing, no-escape rules, and
the `using` effect upper bound. A **non-serial** actor still MUST supply an
explicit `critical` inline callback. An unmarked lambda is not generally
critical and cannot be smuggled through an arbitrary higher-order call.

The Java AST retains a `serial` boolean independently from
`Ast.ActorKind`, preserving it through annotation expansion. Java lexer,
parser, state-checker, effect verifier, TypeChecker, and the synchronous
source evaluator accept the scoped omission. Java accepts both
`define serial actor Counter as ... end` and
`define serial actor Counter as { ... }`.
The LLVM native admission ABI receives serial proof from trusted actor
runtime storage, NEVER from a callback-supplied Boolean. LLVM's restricted
native source compiler reserves `serial` and rejects the guest form until
real source actor+transport lowering exists.

**Serialization rule:** a serial actor has one actor-wide admission lane
for *both* HTTP requests and actor messages. Only one active user request
or message handler can exist. After `await` or I/O yields a carrier, the
logical handler still owns the lane until completion, failure,
cancellation, or finalization. No subsequent `receive()` or HTTP handler
may start until that completion. Supervisory CONTROL may interrupt safely
without admitting another user handler, and all continuations must resume
on scheduler-issued generation-fenced tickets.

**Important integration gap:** Java's existing source actor mailbox already
suppresses later user mail while a source receive is suspended. Its HTTP
transport is not yet proven to route requests through that same per-actor
admission lane. The serial metadata/callback waiver and native ABI tests do
**not** by themselves establish HTTP + mailbox single-flight behavior.
Until ingress adapters are integrated and cross-protocol concurrency tests
pass, do not claim complete runtime serial guarantees or LLVM executable
guest-source parity.


### Serial critical-state audit: fail-closed boundaries (2026-10-11)

- **Instance-method privilege only.** `serial` waives the lexical
  `critical` marker solely within instance methods on a serial actor.
  Static functions, ordinary actors and free functions retain explicit
  critical admission; `using` effect summaries do not infer authority.
- **Single aggregate.** Serial actors may have no stored fields, or one
  sealed `state { ... }` aggregate; legacy actor scalar fields and a
  second state store are rejected. Explicit `extends` on serial actors
  is rejected for now: inherited mutable state and scheduler-lane
  ownership have not been proven. `from`/`implements` interfaces are
  unaffected.
- **Immutable scalar results.** Java `SealedActorState` no longer admits
  arbitrary `Number` subclasses or `Enum` objects as safe callback
  results; user-defined mutable `Number` wrappers and atomic counters
  could otherwise bypass the fail-closed return boundary. Accepted
  numbers are precisely primitive boxed numeric types and immutable
  BigInteger/BigDecimal.
- **LLVM serial admission cannot bypass safety checks.** Funded tests
  exercise missing intrinsic criticality, missing state parameter,
  non-lexical callbacks, escaped loans and insufficient `using`
  declarations even when runtime-provided serial status is true.
- **Verification:** [funded run #38176946646](https://github.com/akrion-sim/akrion-sim-e2e/actions/runs/38176946646)
  compiled real mirrored Java parser, TypeChecker/OwnershipChecker and
  effects admission; Java state runtime tests with a deterministic
  test-only actor-turn harness, native parser fail-closed assertions and
  GCC/Clang ASan/UBSan admission/loan tests **passed**. All 32 pinned
  source/fixture Git object checks passed. Full source Maven/JUnit,
  native-image, LLVM CMake/CTest and unified HTTP/mail lane runtime
  integration are **still not verified**. Upstream source Actions jobs
  have failed with no runner steps; the mirror's unrelated
  secrets-audit has also failed. Do not mark PRs merge-ready.


## Verified Java serial mailbox + HTTP transport integration (2026-10-11)

**This milestone supersedes the earlier "HTTP ingress not yet wired" Java
prototype caveat above.** Java/GraalVM now uses the same persistent actor
mailbox for every request dispatched through a *single*
`http.handler("Counter")` whose target declaration is `serial actor Counter`,
and for ordinary messages delivered using supervisor
`handler.send("tick")`. No second handler for that actor instance may enter
while a user receive is logically active. The handler's persistent actor
instance retains the one state aggregate for its lifetime.

The underlying implementation uses scheduler/continuation mail, **not an OS
mutex around guest code**. `ActorRuntime.suspendCurrentUserMailboxUntil`
registers independent completion barriers, preserving one admitted user
envelope while its method continuation or its HTTP exchange is incomplete.
Only when every barrier has settled can the next user message/HTTP envelope
be admitted. Continuation mail retains priority to make progress.
Nested synchronous actor calls execute on their existing turn; they do not
create a new user admission. State loans must still retire before await.

Per-request HTTP timeout, abort, and client disconnect never cancel the
shared persistent actor. A request that expires *before* reaching
`receive()` is consumed as an authenticated transport tombstone, not sent
to `http.claim`; teardown fences stale completion callbacks and releases
any unclaimed transfer tickets. Server and HTTP context shutdown cancel
persistent actors; normal `stop`/supervisor cancellation still terminate
the actor and drain mail.

The serial actor supervisor transport API is
`http.handler("Counter").dispatch(rt borrow mut request)` together with
`handler.send("tick")`. Its `send` operation only addresses that handler's
persistent serial actor and cannot be invoked from an actor turn. The
transport neither exports a freely castable guest actor pointer nor claims
that two independent `http.handler(...)` registrations identify the same
actor instance. A separate global/registry actor-name identity feature would
need its own admission and lifecycle contract.

**Executed Java/GraalVM validation:**
[funded source-mirror CI #38181207023](https://github.com/zed-pkg-test/java-app/actions/runs/38181207023)
verified **403 exact Git source blobs** for the Java PR, passed
the 15 focused actor-state/continuation tests and 7 HTTP guest integration
tests, and then completed full Maven `verify`: **1,418 tests,
0 failures, 0 errors, 1 skipped**. The serial HTTP test exercised two
responses and a queued non-HTTP actor message against one persistent
stateful actor and checked FIFO counter values 1, 2, 3.
`SerialActorMailboxBarrierTest` additionally verified that one completed
Future does not open the lane while another remains pending, and that
cancellation cannot revive user mail after actor teardown.

**Remaining parity and rollout constraints:** LLVM has native scalar
state/critical admission primitives but **still rejects executable guest
`serial actor` syntax**, because full source lowering and HTTP/mail
integration are not implemented. Do not claim LLVM parity on the basis of
this Java run. Native-image/AOT, stress/fuzz testing with concurrent
HTTP requests, restart/interleaving under real network failures, and
cross-backend executable fixtures remain release gates. Keep both PRs draft
until corresponding backend and dependency gates are complete.
