# Pipe actor audit and hardening

Audited compiler admission, directional dispatch, scoped state, response admission,
callback ABI generation, cancellation ordering, and the Java fail-closed boundary.

## Findings fixed

1. Actor expressions could call main(), including through intermediate functions,
   and re-enter host-owned actor lifecycle code. Compiler call-graph analysis now
   rejects direct and transitive lifecycle-entry calls from actor methods and
   route payloads. Arithmetic helpers remain permitted and execute in a native
   regression fixture.
2. An absent DATA context returned ingress 0, also used for legitimate source
   input. Ingress now returns UINT64_MAX outside a live, registry-pinned pipeline
   DATA turn or after terminal failure. A real generated source callback is
   invoked with an intact heap turn but no DATA authority; it rejects before
   decoding a NULL packet or mutating state.
3. Response admission previously checked only TLS role/direction and duplicate
   response state. It now validates the current flow, message, source identity,
   registry membership, terminal phase, and failure state under the same mutex
   as cancellation. A testing-only hook cancels immediately before admission:
   cancellation wins, the response is not accepted, and normal teardown completes.
   The production object contains no testing hook.
4. A legacy chain's compatibility check could be relaxed by an unrelated unused
   transform declaration anywhere in the program. It now inspects only connected
   stages. A rejection fixture pins this noninterference property.
5. Unbound directional actor declarations lacked their own nominal union metadata.
   Their callback ABI now gets the complete forward/backward case table even when
   no pipeline instantiates them. The native unauthenticated-entry test exercises
   that generated ABI directly without conferring DATA authority.

## Verification

- 86 native compiler/runtime checks passed, including the six new audit checks,
  all three fifteen-stage spellings, explicit completion, overlapping domains,
  state loans, effect permissions, cancellation, and correlated-flow isolation.
- 13 Java role/namespace contract checks passed. Java continues to reject
  directional/state execution before lowering; this audit does not claim parity.
- Apple clang ASan/UBSan passed ten runs each for unbound ingress, cancellation
  before response admission, and the overlapping-domain fifteen-stage fixture.
- Production SourceActorBootstrap.c compiled without the testing macro; symbol
  inspection confirmed the response fault-injection hook is absent.
- Historical pipeline fixtures were migrated to scoped state callbacks and
  explicit effect declarations so their assertions reach the intended checks.
  A semicolon in a CMake diagnostic argument was shortened to its unique prefix
  so the test runner no longer splits the diagnostic into stray arguments.

The supported native protocol remains one bounded 2–16 stage round trip. General
0..N streams, concurrent guest flows, Future admission/backpressure, transport
adapters, first-class ActorStream factories, and Java execution remain outside
these changes. This is a focused source/runtime audit, not a formal proof.
