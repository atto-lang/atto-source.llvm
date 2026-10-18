# Oreslang actor pipeline routing v1 — five-backend conformance contract

**Semantic target**, not a claim of implementation parity. Identical rules apply
to .java (GraalVM), .llvm, .net, .wasm and .forge. Compiler acceptance of
`self.*` is **forbidden** until a real guest-lowered actor runtime satisfies
ownership, message admission and single-flight execution in that backend.

## Topology and identity

- A supervisor publishes a typed DAG with **2..1024** distinct actor
  **instance** IDs and **1..4096** distinct directed edges. Nominal actor
  class names may repeat; instance IDs must not.
- Instance ID, actor type and nominal payload name are valid strict UTF-8,
  contain no NUL, are nonempty and at most **256 encoded bytes**. An
  optional inbound/outbound type may be empty only for the corresponding
  missing port. Edges must match the exact nominal output/input payload type.
- `readable`: outgoing only, no inbound; `writable`: incoming only,
  no outbound; `transform` and `duplex`: both typed ports.
- A `transform` must have at least one **actual connected inbound edge** and
  at least one **actual connected outbound edge**. Merely declaring a typed
  output port does not make a terminal transform a valid pipeline sink.
- Reject duplicate, self, unknown, mismatched, cyclic, isolated and
  wrongly directed edges. Graphs must have an authorized ingress root.
- Actor instance identities are compared as **exact UTF-8 sequences**, not
  Unicode-normalized or case-folded strings. For example, composed `é` and
  decomposed `e + U+0301` remain distinct instances. The supervisor must
  avoid visually ambiguous names in administrative interfaces.
