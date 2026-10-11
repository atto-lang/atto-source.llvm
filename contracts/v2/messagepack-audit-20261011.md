# Oreslang actor MessagePack v2 — audit and hardening, 2026-10-11

**Status:** targeted audited components green; **NOT release or merge admitted**.
The canonical production tagged actor codec is
`dev.oreslang.runtime.TaggedMessageCodec`; the older
`MessagePackActorContract` is a separate host/client primitive and must not
redefine source-level `define enum` / `define pattern` envelopes.
The canonical tagged format is documented in
`contracts/v2/tagged-enum-messagepack.md`.

## Findings fixed

1. **Java tagged-decoder computational amplification / inconsistent sender
   view.** Previously the decoder tried every compiled case, recursively
   attempting full MessagePack decoding of caller-owned bytes. At a maximum
   declared domain of 4096 cases, this caused avoidable unbounded repeated
   parsing relative to one envelope and let caller mutation influence different
   decoder attempts. Now it rejects frames larger than 1 MiB, clones exactly
   once, parses the canonical schema identity, looks up exactly one
   compiler-pinned case and performs one typed decode with pinned hash and tag
   checks. A missing case fails closed without fallback. The schema-peek parser
   is shared with the host actor codec to avoid separate acceptance rules.

2. **Generic native group contract time-of-check/time-of-copy gap.** Previously
   it checked grammar/digest using borrowed host strings and only then copied
   the contract into group storage. It now first snapshots schema bytes, shape
   bytes and the 32-byte digest and validates the copied values before
   registering. Failed validation frees the temporary copies. Registration
   remains trusted single-threaded construction; caller-provided pointers
   must be stable for the duration of the copy, and no other group mutations
   may race registration.

3. **Native LLVM source-actor bootstrap contract gap.** The compiler-facing
   `oreslang_source_actor_spawn_wire` likewise previously validated a borrowed
   contract before copying it. It now bounds, allocates and snapshots all
   contract fields *before* independently validating their grammar and
   canonical digest. Invalid declarations fail before allocating/starting an
   actor. A successfully registered actor owns its schema and shape until its
   supervised termination.

4. **API lifetime/feature ambiguity.** Header contracts now correctly explain
   owned registration, per-actor DATA slot reservations and producer fencing.
   The source-actor wire ABI is **one schema per actor**. It is not a substitute
   for compiler lowering of a multi-alternative closed pattern, and guest code
   cannot choose a new schema from untrusted bytes.

## Executed verification

- Java/GraalVM: [funded exact-source targeted Maven/JUnit
  #38175221113](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38175221113),
  **57 tests, 0 failures, 0 errors, 0 skips**, with **83 exact SHA-pinned
  Java source/test blobs**. Includes real guest-source actor tests, production
  tagged messages, a 384-alternative closed registry, forbidden schema and
  version mutations, ownership, envelope canonicalization and typed transport.
- LLVM native: [funded exact-source C
  #38175456858](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38175456858):
  source-actor wire ABI with GCC ASan+UBSan, legacy scalar actor compatibility,
  the byte-identical Java tagged enum golden, and generic actor-group
  adversarial tests under GCC sanitizers; also Clang optimized repeats.
  This evidence executes native library/test code, not compiled Oreslang
  tagged-variant guest programs.
- Production Java tagged golden file
  `contracts/v1/design/tagged_message_vectors.json` has the identical Git
  blob in both repositories. The canonical tagged transport contract also has
  byte-identical content across both repositories.

## Merge blockers and unresolved risks

1. **LLVM source frontend/codegen:** `define enum`, closed `define pattern`,
   per-case compiler-generated schema registration, source `ActorRef.send`,
   typed `receive`, exhaustive ordered `match`, and payload narrowing and
   destructuring must execute as guest programs; host C adapters are not proof.
2. **Typed futures and cleanup:** fully implemented `Future<T>`,
   nonblocking await continuations, cancellation propagation and finalizers
   must satisfy one-owner/single-delivery guarantees, with `defer/recover`
   and execution-context safety rules.
3. **Producer fencing:** generic and source native actor destructor APIs require
   external isolation of producer threads. No concurrent send may access an
   actor handle after trusted stop/destroy begins. This is not enforced by a
   ref-counted lease type in the current C ABI; lifecycle admission from guest
   code is a separate, release-critical compiler invariant.
4. **Broader validation:** full Maven project suite and GraalVM native-image
   at exact current merged heads, full LLVM CMake/CTest, cross-backend guest
   differential fixtures, stress tests under rapid stop/cancel, malformed
   message fuzzing, schema evolution/migration, and authenticated cross-process
   communication. SHA-256 shape hashes give structural compatibility, **not**
   origin authentication.
5. **No premature merge:** Java #623/#645 and LLVM #163/#180 are stacked
   drafts. Current targeted funded success must not be represented as full
   project CI, AOT success or complete two-backend parity.

The test org mirrors **exact Git blob hashes** because the source org may fail
GitHub Actions before allocating a runner (`runner_id=0`, no steps).
No private cross-org checkout token or secrets are required.
