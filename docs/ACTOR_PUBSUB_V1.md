# Actor Pub/Sub v1 parity target

LLVM: NativeActorMembershipRegistry is the current host-side membership substrate. This contract does not claim guest ActorGroup/Topic lowering yet; unsupported source constructs must reject before IR emission. Parity requires generation-fenced memberships, bounded fanout admission, actor-retirement cleanup, and native execution of the shared conformance cases.

Normative contract: `contracts/v1/actor-pubsub-v1.json`.

No `define publisher` or `define subscriber` language forms are introduced. The shared source model is `ActorGroup`, `Topic<T>`, `Subscription<T>`, and `PublishReceipt`; actors may belong to multiple groups simultaneously, and group membership is never lifecycle ownership.

Golden source parity fixture: `contracts/v1/fixtures/actor-pubsub-v1.ores`.
The backend must either execute this fixture with the normative output or reject the unsupported source surface before lowering; host-only scaffolding is not parity evidence.
