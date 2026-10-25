# Private CI mirrors: deny-by-default policy

Never transfer private oreslang-source.llvm source into a public or internal GitHub repository, including temporary refs, dangling Git objects, Actions artifacts, caches, and logs.

## Destination approval

The file `.github/private-ci-approved-repositories.txt` is empty by default. No cross-repository source transfer is authorized until an exact private `owner/repo` destination is added through a reviewed protected-branch change owned by `@ORESoftware`.

Destination privacy, repository identity, workflow permissions, retention, and personnel access must be revalidated immediately before every transfer. A writable repository or successful source-free runner canary is not authorization.

Java repository approvals do not authorize LLVM source exports.

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

No private CI destination is approved by default. Until a destination is
independently reviewed and satisfies either protected carrier-main enforcement
or the exact-tree review-attested carrier mode pinned by protected source
governance, private LLVM source must remain in the source repository.

## Trusted admission preflight

Run the offline, source-free policy shape tests without exporting compiler code:

```sh
python3 scripts/test-audit-private-ci-governance.py
python3 scripts/audit-private-ci-governance.py
```

The opt-in `--live` preflight reads source GitHub repository metadata,
`branches/main`, and `branches/main/protection` using a read-capable
`GITHUB_TOKEN`. It intentionally diagnoses only the classic protected-main
mode; failure there does not authorize export and does not by itself reject the
separate exact-review-attested V3 mode. The V3 publisher performs the actual
review-history, reviewed-tree, governance-byte, destination, and source-head
checks.

At the current audit, GitHub reports `protected=false` for source `main` and
private rulesets unavailable on the repository plan. Therefore any real V3
export must use an exact independently reviewed source-authority PR until the
repository protection state changes. CODEOWNERS text alone is never treated as
enforced review.

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

Before reading or exporting source, the publisher self-attests the complete
export-critical source-governance surface against the current source `main`.
V3 supports two fail-closed source authority modes:

1. **Protected source main.** Requires CODEOWNER review, stale-review
   dismissal, at least one approval, administrator enforcement, no review
   bypasses, no force pushes, and no branch deletion.
2. **Exact review-attested source main.** When private-repository protection is
   unavailable, the publisher requires source authority PR #958 (and future
   governance PRs that deliberately advance that authority constant) to be
   same-repository, merged to `main`, authored by someone other than immutable
   reviewer ID `11139560` / login `ORESoftware`, and approved by that
   reviewer on the exact PR head before merge. It compares every reviewed
   governance Git entry and file byte-for-byte between that head and current
   `main`: CODEOWNERS, allowlist, carrier contracts, governance workflow,
   security policy, governance audit, launcher, V3 publisher, and their
   security tests.

The review-attested mode does not trust unprotected `main` generally; it
trusts only governance bytes that remain identical to the independently
reviewed authority head. Any mode/type/blob, reviewer-state, repository-ID, or
governance-byte drift fails closed.

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

Carrier `main` is also a live authorization boundary. V3 supports two
fail-closed carrier modes:

1. **Protected carrier main.** Requires CODEOWNER review, stale-review
   dismissal, at least one approval, administrator enforcement,
   force-push/deletion disabled, no pull-request-review bypass actors, and the
   exact security CODEOWNERS file.
2. **Exact-tree review-attested carrier main.** For private repositories where
   branch protection is unavailable, the protected source contract must include
   an exact positive `review_pr`. That PR must be same-repository, merged to
   carrier `main`, authored by an account other than immutable reviewer ID
   `11139560` / login `ORESoftware`, and the latest review from that
   reviewer must be `APPROVED` on the PR's exact head before merge. The final
   carrier main root-tree SHA must equal the independently reviewed PR head
   root-tree SHA exactly, and carrier CODEOWNERS must match the security
   contract.

A pinned carrier SHA by itself is **not** sufficient. The review-attested mode
adds an independently verified exact-tree approval, so any later path, Git
mode, blob, reviewer-state, repository-identity, or main-SHA drift fails closed.
The exact carrier-main SHA is rechecked before every source-bearing API write;
full carrier review/tree attestation is repeated at admission, immediately
before executable refs, and again in the inert payload window before the
one-use authorization ref is created.

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
