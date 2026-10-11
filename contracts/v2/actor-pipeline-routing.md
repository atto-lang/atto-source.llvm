# Actor pipelines: bidirectional routing and lifecycle contract (draft v1)

Status: **design / compiler admission gate, not an implemented guest runtime API**.
Java/GraalVM and LLVM must give these operators identical observable semantics
before `self.forward`, `self.backward`, `self.respond`, or `self.done`
are enabled as guest intrinsics. A stub, dynamically looked-up actor method,
ordinary `send` alias, or immediate success without delivery is forbidden.

## Operations and recipients

| Operation | Destination | Required authority |
| --- | --- | --- |
| `self.send(target, message)` | Explicit actor ref | Compatible destination contract |
| `self.forward(message)` | Selected outgoing pipeline edge | Runtime-minted downstream route |
| `self.backward(message)` | Selected incoming/reverse edge | Runtime-minted upstream route |
| `self.respond(value)` | Correlated request origin, **not** preceding stage | Live typed reply capability |
| `self.done()` | No implicit destination | Current delivery's stage-completion right |
| `self.fail(error)` | Pipeline failure coordinator and eligible waiter | Live failure right |

A linear actor pipeline may call `forward` and `backward` without explicit
recipients. At a fan-out/fan-in node, an unspecified ambiguous edge **must
fail** (compile-time if topology is static; runtime admission if dynamic).
Future port- or route-argument syntax requires a separate language proposal.
A backward notification does not require an outstanding request/reply Future;
it does require an authorized upstream edge. `respond` is never a synonym
for `backward`. `done` closes only the current stage/turn, not the pipeline.

## Runtime-only PipelineContext

Pipeline route metadata is scheduler/coordinator issued, not guest-authored
and not authority inferred from a MessagePack schema ID or wire bytes. A
delivery context must bind at least:

- `pipeline_id` and `flow_id` (independent pipeline and in-flight flow identity).
- `message_id`, parent/causal identity, and optional reply correlation.
- Actor ref **scheduler domain, slot, generation/incarnation**, and the
  actor-turn's scheduler-issued continuation ticket.
- Actual ingress edge and authorized outgoing/reverse edges or opaque route
  capabilities, each bound to its endpoint actor generation and message type.
- Cancellation epoch, completion state, and bounded in-flight admission credit.
- A route/hop budget or equivalent cycle-policy guard; never recurse inline
  through actor callbacks to implement forwarding.

Reverse routing follows the actual ingress edge/causal route, not merely a
mutable `previousActor` pointer. For multiple ingress paths, the context
must preserve which branch produced this delivery. Route/capability tokens
are opaque and cannot be serialized by untrusted actors to acquire authority.

## Admission, scheduling and failure

1. Typecheck each directional value against the selected edge's destination
   **nominal variant contract**. Every accepted value owns a detached snapshot.
   Preserve the existing canonical wire data array
   `[2, schema_id, sha256(shape), payload]` without adding route information
   inside the application payload. Version routing metadata separately.
2. Reserve a bounded DATA/admission credit before allocating or decoding.
   Transfer it atomically on success; release it exactly once after the
   receiving turn finalizer, or immediately when rejected. `await` on
   backpressure suspends the guest continuation, **never** a scheduler carrier
   or the master OresVM control thread.
3. Define asynchronous result states separately: **rejected**, **admitted**,
   **delivered**, and **stage completed**. `forward`/ `backward` must not
   report delivery when only admission succeeded. A destination that stops
   must reject new delivery and settle pending send Futures with a typed
   failure or cancellation, not silently drop.
4. For a `serial actor`, HTTP requests and ordinary DATA messages share
   one logical lane; the actor's turn remains owned across `await` and any
   pipeline send admission barriers. No unrelated turn may enter until the
   suspended turn finalizes.
5. Reject missing/ambiguous routes, stale actor generations, stale
   continuation tickets, expired cancellation epochs, unauthorized reply
   correlation, wrong nominal type, and dead or stopped pipeline links.
   Duplicate/late success, failure, and cleanup callbacks cannot release
   another task's turn or queue credit. `defer` and `recover` execute
   exactly once on their authorized lane, with earlier failures preserved.
6. The compiler may not synthesize a callable `self.forward`/ `self.backward`
   fallback as an ordinary actor method while the runtime intrinsic is
   unavailable. Unsupported backend/lowering is a compile-time diagnostic.

## Compatibility and verification

- Existing Java generic tagged codec can handle up to **4,096** alternatives
  for codec-only use. The current **portable source actor binding ceiling is
  64 alternatives**, matching LLVM `SOURCE_MAX_VARIANTS`. Never imply that
  successful Java host-codec decoding alone establishes portable execution.
- Shape descriptor: at most 4,096 bytes; depth 64; frame at most 1 MiB.
  Retain existing exact v2 golden vectors and immutable-snapshot guarantees.
- Shared executable guest fixtures: 3-stage forward and backward; backward
  unsolicited notifications; source-vs-immediate-predecessor `respond`;
  fan-out/fan-in correlation; missing and ambiguous routes; stale ref after
  restart; cancellation during waiting admission; destination teardown;
  bounded mailbox overload; double completion; nested `await`; serial
  actor interleaving, and `defer`/`recover` error cleanup.
- Java: full Maven/JUnit and Graal native AOT/hybrid; LLVM: full CMake/CTest
  plus ASan/UBSan and optimized repeat executions. Validate exact Git tree
  SHA/blob identities for any funded CI mirror. Do not merge on codec-only
  or handwritten callback harness evidence.

## Current admission status

The Java type checker intentionally rejects `self.forward`,
`self.backward`, `self.respond`, and `self.done` until the routing
context and actual guest lowering land. The native LLVM source actor adapter
supports bounded one-way typed sends but has no equivalent directional
pipeline route context yet. No release parity or enabled API is claimed.
