# Native LLVM compiler source mirroring — denied until reviewed

`oreslang-source.llvm` is a private repository. No source files, Git objects,
Actions artifacts, logs, or compiled binaries may be copied to another
organization merely because the source organization's Actions budget is
exhausted.

## Source-owner authorization

The authoritative private CI destination allowlist is
`.github/private-ci-approved-repositories.txt` on the source repository's
**protected main branch**. It is deliberately empty. A proposed destination
must be approved by independent project maintainers and security reviewers
via the protected branch, not merely by a caller-supplied environment
variable or an unreviewed PR checkout.

Reviewers must confirm confidentiality expectations, member access,
potential inherited Actions access, retention/deletion policies, and whether
the destination is a suitable isolated CI repository. Repository
administrators must configure mandatory independent review and branch
protection; the presence of this text file alone cannot enforce approvals.

## Transfer preconditions

Once authorized, use only an audited export mechanism that (1) authenticates
both source and destination as private, unarchived GitHub repositories,
(2) checks the source's **main** allowlist instead of PR-supplied authorization,
(3) pins the full source commit and tree, (4) excludes symlinks, gitlinks,
unsafe paths and oversized files, (5) checks destination blobs and exact
materialized subtree hashes, (6) writes only beneath a dedicated
inert `snapshots/...` subtree on a new, isolated temporary branch, never into
repository-root `.github/workflows/` where private source could be executed
as destination GitHub Actions configuration, (7) checks canonical source
blob base64 before transfer, and (8) rechecks source/destination privacy,
source-main approval and the pinned destination-main commit before **each**
blob/tree/commit/ref mutation.

The proposed shared platform-conformance publisher hardening is in
[security PR #29](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/29).
Until this gate or an equivalently reviewed implementation is active, do
not mirror LLVM source by arbitrary GitHub connector file/blob operations.

A verified private carrier can help with test execution but **does not**
prove the Oreslang native compiler works and does not override required
source-repository checks or human code review.

## Cleanup and reporting

After approved CI completes, restrict access to temporary refs and review
retained Git objects, Actions logs/artifacts/caches, and their retention
windows. Rewinding/deleting a branch does not automatically purge Git objects.
Document the exact source commit, native compiler binary digest, LLVM IR
digest, linked executable digest and real negative compiler diagnostics.

**No destination is currently authorized by this policy.**
