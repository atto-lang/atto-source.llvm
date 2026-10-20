# Private CI mirrors: deny-by-default policy

Never transfer private oreslang-source.llvm source into a public or internal GitHub repository, including temporary refs, dangling Git objects, Actions artifacts, caches, and logs.

## Destination approval

The file `.github/private-ci-approved-repositories.txt` is empty by default. No cross-repository source transfer is authorized until an exact private `owner/repo` destination is added through an independently reviewed governance change owned by `@ORESoftware`.

Destination privacy, repository identity, workflow permissions, retention, and personnel access must be revalidated immediately before every transfer. A writable repository or successful source-free runner canary is not authorization.

Java repository approvals do not authorize LLVM source exports.

## Transfer requirements

Any publisher must run from source governance accepted by the v3 authority check, never from the PR under test, and must:

- accept only an exact destination listed by independently reviewed source policy;
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

No private CI destination is approved by default. Until a destination is independently reviewed and merged through the exact v3 authority PR, private LLVM source must remain in the source repository.

## Trusted admission preflight

Run the offline, source-free policy shape tests without exporting compiler code:

```sh
python3 scripts/test-audit-private-ci-governance.py
python3 scripts/audit-private-ci-governance.py
```

The opt-in `--live` preflight reads source GitHub repository metadata,
`branches/main`, and `branches/main/protection` using a read-capable
`GITHUB_TOKEN`, failing closed for missing permissions, missing CODEOWNER
approval enforcement, stale-review acceptance, or missing administrator
enforcement. It is a *necessary condition*, never a source-export grant;
the checked destination, independent approval, policy provenance, and
publisher still require separate verification.

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

Before reading or exporting source, the publisher self-attests its own script
bytes plus the reviewed governance set against an authorized source `main`
commit. V3 has two fail-closed authority modes:

1. **Protected-main mode.** When GitHub exposes branch protection, source
   `main` must enforce CODEOWNER review, stale-review dismissal, at least one
   approval, administrator enforcement, no force pushes/deletions, and no
   PR-review bypass actors.
2. **Exact review-attested mode.** When the private-repository plan does not
   expose those controls, authority PR #728 must be merged to source `main`
   only after the latest review from immutable reviewer ID `11139560`
   (`ORESoftware`) is `APPROVED` on that exact PR head and predates merge.
   The PR author must be a different account. V3 then requires every
   export-critical governance path on current `main` to match the reviewed
   head by path, Git mode, object type, and blob SHA. Truncated trees,
   malformed/ambiguous review history, stale/revoked reviews, repo-ID drift, or
   any governance drift fail closed.

The fallback is not general trust in an unprotected branch. It is authority
only for the exact reviewed governance objects and is rechecked immediately
before executable carrier refs are created.

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

The current carrier contract is review-gated in authority PR #728 and pins only explicitly reviewed source-free plans. The carrier contract by itself authorizes no export.

Source-free regression tests:

```sh
python3 scripts/test-audit-private-ci-governance.py
python3 scripts/test-publish-private-ci-source-v3.py
```
