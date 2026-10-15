# Actor error ABI v1 (GraalVM/LLVM)

The two backends carry the identical `actor_errors.ores` source declarations and
`actor-errors.csv` catalog. Numeric codes 1001–1020 are frozen; new codes must be
appended, not renumbered. These are **guest Error subclasses**, not host Java
Throwable/C++ exception subclasses. Parser acceptance and host catalog tests **do
not** establish executable guest exception lowering on LLVM.

## Supervisor and actor transport rules

- A supervisor receives a **copied, value-only envelope** (numeric code,
  bounded UTF-8 diagnostic, bounded opaque actor/supervisor identifiers).
- Never carry a live guest Error object, stack object, Java Throwable, C++
  exception_ptr, function, callback, actor heap pointer or mutable collection.
- Envelope diagnostics are bounded to 1024 UTF-8 bytes, malformed bytes
  are repaired, and terminal/bidi/zero-width controls are scrubbed. **This is
  not secret redaction**: the host constructor still accepts arbitrary text.
  Runtime callers must use a fixed, public-safe diagnostic (for example the
  frozen error key) instead of passing host exception messages, file paths,
  capability values, payloads, tokens or stack traces. A future trusted trace
  channel must enforce disclosure authority independently.
- Full traces require a trusted, explicitly requested diagnostic channel and
  must be generated on the originating actor before teardown. A guest-supplied
  `include_trace` request alone does not grant trace authority.
- `cancelled`, deadline, isolation, revoked capability, fencing and runtime
  invariants are control-plane terminals. They are not caught by `trap` and
  must not be converted to success by local `on_throw` / `on_raise` hooks.
- Recoverable send/mailbox/remote failures may be returned as typed
  `Result<_, Error>` by their APIs; they must not bypass admission/ownership.
- Supervisor priority remains host fatal/process exit > cancellation > panic
  > raise > throw; a lower-priority cleanup error cannot hide higher priority.

## Integration stages

1. **This change:** freeze guest declarations and code registry, expose
   bounded host-only error envelopes with direct JVM/native tests.
2. Wire source-level constructors, inheritance checks (only Error-derived
   values may throw/raise/panic) and typed Result/serialization to both
   compilers, using real guest conformance fixtures.
3. Connect runtime send/spawn/stop/supervisor paths and validate all actor
   handoff points, including cancellation and teardown, with native Image and
   LLVM end-to-end evidence.

Do not infer stages 2–3 are complete from existence of these files.

## Bounded supervisor inbox contract (October 14, 2026)

The native host CONTROL boundary now includes an `ActorFailureControlInbox`
whose immutable owner is the runtime-chosen direct-parent identity. It holds
at most 256 value-only notices, rejects foreign-parent notices and anonymous
inbox owners, and exposes explicit overflow accounting. The C++ effect-policy
test covers ordering, capacity, draining, invalid identities and overflow.

**This is not native guest exception handling.** No LLVM actor scheduler yet
wires this host-only inbox into actor turns or source-language `on_raise`
handlers. The corresponding Java draft has a live runtime-owned parent inbox
and a restricted synchronous actor `throw`-to-`raise` boundary. No native
CMake/CTest, Java JUnit, or AOT runs have been verified successful at these
PR heads. Do not merge either PR on static headers or source admission alone.

## Current hard blockers (October 14, 2026)

- The Java/GraalVM draft branch now has a compiler-owned nominal `Error`
  root, restricted lexical guest `throw/catch`, and separate non-suspending
  actor `raise/panic` effects with immutable host-supervisor CONTROL queue
  delivery. The parent-actor recovery hooks and cross-backend execution
  tests remain unverified and gated.
- LLVM retains staged guest `Error` source declarations only. It has a
  **host-only** matching `ActorFailureControlNotice` schema with strict
  Raise/Panic tags and a fixed, value-only diagnostic. The native C++ guest
  parser still rejects inheritance and typed effects; this header does
  not constitute LLVM guest exception lowering.
- LLVM still compiles only a narrow native source subset: its class
  parser now rejects `Error` redeclaration and inherited classes with
  explicit diagnostics, rather than silently implying support. `src/core`
  remains staged Oreslang source, not a linked guest Error hierarchy.
  Negative native compiler admission tests and host-side effect-priority
  tests have been added, but not yet executed in CI.
- The host `recoverable` value is **classification metadata**, not a proof
  that the runtime correctly catches, escalates, retries or supervises it.
- The cross-backend static verifier `scripts/verify_actor_error_abi.py`
  freezes source/host names, codes and declared policy; it does not exercise
  actor execution or the wire serializer. Do not mark the PR production-ready
  until executable guest inheritance + Error-only throw/raise/panic + transport
  and supervisor tests pass on both backends.


## Host CONTROL allocation audit — October 14, 2026

- Replaced the supervisor inbox's dynamically growing `std::deque` with a
  fixed-capacity ring (`vector<optional<notice>>`) allocated in the constructor
  *after* validating that capacity is within 1–256.
