# Private CI mirrors: deny-by-default policy

Never transfer private oreslang-source.llvm source into a public or internal GitHub repository, including temporary refs, dangling Git objects, Actions artifacts, caches, and logs.

## Destination approval

The file `.github/private-ci-approved-repositories.txt` is empty by default. No cross-repository source transfer is authorized until an exact private `owner/repo` destination is added through source governance independently reviewed by `@ORESoftware` **and source `main` is protected with the required review controls**. There is no unprotected-source export fallback.

Destination privacy, repository identity, workflow permissions, retention, and personnel access must be revalidated immediately before every transfer. A writable repository or successful source-free runner canary is not authorization.

Java repository approvals do not authorize LLVM source exports.

## Incident resume gate

Before the publisher may read or export private source bytes, it must query the
immutable platform-conformance incident identities for #22, #77, #127, #212, and #222 and
require each issue to be exactly `closed` with `state_reason=completed`
and GitHub's immutable `closed_by` identity equal to reviewer
`ORESoftware` (user ID `11139560`). Open incidents, alternate close reasons,
issue-ID/URL drift, closure by another account, missing closure identity, or a
pull request masquerading as an issue all fail closed. The gate is rechecked every time
protected authority is re-attested, including immediately before executable
carrier authorization.

## Transfer requirements

Any future publisher must run from trusted protected-source code, never from the PR under test, and must:

- accept only an exact destination listed by protected-main policy;
- require live `private=true` visibility and bind the destination repository ID;
- create a fresh isolated `ci/oreslang-*` branch and never update destination `main`;
- export the complete intended source snapshot without stale destination files;
- reject symlinks, unsafe filesystem objects, unexpected external paths, and unbounded input;
- verify every uploaded blob and a canonical full-tree manifest against the source snapshot;
- bind evidence to source repository, PR number, exact head SHA, source-tree digest, mirror commit/tree digest, toolchain, and test commands;
- use minimum `GITHUB_TOKEN` permissions and no cross-organization source checkout credentials;
- retain no source-bearing artifacts or caches beyond the reviewed retention policy; and
- fail closed if policy, visibility, identity, source head, or tree digest changes during publication.

Direct GitHub connector/API blob/tree writes are not an authorization bypass. They may only be used by an approved publisher after the same protected-source checks are satisfied.

## Capacity canaries

A source-free workflow may be used to verify that a candidate private destination currently has GitHub Actions capacity. Canary success proves runner availability only; it does not authorize source transfer or certify any Oreslang PR.

## Current state

No private CI destination is approved by default. Until a destination is independently reviewed and merged into protected `main`, private LLVM source must remain in the source repository.

## Trusted admission preflight

Run the offline, source-free policy shape tests without exporting compiler code:

```sh
python3 scripts/test-audit-private-ci-governance.py
python3 scripts/audit-private-ci-governance.py
```

The opt-in `--live` preflight diagnoses the mandatory source protected-main
gate by reading source repository metadata, `branches/main`, and branch
protection with a read-capable `GITHUB_TOKEN`. Carrier review/tree attestation
is enforced separately by V3 when the destination carrier itself is
unprotected. Neither check is a source-export grant.

**Observed governance blocker (2026-10-19):** GitHub returned
`protected=false` for source `main`. Ruleset reads also returned a
plan-related 403. No source transfer should occur while actual protected-main
enforcement cannot be independently verified. Adding CODEOWNERS text alone
does not turn on branch protection.

## Provenance and cleanup

An authorized future publisher must bind its immutable receipt to the
source repository ID, source PR head and full root Git tree SHA, the exported
subtree SHA (which must match the complete authorized tree), destination
repository ID and visibility, exact test ref, workflow and toolchain versions,
test outcomes, and expiration time. It must use an approved cleanup plan for
refs, Git objects, caches, Actions artifacts, and logs; deletion of branch refs
alone does not erase retained private Git objects. Neither source-free
synthetic canary tests nor success against another branch can substitute for
this exact-head evidence.


## V3 exact source-API publisher