- An edge's identity is the **structural tuple** `(source instance ID,
  target instance ID)`, never string concatenation using user-controlled
  delimiters. `(a:b, c)` and `(a, b:c)` are separate valid connections.
- Connection order is the supervisor-published edge order, never hash map
  iteration order or task scheduling order. On topology generation change,
  rebuild selectors only after quiescence or an explicit abort barrier.

## Sealed actor methods and routing policies

| Oreslang intrinsic | Routing requirement |
| --- | --- |
| `self.forward(value)` | exactly one connected downstream; reject ambiguity |
| `self.forward_all(value)` | every downstream in edge order; one per-edge admission result |
| `self.forward_round_robin(value)` | one downstream, strict **source+generation** cursor, each attempted selection consumes a turn |
| `self.forward_to(target, value)` | only a supervisor-authorized outgoing edge |
| `self.forward_by_key(key, value)` | UTF-8 key bytes, 0..4096 bytes, exact FNV-1a **unsigned 64-bit** mod connected edge count |
| `self.backward(value)` | one correlated immediate authenticated upstream hop |
| `self.reply(value)` | correlated reverse path to the original sender, preserving branch identity |

Graph identifiers prohibit embedded NUL. **Routing keys are different**:
they may be empty or contain U+0000, hashed as their exact UTF-8 bytes.
Reject malformed Unicode; do not replace invalid code points or treat the
first NUL as string termination. The canonical `"\\u0000"` vector
hashes to `af63bd4c8601b7df`, which selects `worker-a` in the
three-consumer fixture.

`forward_all` is **not transactional**. A successful receipt is only
mailbox admission, not processing or exactly-once delivery. Partial success
is returned per edge, never hidden. A moved unique value cannot be fanned
out without explicit copying or immutable sharing. Per-edge message/byte/
pending-writer capacity must be bounded.

An actor executes on at most one physical carrier at a time. Sendable
messages must obey per-actor ownership, no mutable cross-isolate aliases,
no borrowed loans across suspension, and lifecycle barriers: STOP seals
new admission; accepted mail and terminal cleanup obey supervisor policy;
DONE follows complete cleanup. No actor carrier may block on a host
retirement gate.

**Keyed route validation order:** validate the active graph generation and
caller-authorized downstream source **before** inspecting or hashing the
user-supplied UTF-8 key. Unknown sources and retired generations must fail
with their routing/retirement diagnostic even if the key is malformed.
The scheduler's actor identity authorization remains a separate, stronger
precondition before any selector operation.

## Fixed cross-backend selector vectors

Three downstream edges, in publication order: `worker-a`, `worker-b`,
`worker-c`. Fan-out returns `[worker-a, worker-b, worker-c]`.
Round-robin returns `a,b,c,a,b,c`; selecting and then failing mailbox
admission still consumes that position. `forward` rejects this graph;
`forward_to(worker-b)` selects `worker-b`;
`forward_to(worker-z)` rejects.

FNV-1a64 modulo 3:
- `customer-7` -> `worker-a` (hash `660308ab83c6fdf1`)
- `alpha` -> `worker-a` (hash `8ac625bb85ed202b`)
- `beta` -> `worker-c` (hash `7627619b954620a7`)
- `🌍` -> `worker-a` (hash `ff430438753bca3b`)
- empty key -> `worker-c` (hash `cbf29ce484222325`)

Reject invalid Unicode, overlong encodings, NUL in graph identity,
duplicate actor instance IDs, source spoofing, unknown destination,
unsupported actor mode and post-retirement routing.

## Evidence labels required in backend PRs

**A: guest-executed parity**: identical typed Oreslang source fixtures
compiled and run with real actor messages and correlated reverse replies.

**B: native/host message parity**: real bounded mailbox routing, with
source authority and lifecycle checks (but no compiled guest `self.*`).

**C: selector parity**: checked graph + routing decisions tested, no guest
message delivery.

**D: fail-closed bootstrap**: unimplemented source constructs are rejected;
an independent selector contract may exist but must not imply actor
execution. Avoid accepting syntax as a placeholder noop.

A CI pass proves only tests **actually executed on an exact, approved tree**.
A zero-step GitHub Actions failure is no test evidence. Never copy private
trees into an unapproved organization, even if it has available minutes.

## Optional ActorGroup membership selector (v1 host extension)

An ActorGroup is **not** an actor, implicit transport broker, replacement
supervisor, source of new graph edges, or bearer authorization for returning
DATA. A supervisor may bind one runtime-local ActorGroup to a checked
routing generation; the graph-to-ActorId bindings are independently sealed.

- The selected candidate set is `current_group_member_actor_ids ∩
  current_source_outbound_graph_bound_actor_ids`. Compare actual ActorId
  identities, not class names, group names, array offsets, string aliases,
  or actor types. Creator membership alone must not imply delivery.
- Read one immutable copy-on-write group membership snapshot per routing
  attempt. Joins, leaves, termination, and group closure affect **future**
  selections, never an already-published immutable route/return path.
  Group-managed worker lifecycle and cooperative membership remain
  distinct; closing a group must not stop unrelated membership-only actors.
- `fanout` selects every eligible outbound edge in original DAG edge
  order, with an independently bounded mailbox admission and a receipt
  per attempted destination. Preserve the **original graph edge index** in
  every receipt, even if a selection omitted earlier edges.
- `selective_fanout` may only choose a nonempty subsequence of the
  source's authorized outbound edges. Reject unknown targets, duplicate
  branches, nonmember targets, and out-of-order/empty selections before
  reserving child frames or publishing DATA.
- `round_robin` retains a cursor over the fixed graph's full outbound
  order and scans cyclically for the first currently eligible member.
  A selected attempt advances the cursor even when mailbox admission
  later fails. An empty membership intersection rejects without moving
  the cursor or reserving a frame.
- `forward_to` requires its connected target to be a current member.
  Plain `forward` remains unambiguous only if exactly one target remains.
  `forward_by_key` keeps its strict UTF-8 graph hash and rejects if the
  designated actor has left; it must not silently remap persistent keys.
- All selectors still enforce the scheduler-owned DATA turn, shared actor
  eligibility, session/generation liveness, exact actor recipient binding,
  STOP fencing and independent mailbox resource limits. The ActorGroup
  does not grant cross-runtime or PRIVATE/UNTRUSTED mailbox authority.

A **host group-selection implementation** does NOT by itself enable source
`self.forward_all` or `self.forward_selected`. Guest fan-out requires a
typed, immutable per-edge receipt vector that reports partial success
without promising atomic multi-recipient delivery. Java currently has
host group selection; LLVM, .NET, Wasm and Forge need matching executable
tests before any capability/parity claim.

Required group-conformance vectors: one-member fanout; unauthorized
requested member rejection without consuming the source frame; membership
join expanding fanout; selected second edge preserving original index;
two eligible round-robin turns; terminated member skipped next turn; no
out-of-graph delivery; no hidden replay after partial success; group close
does not stop merely cooperating actors; and a reply from a previously
admitted branch follows its sealed origin path after membership changes.

## Exact admission linearization and retry classification (v1)

A selected correlation frame does **not** itself prove that its DATA was
published to the destination mailbox. Implementations MUST distinguish
reserved-but-unpublished from irrevocably admitted DATA:

1. At successful bounded mailbox enqueue (the `tryWrite` / native
   equivalent linearization point), record a **monotonic VM-owned published
   bit on the exact correlation frame**. Set it before any subsequent
   mutex-publication accounting, dispatch/scheduling work, or callback
   can fail. Never expose or serialize this bit as a guest field.
2. Prior to successful enqueue, a failed admission may retire the reserved
   child and, for authenticated `backward`/`reply`, restore only the
   **exact one-shot parent frame** consumed in creating that child.
   Matching flow ID and actor path are not sufficient: sibling branches
   may legitimately share both. Failed attempt route IDs remain spent.
   A rollback must never be applied twice or after generation fencing.
3. After successful enqueue, **no** exception, actor STOP, cancellation,
   scheduling rejection, or envelope cleanup can turn the admitted DATA
   into a rejected receipt or justify retrying the previous route.
   Leave its correlation frame to real mailbox delivery/teardown and
   preserve original VM-fatal `Error` classification where applicable.
4. `forward_all` / selective fanout receipts are **per attempted edge**.
   A postpublication fault cannot be represented as an ordinary negative
   admission receipt or be used to reissue an earlier accepted branch.
   It is a terminal/fault outcome requiring supervisor observability,
   not transactional rollback of already-published branches.
5. Guest source-level `self.forward_all`/selective fanout requires typed,
   immutable receipt semantics. None of these rules imply end-to-end
   exactly-once processing, durable queues, global mailbox atomicity,
   or network delivery guarantees.

Required regression evidence: same-flow/same-path sibling rollback
impersonation rejection, repeated rollback rejection, prepublication
reverse failure followed by safe alternate reply, published return
cannot be restored before OR after frame retirement, envelope receiving
the published bit before guest code, and partial fanout preserving
admission classifications. Exact-tree compiler/runtime executions are
separate from structural/static source assertions.

## Exclusive route-frame mailbox publication (2026-10-17 hardening)

Each live routing frame has one **VM-only atomic admission state**:
`FREE = 0`, `CLAIMED = 1`, `PUBLISHED = 2`.
A sender must acquire the exclusive `FREE → CLAIMED` transition
before fallible DATA preparation. Competing simultaneous sends of one
sealed frame fail closed without duplicate mailbox DATA or additional
actor work. A definite prepublication rejection performs
`CLAIMED → FREE`, preserving the original route/return right when
the caller owns a verified failed-admission result.

A successful bounded mailbox enqueue (the **linearization point**) is
immediately followed by `PUBLISHED = 2`, without allocation, a
fallible session/epoch check, callback, monitor acquisition or
potentially throwing bookkeeping between `tryWrite` and the marker.
`PUBLISHED` is monotonic. Any later runtime error is postpublication
and must not revoke accepted envelope ownership, issue a negative
admission receipt, restore a reverse source, or authorize retry.

Root ingress is also subject to this rule: a postpublication failure
must not unconditionally retire the accepted ingress frame or mislead
the supervisor into re-ingressing the same DATA. A fatal `Error` must
not be hidden as a recoverable rejection.

Cross-backend stress vectors: eight simultaneous claimants yield
exactly one reservation; abort restores exactly one retryable claim;
successful enqueue forbids all subsequent claims; publisher faults
both before and after the actual mailbox enqueue produce distinguishable
admission outcomes; sibling-frame rollback stays identity-checked.
A test that imports only a subset of identical Git blobs is evidence
for that subset, **never proof of a complete exact-tree build**.
