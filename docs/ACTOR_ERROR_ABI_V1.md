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

