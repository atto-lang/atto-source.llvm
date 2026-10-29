# Durable Actor Dispatch v1 — local-host claim adapter

STATUS: DRAFT HOST-ONLY SUBSTRATE, NOT NATIVE ORESLANG ACTOR EXECUTION.

This executable one-shot admission claim is implemented in the Java/GraalVM
and LLVM POSIX host references. Their on-disk claim names and records match.

## Persistent identity (security-critical)

The opaque filename is SHA-256 of the following UTF-8 bytes:

  ores-agent-dispatch-claim-v1\nTENANT_ID\nDURABLE_ACTOR_ID\nDISPATCH_ID\n

Example tenant=tenant, actor=worker, dispatch=dispatch:
  0d308e5482aa6701e9dc55b9c1e70e311eb83188e26c8bf86befa838c2d11e21.claim

Input fields are validated ASCII with no newline. Never include actor generation,
activation epoch, Git revision, or context digest in the idempotency filename:
an actor restart, hot reload, or changed payload must not mint a second claim.
These other fields remain mandatory authorization/provenance checks. The
actor_id must be a durable logical actor identity, never a transient runtime
slot or process address.

Claim records contain exactly:
  ORES_AGENT_CLAIM_V1\nLOWERCASE_CONTEXT_SHA256\n

A corrupted, incomplete, conflicting, or ambiguous claim is permanently
reserved until an independently authenticated provider reconciliation.
The adapter cannot perform such reconciliation by itself.

## Single-host durability, not distributed exactly-once

- The trusted host provisions an absolute owner-only POSIX 0700 directory;
  guest code cannot choose its path. Symlink roots and insecure directory
  permissions fail closed.
- Claim creation is exclusive and atomic on an appropriate local filesystem,
  mode 0600, with file and parent directory fsync before returning true.
- The callback is bound through the trusted HostGate, preserving independent
  authorization, generation, revision, revocation and deadline validation.
- No claim is automatically deleted after uncertain submission or host crash.
- Cross-process local admission is tested, but multi-host linearizable CAS,
  remote provider idempotency and signed post-effect receipts are NOT built.
- This change is on an unmerged draft branch. Any deployment with older
  prototype claims requires reviewed migration/reconciliation first; never
  wipe or silently reset claim files.

## Local executable evidence

Java 21: independent JVM processes race on a shared private directory;
one winner. Replays, changed digests, restarted generations, changed Git
revision, symlink roots and insecure directories are denied.

LLVM POSIX host: GCC and Clang ASan/UBSan pass. Twelve forked claimants
yield one winner; SIGKILL after claim leaves later claims refused. The
HostGate is wired to the durable claim callback in the host test.

Interoperability: C++ first then Java, and Java first then C++, deny
duplicate claims; eight mixed Java/C++ contenders yield one winner and
seven denials. Tested sources matched their exact committed Git blobs.

These results are HOST-ONLY. No real native Oreslang guest, GraalVM Native
Image AOT, remote irreversible effect, signed provider reconciliation,
networked distributed CAS, or nonzero-step source Actions proof exists here.

## Next production gates

Implement trusted issuer and per-actor capability validation at the actual
provider, storage-enforced multi-host CAS, authenticated receipts, SIGKILL
before/after remote commit, and real compiler/runtime actor messaging.
Keep drafts unmerged pending independent review and exact-source CI.
Private-source export remains prohibited under open conformance incident #22.
