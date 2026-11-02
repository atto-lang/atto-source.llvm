# Actor Pub/Sub v1 parity target

LLVM: NativeActorMembershipRegistry is the current host-side membership substrate. This contract does not claim guest ActorGroup/Topic lowering yet; unsupported source constructs must reject before IR emission. Parity requires generation-fenced memberships, bounded fanout admission, actor-retirement cleanup, and native execution of the shared conformance cases.

Normative contract: `contracts/v1/actor-pubsub-v1.json`.

No `define publisher` or `define subscriber` language forms are introduced. The shared source model is `ActorGroup`, `Topic<T>`, `Subscription<T>`, and `PublishReceipt`; actors may belong to multiple groups simultaneously, and group membership is never lifecycle ownership.

Golden source parity fixture: `contracts/v1/fixtures/actor-pubsub-v1.ores`.
The backend must either execute this fixture with the normative output or reject the unsupported source surface before lowering; host-only scaffolding is not parity evidence.


## Current LLVM lowering foundation

The native runtime now exposes three layers without claiming positive source parity:

1. `OresSourceActorIdentity` snapshots a live compiler-created source actor as
   logical actor ID + scheduler domain/slot/generation under the source actor
   lifetime fence. Stale opaque pointers are rejected without dereferencing
   freed actor storage, and actor-owned heap generation follows the scheduler
   generation.
2. `NativeActorPubSubAdmission<T>` implements bounded membership-fenced
   RELIABLE/LATEST/LOSSY admission. Leaving one group removes only that
   group's subscriptions; actor retirement removes all subscriptions.
3. `NativeActorPubSubABI.h` exposes an exception-contained C ABI for i64
   group/topic admission. A C translation-unit test pins the symbols future
   compiler-generated LLVM IR may call, and a real SourceActorBootstrap test
   drives live source actor identities through that ABI.

The ABI is intentionally i64-only today. The native topic substrate now uses
preallocated bounded rings of shared immutable payloads and enforces default
16 MiB per-subscription / 64 MiB graph-wide logical queued-payload limits plus
a global preallocated-slot budget. RELIABLE fanout allocates/freezes once
before commit, so allocation failure cannot create a partial publication.

`Topic<String>` source nevertheless remains fail-closed: the existing native
String is an owned buffer, while the compiler-facing C ABI still needs an
ownership-safe conversion into/out of the shared immutable topic payload
without copying an owning String struct by value. `Subscription.next()` also
remains blocked on continuation-aware nonblocking read lowering. Therefore the
positive String golden fixture still rejects before IR and is not counted as
LLVM source parity.
