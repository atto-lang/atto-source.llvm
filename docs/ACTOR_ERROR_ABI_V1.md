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
- `offer(notice, direct_child_id)` requires a trusted runtime-supplied structured
  child identity (not an identity copied from the notice) before acquiring the
  mutex. The host API rejects self-parenting and mismatched child authority,
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
