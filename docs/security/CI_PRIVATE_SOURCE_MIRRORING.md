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

No private CI destination is approved by default. Until a destination is independently reviewed and merged into protected `main`, private LLVM source must remain in the source repository.

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
