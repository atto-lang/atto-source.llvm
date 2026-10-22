# LLVM #732 private CI — review proposal, NOT export authority

**Status: DENIED / PREPARATORY ONLY (2026-10-20).** This file is documentation, not a machine-consumed allowlist, receipt, source-export authorization, workflow, credential or dispatcher permission. A GitHub PR, plan file, or approval comment by itself does **not** authorize copying private Git objects. Never treat this document as sufficient to mirror source or create `ci/oreslang-*` or `ci/authorized-*` destination refs.

## Frozen source candidate

- Source repository: `ores-truffle-oreslang/oreslang-source.llvm`, numeric GitHub repository ID `1410897874`, **private**
- Source PR: [#732](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/732), **draft**
- Live PR head when this proposal was assembled: `5b50ec005823da70af22ff8aa549e4130f0d681c`
- Exact root Git tree from GitHub's commit object: `13bdbd7aa343241d51410a61c55d52efc1be46ef`
- Essential pinned file blobs: `src/main/cpp/oreslang/runtime/NativeActorScheduler.c` `74f2bf706c23127bc24a26ea4c931c84f436fae9`; `tests/native_actor_indexed_ready_test.c` `b4c0d15c65c27a3e642c3013ade6c128ca68ef01`; `CMakeLists.txt` `22154cc56213888c27bc1d2c69996e9bc2109ed5`.
- Exact source commit/tree and every reviewed governance blob must be reverified just before any future publication. If source #732 changes, **reject** rather than silently repin.

## Destination — pending independent review

- Intended funded private carrier: `usa-acc/usa-acc-e2e`, numeric repository ID `1346346666`
- Carrier `main` commit observed during preparation: `be55c679d1c35eaa0e6cf3556489aebdca14027d`, Git tree `6dbfca0209df57bb6c3c008c2b8b14d541dcde09`
- Source-free [carrier plan PR #32](https://github.com/usa-acc/usa-acc-e2e/pull/32) remains **draft**; do not treat its head as destination `main`.
- Proposed plan path: `ci/test-plans/llvm-pr732.json`
- Current proposed plan Git blob: `664194598ebdc4c5372766db2edd4c3a1ea1c554`
- Existing immutable carrier workflow Git blob: `437da046ec9c8af0b608089c65923e3f4f857ec5`
- Existing cleanup workflow Git blob: `64ad80c6aa29b8802661a5fff15e21c050109f13`
- Existing cleanup script Git blob: `e66e966ec9b202f37f31b0e8ff920af9098bc902`
- Existing trusted test-plan runner blob: `03fd89b545cf0c93ca3e2c62de815eee5c1da035`

**Source of truth for the future real authority:** the carrier's **reviewed and protected** current `main` after the plan PR is approved and merged. Refresh its base SHA, plan Git blob and SHA-256 digest at that point. Check the workflow, cleanup, runner and plan hashes again; any drift denies. Do not use the stale #728 authority, which authorizes different PRs (#725/#726/#729) and an obsolete carrier base.

## Proposed native test plan (not executed against Oreslang)

The plan is restricted to source PR 732 and binds eight source blobs. It uses a source-root sandbox with three reviewed writable overlay directories and *nine static commands*:

1. Configure, build, and run the real native scheduler indexed-ready, carrier-locality, and send-stop CTest targets with GCC.
2. Configure, build, and run the same targets with Clang.
3. Configure, build, and run those native targets with Clang AddressSanitizer and UndefinedBehaviorSanitizer.

CTest output must include `NATIVE_INDEXED_READY_PASS actors=4096` to reject vacuous success. The child command plan uses only `cmake` and `ctest`; no shells, credentials, dynamic dependency install, network in sandbox, source-bearing artifacts, or caching.

## Mandatory security gates before any source transfer

1. **Establish enforceable repository governance:** the source `main` and destination `main` currently report `protected: false`. REST rulesets lookup on both private repos returned HTTP 403 asking for GitHub Pro or public visibility. Determine appropriate eligible private-repository plan and actual branch-review settings; do not substitute a local allowlist or self-approval.
2. **Independently review the carrier plan and its sandbox runner** on its exact latest head; require reviewer distinct from source author/last pusher, without admin bypass, stale review or force-push loopholes.
3. **Only after merging that reviewed plan**, construct a separate executable source authority with its exact plan file blob, SHA-256, destination main/tree, immutable destination numeric ID, pinned workflow/cleanup, source exact head/tree, and a narrow TTL. Obtain independent approval on the exact authority head and verify it is actually enforced.
4. Source-side publisher must refuse export until it independently verifies all authorizations, exact Git tree, live PR status, origin policy consistency and destination preflight. Reject source/destination drift, additional files, symlinks or unreviewed change.
5. Never export private blobs merely because another org has Actions minutes. Only an independently approved private carrier with confirmed cleanup/retention bounds may receive them; no secrets, cross-org `GITHUB_TOKEN`, dynamic shell payloads, Actions artifact/cache persistence, or unchecked repo identity.
6. Only after those checks may the existing one-use dispatcher create the pinned source-bearing branch and authorization ref, run native code, verify actual GCC/Clang/CTest/sanitizer job steps, and clean source-bearing refs. Confirm cleanup, retention, no residual private refs, and match all results to exact head/tree.
7. Keep LLVM #732 draft until authentic native runs and required reviews are green; the public reference suite is separate evidence only.

## Current CI blocker

LLVM [exact-source workflow run 39481438560](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/39481438560) ended within approximately three seconds, with both GCC and Clang jobs assigned **no runner**, **no steps** and no retrievable job log. Neither checkout nor compilation occurred. This proves a pre-execution admission failure, **not** its root cause; organization Actions billing/spending, policy and runner eligibility still require an administrator to verify. The source-free carrier PR has real hosted verification evidence but is NOT authorization to export the private tree.

**No private-source bytes were transferred while preparing this proposal.**
