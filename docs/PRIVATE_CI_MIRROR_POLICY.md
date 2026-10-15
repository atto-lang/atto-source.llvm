# Private Oreslang CI mirrors: source-transfer security

This procedure is for **maintainer- and security-approved** private source
mirrors when source-repository GitHub Actions is unavailable. It never grants
blanket permission to transfer code to another organization.

## Required checks before any source transfer

1. Confirm the actual source repository and exact PR head commit SHA.
2. Confirm the source organization's billing/Actions failure reason through a
   reliable administrator-visible signal. A failed workflow with zero steps is
   **not by itself proof of exhausted minutes**.
3. Select a dedicated, **private** target repository with known administrators,
   reviewed collaborators, an approved GitHub App installation and an actually
   successful, executed disposable capacity probe. Merely seeing old Actions
   runs or repository write permission is insufficient.
4. Obtain protected-branch security and maintainer review of an exact
   `owner/repo` entry in
   `.github/private-ci-approved-repositories.txt`. No wildcard or automatic
   inference is permitted. The reviewed allowlist starts empty.
5. Run this fail-closed preflight on the source checkout:

   ```sh
   python3 scripts/check_private_ci_destination.py \
     --source ores-truffle-oreslang/oreslang-source.llvm \
     --destination APPROVED_OWNER/APPROVED_PRIVATE_REPO
   python3 -m unittest discover -s tests -p 'test_private_ci_destination.py'
   ```

   This verifies only the **allowlist**. Recheck private visibility, collaborator
   access, GitHub App scope, approvals, and Actions capacity against live GitHub
   metadata before continuing. Do not trust CLI claims of repository privacy.

## Copying through the GitHub connector (after approval)

- A target repository's `GITHUB_TOKEN` **cannot** check out a private source
  repository in another organization. Do not add a personal access token,
  deploy key, secret, or privileged cross-org workflow permission.
- Read source objects through the already-authorized GitHub connector at a
  pinned, immutable commit SHA. Copy the required tracked files as **new
  blobs** into a uniquely named temporary branch of the approved target.
  Source object SHA references are not automatically portable between
  repositories: create target-owned blobs and a target-owned Git tree/commit.
- Inspect the copied file manifest first. Exclude stored credentials,
  environment files, unneeded test fixtures, and source-controlled Actions
  workflows from the target snapshot. If removing files changes test coverage,
  record that explicitly; do not claim a byte-identical full-tree run.
- Run only a **trusted** target-default-branch workflow whose permissions are
  explicitly `contents: read`, with no secrets, third-party write actions or
  dangerous `pull_request_target` path. It must check out the pinned
  temporary commit as data. Untrusted mirrored workflows must not execute.
- Verify actual executed build/test steps and collect exact source and target
  commit IDs, test names, exit statuses and log URLs. Do not present a queued,
  startup-failed or step-less job as passing evidence.
- Delete the temporary branch and artifacts after verification, honoring
  security review and retention policy. Record any cleanup failure. Before
  cleanup, preserve non-sensitive test evidence in the source PR.

**Never mirror to a public repository, a third-party organization that lacks
approval, or a target with unknown budget/access.** If approval, privacy,
capacity or permission checks fail, stop without transferring private source.

**Preflight hardening:** the operational Python CLI intentionally does
not accept `--allowlist`; the only allowed manifest path is the checked-out
repository's `.github/private-ci-approved-repositories.txt`. This prevents
casual caller-supplied manifest substitution but **does not prove the checkout
came from protected `main`**. A trusted workflow, committed and reviewed on
the default branch, must independently load and verify the protected-branch
policy. Never allow an unreviewed PR to authorize its own target by altering
a source branch's manifest or test workflow. The preflight also rejects
same-owner repositories: this tool is specifically for cross-organization
mirrors, not same-organization runner selection.


## Exact-head test evidence and approval-file safety

The focused actor verification workflows check out the immutable PR-head SHA
rather than a moving branch or a synthetic pull-request merge ref, and pass
that SHA in `ORESLANG_EXPECTED_SOURCE_SHA`. Each verifier refuses a malformed
SHA, a mismatched HEAD, and dirty tracked/untracked inputs **before** invoking
Maven or the native host compiler. The SHA check is about tested *source
identity*, not permission to export source across organizations.

Run the offline guard regressions with:

```sh
python3 -m unittest discover -s tests -p 'test_private_ci_destination.py' -v
python3 -m unittest discover -s tests -p 'test_actor_ci_checkout_proof.py' -v
```

The latter suite uses temporary Git repositories and deliberately mocked
`mvn`/`java`/C++ programs. Passing it proves the checkout admission logic,
**not** actual Java/GraalVM/LLVM compilation or actor behavior.

The cross-org approval parser uses bounded 16 KiB reads from an opened
regular-file descriptor with `O_NOFOLLOW` (fail closed when unsupported)
and `O_NONBLOCK` where available. It now opens the immediate approval-policy
parent directory using a separate `O_DIRECTORY | O_NOFOLLOW` descriptor,
then opens the manifest relative to that directory with `dir_fd`; hardlinked
manifests (`st_nlink != 1`) are rejected. This guards against the tested
file and parent symlink substitutions, special-file stalls and oversized
manifests. These filesystem controls do **not** prove that the directory
contents are a code-owner-reviewed Git policy or prevent all hostile normal
directory replacements; the authorization must still be independently
authenticated from **protected source main** before any private source copy.
An unreviewed PR manifest or a passed local parser test is not authorization.

## Status on October 14, 2026

The Java source allowlist has **no approved repositories**. Current GitHub
Actions reports do not independently prove a spending-limit condition.
Cross-org private-source mirroring is therefore blocked by policy; host-only
local tests or approved same-organization runners are valid alternatives.
This document is a procedure, not evidence that Java JUnit, GraalVM Native
Image, LLVM CTest, or guest actor supervision passed.
