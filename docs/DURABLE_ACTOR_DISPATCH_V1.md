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
  guest code cannot choose its path. Symlinks at **any ancestor** (not only
  the final directory), relative paths, dot-dot traversal, and insecure
  directory permissions fail closed. Native LLVM uses a root-anchored
  O_NOFOLLOW openat walk so another pathname cannot redirect an already
  opened directory descriptor. Java now uses the JDK
  SecureDirectoryStream for component-by-component nofollow traversal,
  CREATE_NEW relative to the pinned handle, and directory fsync through
  that same handle. Filesystems/providers without these capabilities
  fail closed. Java compares directory ownership to the OS-provided
  UnixSystem username, not mutable JVM user.home/user.name properties.
  This does not defend against a hostile process with the **same OS
  identity** replacing the entire initial trusted namespace before
  it is opened; host provisioning and trust separation remain required.
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
revision, symlink ancestors, relative/noncanonical paths, property-spoofed
owner references, and insecure directories are denied. A concurrent rename /
symlink-swap stress test executed 800 swaps with no decoy-directory claims.

LLVM POSIX host: GCC and Clang ASan/UBSan pass. Twelve forked claimants
yield one winner; SIGKILL after claim leaves later claims refused. The
HostGate is wired to the durable claim callback in the host test.
The native store now walks all directory components using O_NOFOLLOW
directory handles, with negative tests for ancestor symlinks and dot-dot.

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

## PostgreSQL transactional host-admission candidate (not yet source-executed)

The prior migration was **malformed** and has been replaced entirely in Java
and LLVM. The corrected migration `postgresql/001_actor_dispatch_v1.sql`
has Git blob `c36d462ec9a627b9fc30121ccd0df75f0a340142` in both repositories.
The matching credential-free role/fence/binding fixture
`postgresql/002_test_fixture.sql` is Git blob
`d80bbf5228f06eeba6c09c414c08bec4f8a5ca9c`.

The transactional SECURITY DEFINER function validates the authenticated DB
`session_user` against an independently maintained tenant/actor binding,
locks that binding and the current generation/revision fence for the claim
transaction, rejects asynchronous-commit sessions and server fsync=off,
and uses a stable-identity unique constraint for one-shot admission.
Runtime users must receive function EXECUTE only, no direct table access.

Source-local test workflows now include PostgreSQL service startup, 100
competing Java/LLVM host requests, twelve independent process claimants,
an exact persistent-row count, negative cross-tenant checks, post-claim
SIGKILL replay denial and post-restart generation fencing. **These are
test scenarios committed to CI, not passed Postgres integration results.**
Current source-org workflows fail without executing steps.

An alternate private runner's **source-free generic PostgreSQL 17**
service/transaction canary (honeypot-r-us-test/hnpt-e2e PR #20,
run 41004508072) did execute the generic 12-client unique-constraint
check. It contained no Oreslang source and does not prove this migration.
Private-source export remains forbidden under open conformance incident #22.

No cross-host production deployment, authenticated provider receipts, actual
irreversible provider effects, or native Oreslang guest program was verified.
Keep both PRs DRAFT pending real exact-head CI and independent review.

## October 30 hardening / executable-evidence limits

- The initial migration now refuses pre-existing schemas and functions instead
  of silently replacing arbitrary objects. A dedicated non-superuser role owns
  the schema and SECURITY DEFINER function; migration recreation must fail.
- CI now boots a loopback-only ephemeral PostgreSQL 17 container using
  per-run random SCRAM credentials, not POSTGRES_HOST_AUTH_METHOD=trust.
  It checks runtime credential cannot authenticate as the database owner,
  and runtime roles cannot read claim or authorization tables.
- A function-local SET synchronous_commit cannot by itself guarantee durable
  COMMIT because the GUC setting can revert when the function returns. The
  function instead requires synchronous_commit in (on, remote_apply), while
  the trusted adapters submit a single autocommit statement. A client that
  deliberately changes the setting before an outer COMMIT is outside this
  trusted-adapter model. The integration workflow denies async-commit claims
  and explicitly verifies the claim was not inserted.
- Java and native CI now explicitly assert stale-generation exit status 2
  after changing the supervisor fence, alongside the 100-request and
  12-process atomic-claim tests.

Static exact-blob/contract validation PASSED. The source-org Java and LLVM
jobs and the latest source-free canaries in honeypot-r-us-test, benefactor-cc
and voxletra all FAILED BEFORE RUNNER EXECUTION (zero job steps). Earlier
source-free PostgreSQL run 41004508072 did pass on honeypot-r-us-test, but
that historical result does not establish capacity at this moment. No private
source was transferred to any alternate organization. The real Oreslang
PostgreSQL migration and provider/actor effects remain unexecuted.