- `offer(notice, permit)` requires a trusted runtime-issued structured
  child permit minted by `register_direct_child` (never a notice field)
  before acquiring the mutex. The host API rejects self-parenting,
  mismatched child authority, stale generations and foreign inbox incarnations,
  independently of the envelope's claimed parent. This does not yet constitute
  a native scheduler integration. It accepts a value copy and moves
  the owned value into an existing slot. It never grows a queue while the
  CONTROL lock is held. `try_pop` moves a complete notice out before releasing
  the lock. No guest object/callback enters either path.
- Added native C++ test coverage for FIFO ordering, ring wraparound,
  repeated reuse, full-capacity overflow and supervisor identity rejection.
- **Unverified:** These additions have not passed an exact-head CTest build;
  LLVM guest exception lowering and scheduler integration remain unimplemented.

## Supervisor inbox retirement and concurrent stress (October 14, 2026)

The host-only native `ActorFailureControlInbox` is now explicitly
retirable. Retirement is idempotent, serializes with `offer`/`try_pop`,
clears unread value-only notices and increments the durable per-inbox drop
counter for each orphaned notice. Later offers fail closed and count as
dropped instead of reviving a supervisor.

Added a new threaded CTest
`native_actor_failure_control_concurrent_fifo_retirement`, registered with
`Threads::Threads`. It exercises a single producer/consumer FIFO under
concurrent offers and pops, plus a producer racing irreversible retirement
with conservation of accepted/rejected/orphaned notices. The host-only
effect-policy test covers idempotence and post-retirement rejection.

This remains a **host C++ data-structure contract only**, not LLVM guest Error
construction, effects, scheduler integration or native actor restarts.
No exact-head CMake/CTest result is available from the failing Actions jobs.



## Bounded native suppressed-failure policy (October 14, 2026)

The native host `ExceptionPolicy::Failure` now retains at most **16
suppressed failure nodes** at a maximum depth of **8**, regardless of the
number of failed cleanup actions or priority transitions. Each individual
diagnostic retains its 1024-byte bound. Truncated metadata contributes to
`suppressed_dropped()` via saturating counters; terminal failure priority
is unchanged. This is a host-owned, value-only representation, not
source-level LLVM guest exception lowering or a trace-disclosure API.

The policy CTest adds a 4,096-cleanup stress sequence, checks total retained
nodes and nesting, validates cancellation still takes precedence, and
verifies counter arithmetic at the size_t overflow boundary. The companion
negative parser test now demands a successful LLVM IR positive control and
specific effect-rejection diagnostics, preventing an invalid executable or
broken parser from reporting a false pass. **Neither test has yet completed
in available CI.**

## Registered native direct-child authority (October 14, 2026)

The native host-only `ActorFailureControlInbox` now checks **three** independent
conditions before accepting a terminal child notice:

1. The envelope's supervisor matches the owning inbox.
2. The trusted registration permit's child identity matches the notice's actor ID.
3. That exact inbox incarnation + child generation is currently registered
   with the supervisor by the host actor spawn/finalization machinery.

`register_direct_child(id)` returns a host-only `optional<DirectChildPermit>`
from the trusted actor-spawn boundary. Guest mailboxes, source code, notice IDs
and untrusted `ActorRef` values must **never** register membership or receive
a permit. `unregister_direct_child(permit)` runs only after the final CONTROL
handoff using precisely that generation's permit. Revocation prevents new admissions, but
already queued immutable notices remain readable. A supervisor's retirement
clears the membership registry and ring atomically under the same mutex.

Registration cardinality is capped at 4,096 child identities per supervisor;
excess registrations fail closed. Registration may allocate at spawn, but
`offer` and `try_pop` do not grow queues or registries. Tests now require
explicit registration and cover forged, duplicate, withdrawn, and retired
child authority as well as producer/consumer/retirement races.

**Scope:** This adds an enforceable ownership contract to the native host
container. It is not wired into LLVM's actor scheduler and it does not
implement guest `throw`, `raise`, `panic`, unwinding, `recover`, or native
supervisor callbacks. No exact-head LLVM CTest/AOT results have been obtained.

## Actor-local hook-to-supervisor host conformance (October 14, 2026)

The `effect_supervisor_end_to_end_test.cpp` host fixture now composes
`dispatch_actor_local_unhandled`, `EffectSupervisorBridge`, and
`SupervisorControlTurn` in one test, rather than asserting only the isolated
policy calls:

- `on_throw` returning `Handled + Return` prevents terminal CONTROL
  publication and permits the host child reservation to retire normally.
- A callback returning `Escalate + Return` does **not** suppress the original
  Throw: the bridge promotes it to supervisor Panic with Throw provenance.
- An already-terminal Panic bypasses every actor-local `on_throw` callback.
- Panic inside the callback dominates its claimed `Handled` resolution.
- An unknown native `ActorLocalResolution` enum discriminant is converted to
  fixed Fatal code `0xFFFFFFFD`, never treated as successful recovery or
  silently downgraded to an ordinary throw.

