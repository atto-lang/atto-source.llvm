# Native transform pipe actor: executable status (2026-10-12)

## Implemented restricted LLVM profile

`define transform pipe actor Stage<forward_messages=F,backward_messages=B> as ... end`
declares a compiler-enforced actor role. Both input domains must name
existing closed nominal match-pattern or enum domains. The actor must define
exactly one `receive_forward(F m): void` and one
`receive_backward(B m): void`; neither generic `receive(Message m)` nor
scalar returns satisfy the contract. Compiler checks exhaustive tagged arms
within the *individual direction's domain* and rejects output cases outside
the active handler's domain.

The compiler generates two separate methods and a single runtime-facing
callback wrapper. The wrapper dispatches using the trusted ingress direction
supplied by `SourceActorBootstrap` from the scheduler's admitted DATA turn:
1 forwards from upstream; 2 backwards from downstream. Guest payload and
caller-supplied flags cannot set ingress authority. The two hooks share the
actor's one scheduler lane and state heap; no second carrier or mailbox.

For the *restricted* linear single-flow native implementation, the compiler
constructs a sorted union of every linked pipeline stage's nominal enum
cases. The union is a single identical transport schema table for all
stages, avoiding ordinal collisions between F and B. Within each handler,
the compiler validates the narrower declared domain. This internal shared
pipeline fingerprint is not a claim of independently verified GraalVM wire
parity or public message-schema evolution.

## Actual execution

`tests/fixtures/source-actor-pipeline-fifteen-{dot,colon,operator}.ores`
now compile thirteen explicit transform middle stages and ordinary source/sink
actors. The initial input is 1; 14 forward and 14 backward hops produce 30.
The native compiler's IR is compiled and run by Clang on a real GitHub Actions
runner. Each of three pipe spellings is executed five times. Seven malformed
role/signature fixtures must fail before LLVM IR.

Exact-source private funded validation:
https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38296833465

The job verified 34 Git blob identities, built the real LLVM frontend and
passed all ten selected CTests (3 execution gates x 5 native runs,
7 negative gates). The source organization's Actions are unable to allocate
a runner (runner_id=0), so no source-org green status is claimed.

## Not yet supported

- The combined `state { value: int = 0; }` and scoped
  `self.handle_state(critical |state| -> { ... })` example is **not**
  lowered by this restricted LLVM bootstrap compiler. Executable fixtures
  currently use actor-owned scalar `let` fields. Do not treat the proposal
  in `TRANSFORM_PIPE_ACTOR_PROPOSAL.ores` as runnable yet.
- This implementation only supports one nominal ingress/output domain per
  direction, not distinct forward-in/forward-out/backward-in/backward-out
  ports; four-port transformations remain to be specified.
- The Java/GraalVM source actor scheduler still requires the ordinary
  `receive(ActorMail<T>)` entry point. Java guest directional dispatch and
  full source-level runtime parity have not been established.
- Bounded one-flow synchronous native output is not general guest async
  `Future<T>` backpressure, arbitrary streams, correlated responses, or
  a guarantee that state loans can cross an `await` boundary.
- Keep the PR in draft until Java compilation/dispatch, scoped state lowering,
  release CI and cross-backend semantic conformance are verified.