The source-side v3 publisher is `scripts/publish-private-ci-source-v3.py`,
invoked only through `scripts/run-in-private-ci-destination.sh`. It does not
trust a caller-provided working-tree snapshot. Instead, it requires the exact
current head SHA of an open PR against source `main`, reads that commit's
complete root Git tree from the official private source API, rejects
symlink/submodule/unsupported modes and unsafe paths, fetches each bounded Git
blob by SHA, and verifies the blob bytes against the Git object ID before any
destination write.

Before reading or exporting source, the publisher self-attests its governance
bytes against the exact source `main` commit and requires source `main` to
be protected. Source `main` must enforce CODEOWNER review, stale-review
dismissal, at least one approval, administrator enforcement, no force
pushes/deletions, and no PR-review bypass actors. If GitHub reports
`protected=false` or the required protection cannot be verified, private
source export is denied.

There is deliberately **no review-attested fallback for the source repository**.
A reviewed governance PR, CODEOWNERS text, or source-free carrier success cannot
substitute for live source-main protection. The source PR under test is also
bound separately to its exact open-PR head and complete root tree.

An allowlist entry is necessary but not sufficient. The protected
`.github/private-ci-carrier-contracts.json` additionally pins:

- the destination repository ID;
- the exact reviewed, source-free carrier `main` commit;
- the immutable carrier workflow blob; and
- exact reviewed test-plan blobs and allowed PR numbers.

The reviewed carrier base must contain neither
`private-oreslang-source/` nor `ci/oreslang-receipt.json`. Any carrier
`main` advance, delete/recreate, workflow change, or test-plan change fails
closed until the source contract is independently reviewed again.

Carrier `main` is also a live authorization boundary. Protected-main mode
requires CODEOWNER review, stale-review dismissal, at least one approval,
administrator enforcement, force-push/deletion disabled, and no review bypass
actors.

When carrier branch protection is unavailable, the protected source carrier
contract must additionally pin a positive `review_pr`. That PR must be a
same-repository PR merged to carrier `main`, independently approved by
immutable reviewer ID `11139560` / login `ORESoftware` on its exact head
before merge, and authored by a different valid account. The final carrier
`main` root-tree SHA must equal the reviewed PR head root-tree SHA, and the
carrier must contain the exact security CODEOWNERS file. The contract still
pins the exact final carrier-main commit SHA, workflow blob, plan blob, and
allowed source PR. Carrier `main` SHA is rechecked before every source-bearing
write; movement during transfer fails closed.

The carrier workflow keeps the source-test job at `contents: read`. Cleanup is
performed by a separate no-checkout job with `contents: write` that validates
both ephemeral refs still point at the exact carrier commit before deleting the
`ci/authorized-<CARRIER_COMMIT_SHA>` and `ci/oreslang-<SOURCE_TREE_SHA>` refs.
Ref deletion limits accidental continued reachability; it does not claim to
erase already-created Git objects, Actions logs, or provider-side retention.

After reconstructing the exact source root tree in the carrier, the publisher
requires the uploaded subtree SHA to equal the original PR root-tree SHA. It
creates an `ores-private-ci-receipt-v3` binding source repository ID, PR
number, head SHA, root-tree SHA, manifest SHA-256, destination repository ID,
reviewed carrier base, carrier workflow blob, reviewed test-plan digest, and a
24-hour expiry. The receipt digest is bound into the carrier commit message.

The executable ref is derived, never caller-selected:

```text
ci/oreslang-<SOURCE_ROOT_TREE_SHA>
```

The publisher then creates the carrier's exact dispatcher marker:

```text
ci/authorized-<CARRIER_COMMIT_SHA>
```

The destination workflow refuses real source tests without that exact marker
and revalidates live destination identity before checkout. No source repository
credential is present in the carrier.

Supported invocation:

```sh
scripts/run-in-private-ci-destination.sh opto-sync-test/go-java-validation \
  --source-pr 669 \
  --source-commit FULL_40_HEX_OPEN_PR_HEAD_SHA \
  --test-plan ci/test-plans/llvm-pr669.json
```

The current protected carrier contract includes reviewed source-free plans for
LLVM PRs #669 and #674. The allowlist remains separately reviewed; the carrier
contract by itself authorizes no export.

Source-free regression tests:

```sh
python3 scripts/test-audit-private-ci-governance.py
python3 scripts/test-publish-private-ci-source-v3.py
```
