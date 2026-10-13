# Explicit native pipe completion

The current LLVM branch combines the existing directional transform and scoped
actor-state implementation with explicit `source pipe actor` and `sink pipe actor`
endpoints. Transform actors require both typed directional handlers. Source actors
handle forward external input and backward results. Sink actors handle forward
input and emit into the declared backward domain. Every new-role handler returns
void. Source completion is explicit `self.respond(Signal.Result(next))`.

State callbacks follow the current contract: signatures declare `with state.read,
state.write`, and the actual mutable loan is `self.handle_state(critical |state| ->
{ ... })`. A scalar `next` leaves the callback; the state view does not. Existing
`using state.read/write` checks are preserved. State remains a fixed set of typed
private keys in the owning actor heap.

Tests execute a source, thirteen transforms, and a sink through all three topology
spellings: `ActorUtils.pipe(...)`, `ActorUtils::pipe(...)`, and `... pipe ...`.
Starting from 1, each forward and backward stage increments by 1: the forward
peak is 16 and final result is 30, with fourteen hops in each direction.
A second test uses `Signal.Data` for both directional domains and increments by 2
on the backward path; it produces 44. Dispatch therefore follows scheduler-owned
ingress, not a payload tag. A missing explicit response fails even when main
expects zero, so the internal callback's scalar observation ABI cannot silently
stand in for an implicit reply.

The bounded native bootstrap still owns a 2–16 stage, single-flow round trip in
`main`, with one send per route leg. General 0..N streaming responses, concurrent
flows, Future-based emission backpressure, transport-specific adapters and
first-class ActorStream factories remain separate work. This test protocol does
not establish a general one-response-per-request actor contract.

Java's current branch parses and verifies transform role/state shapes but rejects
execution until directional DATA and scoped state lowering exist. The current
Java role and namespace regression suites pass; that is compiler admission and
rejection evidence, not Java pipeline execution evidence.

The earlier locally tested implementation is preserved on
`backup/local-directional-20261013`. This change is based on the newer shared
`feat/actor-utils-pipe-static-syntax` branch, preserving its unified state/effect
checks rather than replacing them with that earlier prototype.
