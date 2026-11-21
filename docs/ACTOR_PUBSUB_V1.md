# Actor Pub/Sub v1 parity target

LLVM: NativeActorMembershipRegistry is the current host-side membership substrate. This contract does not claim guest ActorGroup/Topic lowering yet; unsupported source constructs must reject before IR emission. Parity requires generation-fenced memberships, bounded fanout admission, actor-retirement cleanup, and native execution of the shared conformance cases.

Normative contract: `contracts/v1/actor-pubsub-v1.json`.

### Portable topic-name parity boundary

The host-side C++ admission API and the i64/UTF-8 C ABI now share one
nonallocating, locale-independent topic-identity rule:
`^[A-Za-z][A-Za-z0-9._/-]{0,127}$` (case-sensitive ASCII, 1–128 bytes).
Invalid UTF-8 bytes, Unicode names, whitespace, embedded NUL in C++ `std::string`
names, and 129-byte names fail before topic-ledger mutation.
The C-string ABI cannot represent an embedded NUL inside one topic name:
its bounded scan validates the terminated prefix supplied by its caller.

This aligns the topic-name identity portion of proposed portable PubSub
conformance PR #305 (including PS018), but is **not** execution of the
26 PS action vectors. The LLVM host substrate and portable reference also
differ in lifecycle and authority: native `try_read` immediately dequeues
instead of retaining in-flight capacity, and guest `ActorGroup`/`Topic`
source lowering remains unsupported and fail-closed. The host ABI is not
an authenticated, stable, compiler-generated guest API.

Native verification gates: CTests `native_actor_pubsub_admission` and
`native_actor_pubsub_c_abi` must execute on a source-local runner with
recorded steps; the source parser rejection gate must continue to pass.
An Actions run failed before any steps is `NOT_EXECUTED`.

No `define publisher` or `define subscriber` language forms are introduced. The shared source model is `ActorGroup`, `Topic<T>`, `Subscription<T>`, and `PublishReceipt`; actors may belong to multiple groups simultaneously, and group membership is never lifecycle ownership.

Golden source parity fixture: `contracts/v1/fixtures/actor-pubsub-v1.ores`.
The backend must either execute this fixture with the normative output or reject the unsupported source surface before lowering; host-only scaffolding is not parity evidence.


## Nonblocking RELIABLE native host admission (not compiled guest parity)

The native `NativeActorPubSubAdmission<T>::try_publish_reliable`
entrypoint delegates to authenticated, generation-fenced publication and
reports a `std::optional<PublishReceipt>`. Success is present only when
all RELIABLE recipient reservations succeeded; a Full refusal returns empty
and does not consume a topic-local u64 sequence or enqueue a partial
fanout. A revoked actor generation, unknown or closed topic, malformed UTF-8
payload, or LATEST/LOSSY policy still fails explicitly.

The RELIABLE-only policy check occurs while the membership/topic publication
locks are held; a caller cannot validate one policy and publish against a
different topic state after a concurrent replacement. The original native
`publish` API retains its existing delivery-policy behavior. Neither API
grants lifecycle authority to group members.

This is a **host C++ reference/runtime substrate**, not a source
`Topic<T>.try_publish` guest intrinsic. LLVM's `.atto` ActorGroup/Topic
fixtures remain explicit compiler negatives until source lowering, typed
`Result<PublishReceipt, PublishError>`, guest authorization, continuation
handling and executable guest traces exist. Java exposes a corresponding
host-only `Optional<PublishReceipt>` admission primitive, but matching
host APIs do not certify portable source parity.

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

## Canonical `share! try_publish` compiler-negative gate (2026-11-21)

The original actor.group and Topic<T> preflight did not explicitly reject a call to `imported_topic.try_publish(share! event)?` where the topic handle is provided by an enclosing scope. The parser gate now recognizes the bare `.try_publish` member and fails with the existing `native ActorGroup/Topic source lowering unsupported` diagnostic before native IR emission; it does not reserve free functions named `try_publish`. `tests/actor_pubsub_source_gate.py` pins a new `.atto` negative fixture by Git blob `c562ad380bb283da9ffa26160653c1186ab63462` and makes the exact compiler-negative CTest require it.

**NOT_IMPLEMENTED / NOT_EXECUTED:** This is a compiler preflight hardening change on the existing LLVM pub/sub PR. It is not successful LLVM guest lowering, not a typed Result ABI, not sanitizer execution, and not Atto Ensemble 28-case guest parity. See [cross-backend gate #551](https://github.com/atto-lang/atto-platform-conformance/pull/551). Independent source-local CTest, ASan and UBSan evidence remains required.
