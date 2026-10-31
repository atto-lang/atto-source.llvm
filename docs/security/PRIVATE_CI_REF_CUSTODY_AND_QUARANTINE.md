# Private CI reference custody and quarantine contract

Status: **proposed; NOT independently approved** (2026-10-31).

Applies to the draft V3/V4 publisher in `scripts/publish-private-ci-source-v3.py`
and to proposed cross-organization Oreslang `.NET` exact-tree testing.
This document **does not** authorize private-source publication. Source
destination allowlists must stay empty and the production
`require_atomic_ref_rollback()` guard must keep denying until every
applicable gate below is independently completed.

## Threat model: SHA leases are necessary but insufficient

Two source-bearing refs represent a uniquely generated handoff:

- `refs/heads/ci/oreslang-v4-r<run>-a<attempt>-<nonce>`
- `refs/heads/ci/authorized-v4-r<run>-a<attempt>-<nonce>`

The run/attempt/nonce identifies a proposed generation; it is not proof
that only the issuing publisher can update or delete either reference.
A competing writer can move A→B→A between observations. An atomic
`git push --atomic --force-with-lease` may then succeed despite that
ownership violation. Two matching SHA observations, a successful local
Git transport experiment, and a 404 after deletion are **not** proof
of exclusive custody or of which actor deleted the refs.

GitHub REST `DELETE /git/refs/{ref}` accepts no verified old-SHA
precondition; a GET-then-name-DELETE sequence is prohibited.

## Required independently enforced custody

1. **Destination and approval identity.** Bind one private destination
   repository's numeric GitHub ID, exact source commit, root tree,
   allowed workflow bytes, exact run/attempt and a bounded retention
   period. No moving branch or repository-name-only approval.
2. **Exclusive namespace ACL.** A destination administrator independent
   of the publisher must configure and attest effective rulesets or
   equivalent access controls preventing every other actor (including
   default workflow tokens, alternate GitHub Apps, maintainers and
   routine admins) from creating, force-updating, deleting or
   recreating either generation namespace. Record provider ruleset IDs,
   enforced status, actor identities, and the set of allowed bypass
   actors. If the provider cannot enforce this scope, deny automatic
   cleanup and export.
3. **Separated authority.** Use a narrow, repo-scoped publishing
   identity; the identity that can change rulesets must not be the
   source uploader, CI workflow, or cleanup actor. Credentials must be
   short-lived, isolated, not logged, and never passed to guest builds.
   GitHub Actions' destination `GITHUB_TOKEN` cannot checkout private
   source from another organization. No source-bearing ref may be
   exposed to public or broadly accessible repositories.
4. **Prepublish readback.** Authenticate protected source main,
   independent approval, exact plan and tree/blob identities and
   destination owner/ruleset enforcement *immediately before* the first
   source-bearing destination write. Reattest immediately before ref
   creation. Any identity, author, policy or generation drift denies.
5. **Generation binding.** Preflight both ref names as absent. Prevent
   shared namespace reuse, unauthorized mutation and retry ABA. Treat
   ambiguous create/write results as potential exposure requiring
   quarantine; never retry with a different or broader reference.
6. **No unproven automatic deletion.** Require provider-verified atomic
   two-ref SHA-lease deletion **and** independently enforced exclusive
   namespace custody. A failed `--atomic` push, 404 readback, moved
   ref, absent authorization, or ambiguous transport must never
   trigger sequential or REST name-based deletion. Never infer
   successful cleanup from both refs becoming absent following a
   rejected atomic push.
7. **Quarantine/manual resolution.** If safe cleanup cannot be proven,
   stop additional dispatch, revoke test authorization, preserve
   audit evidence (repo ID, generation, last verified refs, SHA,
   timestamp, outcome, run attempt and owner), restrict the private
   destination against further writes, and open a tracked independent
   security incident. Only a separately authorized destination owner
   may resolve/quarantine source-bearing refs under the retention
   policy; do not auto-clean them in a best-effort exception handler.
8. **Proof of execution.** The exact-head V3 Python regression suite,
   destination provider permission/ABA probes, and eventual private
   NativeAOT execution must have nonzero executed steps and
   independently inspected receipts. Zero-step jobs are runner
   admission failures, not test evidence. A synthetic source-free
   runner passing does not attest private source integrity.

## Acceptance / release checklist

- [ ] Protected source and destination branches, CODEOWNERS,
      independently approved rulesets and reviewer identities
- [ ] Attested exclusive namespace custody; no unvetted bypass actors
- [ ] Exact source commit/tree and destination numeric ID approved
- [ ] Provider-specific atomic delete, update, recreation, retry,
      transport and ABA adversarial tests run against disposable
      **source-free** refs using the actual constrained publisher role
- [ ] Full exact-head V3 publisher and local Git cleanup test suites
      actually executed with nonzero steps
- [ ] Verified quarantine and owner cleanup/retention procedure
- [ ] Independent signoff on incident #1154, no hidden bypass flag
- [ ] Only then separately approve source transfer and native execution

## Current evidence and prohibitions

At 2026-10-31, private [Beamscale run 41234079715](https://github.com/beamscale/bmscl-compiler/actions/runs/41234079715)
passed a source-free local bare-Git experiment with 3 executed steps:
the stale two-ref lease fails, matching leases delete, and ABA remains
undetected. The LLVM exact-head CI in the source organization has
executed zero steps. The publisher remains disabled before any private
source reads or writes, and the approved destination list is empty.

The `.NET` exact-tree Beamscale approval request is
[issue #277](https://github.com/ores-truffle-oreslang/oreslang-source.net/issues/277).
The cleanup blocker is [issue #1154](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1154).
Neither issue is resolved by this proposed contract.