This fixture is strictly **host-native conformance**, not linked Oreslang
source execution. The LLVM guest compiler still rejects actual source
`Error` inheritance, `throw`, `raise`, and `panic`. The Java backend
has introduced a narrower source actor `on_throw(Error): bool` receive-turn
slice; this does **not** establish cross-backend feature parity. Exact-head
CTest, sanitizers, native AOT and scheduler integration remain unverified.


## First-terminal-only native CONTROL admission (October 14, 2026)

The host-only `ActorFailureControlInbox` now tracks a terminal-admitted bit
for each **currently registered** direct-child identity under the same mutex
that protects the fixed-capacity ring. The first successful `offer` sets
that bit only after its value-only notice occupies a ring slot. A second offer
from that registration returns false even if the first notice has already
been drained. This prevents duplicate child supervision and replay-driven
restart decisions at the inbox boundary.

Duplicate rejections have a separately saturating
`duplicate_rejections()` counter; they are not misclassified as missing
first-notice delivery in `dropped()`. A full queue does **not** set the
terminal bit; a failed first attempt may be retried after capacity returns.
A retired supervisor still rejects late offers and counts discarded notices
under its previous retirement contract.

**Generational authority:** `register_direct_child` now mints a
`DirectChildPermit` with a globally monotonic inbox incarnation nonce and
strictly increasing per-inbox child generation. The old generation cannot
`offer` or `unregister` after its ID is reused; a token from a different
inbox cannot authorize publication even if its supervisor ID string is
identical. Counters refuse to wrap. The public string-only CONTROL-offer API
has been removed so a copied actor ID alone can never authorize the notice.

The future native scheduler must mint and retain permits exclusively in host
actor-spawn state, not guest data; it must pass the exact permit for terminal
handoff and revoke it after teardown. Host caller misuse of a fresh permit
with forged metadata remains outside the container's proof. The notice
retains value-only IDs and fixed diagnostics, never a token/guest object.

Native host tests now cover one successful terminal admission, duplicate
rejection before and after drain, saturation followed by a first-admission
retry, wraparound with fresh identities, and 16 simultaneous duplicate
producers. Earlier ring tests that reused the same child for multiple
terminal notices have been corrected.

**Still unverified:** exact-head CMake/CTest and sanitizers. GitHub Actions
reports failed jobs with zero executed steps, and job log downloads are
unavailable. This is not native LLVM guest lowering, source `on_raise`,
native supervisor scheduling, or production evidence.

## Side-effecting supervisor callback retry fence (2026-10-14)

`SupervisorControlTurn::admit_turn` distinguishes a failed decision
callback from an exception thrown after requesting a potentially
side-effecting restart or escalation. Decision dispatch failures **before**
a restart/escalation action remain retryable under the existing CONTROL lane
policy. Once `on_restart` has been invoked, a later exception must **not**
invoke the same restart callback again: a replacement incarnation may have
already been created. The runtime instead attempts `on_escalate` once if
escalation was not already attempted, retires the admitted terminal event,
and propagates the original host exception. If `on_escalate` itself throws
after producing a terminal host effect, the same event is not replayed.

`supervisor_control_turn_test.cpp` adds fault-injected callbacks that
simulate restart and escalation side effects followed by exceptions. It
verifies at-most-once callback invocation, terminal notice retirement,
no duplicate restart admission, and propagated exceptions.

This is host-only C++17 behavior, **not** native Oreslang guest exception
lowering or actual scheduler-driven actor restart. The focused exact-head
GitHub Actions workflow still reports failure with no executed job steps,
so the new fixture is not verified passing under CTest or production native
carriers. No private source was transferred to other GitHub organizations:
both source allowlists currently approve no destinations.


## Low-level CONTROL handler replay contract (October 15, 2026)

`SupervisorFailureLane::dispatch_one` is **at-most-once** after entering a
native host handler. A callback may have created a replacement actor, emitted
a fatal notice, or otherwise changed host state *before* it throws. Its
exception therefore retires that single terminal notice instead of placing
it back into the CONTROL queue; the original exception is propagated.

This favors **no duplicate side effects** over automatic redelivery for
transient callback failures. Hosts requiring durable failure processing must
handle rejection/escalation explicitly in their supervisor policy and maintain
their own durable host-side audit record. It is not an exactly-once processing
guarantee and not evidence that a parent handled the failure.

`SupervisorControlTurn` still owns its separate admission and decision policy:
its pre-decision hook failure is retryable; once a restart or escalation
callback has started, that path retires the notice to avoid effect replay.
The low-level `dispatch_one` must not be used to bypass the supervisor's
restart-intensity and generation checks.

Host regressions cover exceptions before any visible side effect and exceptions
*after* recording a callback side effect. In both cases there is no second
invocation of the terminal callback for the same child. Real native scheduler
integration, LLVM guest lowering, and exact-head CI execution are still pending.
