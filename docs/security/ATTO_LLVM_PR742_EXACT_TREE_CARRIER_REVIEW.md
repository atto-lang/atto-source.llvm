# LLVM PR #742 — independent private CI carrier approval request

**Status: REVIEW ONLY. NOT APPROVED. NO PRIVATE GIT OBJECT MAY BE EXPORTED ON THE BASIS OF THIS FILE.**

This request accompanies [LLVM PR #742](https://github.com/atto-lang/atto-source.llvm/pull/742) and [source security issue #233](https://github.com/atto-lang/atto-source.llvm/issues/233). It is deliberately maintained on a **separate review branch**, so it does not mutate PR #742's exact tested head. It is not the protected-branch allowlist, is not an executable export permit, and cannot bypass the source-side independent review or retention requirements.

## Immutable source identity for independent review

| Field | Requested reviewed value |
| --- | --- |
| Source repository | `atto-lang/atto-source.llvm` (private) |
| Source numeric repository ID | `1448700962` |
| Source PR | `742` |
| Source PR commit | `5ca234e3637dd3c3c00093c8c78c63f898d470c9` |
| Source root Git-tree SHA | `0876a80e4702529583ff7d919202a5ed766da5eb` |
| Recursive tree | 1,915 entries, `truncated:false` |
| File blobs | 1,824: 1,810 mode `100644`, 14 mode `100755` |
| Directories | 91 |
| Symlinks/gitlinks | 0 / 0 |
| Independent export authorization | **PENDING** |
| Fresh private carrier with executed steps | **NOT VERIFIED** |

The source head, source root, and destination must be re-read immediately before export. Any source-head/tree drift invalidates this request; a PR-controlled manifest is *never its own export authority*. Copying a subset of files does not establish an exact Git-tree mirror. The destination root SHA, all Git blob OIDs, file paths, executable modes, symlinks and gitlinks must match the independently approved source snapshot.

## Carrier candidates are NOT approved for this PR

Existing source-main `.github/private-ci-approved-repositories.txt` lists `streempilot-test/browser-studio-e2e`, but the protected-source `.github/private-ci-carrier-contracts.json` binds that carrier to test plan `ci/test-plans/llvm-pr787.json`, **not PR #742**. Do not substitute a repository allowlist for a PR-specific, immutable independently reviewed plan.

* [streempilot-test/browser-studio-e2e run 44954699842](https://github.com/streempilot-test/browser-studio-e2e/actions/runs/44954699842): private, zero executed steps.
* [gha-indie-worker/gha-indie-worker-e2e run 44957101583](https://github.com/gha-indie-worker/gha-indie-worker-e2e/actions/runs/44957101583): **public**, three executed steps, GCC/Clang synthetic source-free C11 program only, commit `81532e9f905ebefecd00bccbdffc7147af8e2365`. **NOT a private budget or export authorization.**
* [gha-indie-worker/atto-ci run 44957202225](https://github.com/gha-indie-worker/atto-ci/actions/runs/44957202225): **private**, zero executed steps, destination repo ID `1458328770`; no source copied.
* Other tested private organizations have similarly failed zero-step source-free admission canaries. Their Actions failures are infrastructure evidence, not compiler test failures.

The available GitHub App API cannot inspect private Actions billing configuration or recover runner-start annotations for these zero-step jobs. A GitHub organization/repository administrator must review the literal job-start error, payment/spending limits, disabled Actions policy, assigned runner labels, OS/plan eligibility and repository fork/PR restrictions. Do **not** assert a specific exhausted-minute cause without admin evidence.

## Independently authorized transfer preconditions — all required

1. The source/security owner independently identifies **one private destination** by exact `owner/repo` and numeric repository ID, with permission to store the full source tree and a documented reader/retention/deletion policy. A public GitHub repository is **never** a destination.
2. A **fresh source-free workflow on that same private destination** actually executes a named GCC/Clang command. A public org peer's success is insufficient.
3. An independent reviewer (for example `@ORESoftware`) attests to source-main protection, source-side export policy, the exact source repo ID/commit/tree and destination numeric ID, the immutable **PR #742-specific** test plan and workflow blobs, and outstanding cleanup/retention constraints. Merely creating or merging this review document is not attestation.
4. Only after every source-side permission is effective, the publisher copies the complete source Git tree via the GitHub connector into a *temporary private test branch*, without PATs, personal secrets, credentials, cross-org `GITHUB_TOKEN` checkout or public logs/artifacts containing source. Destination workflow uses its local `GITHUB_TOKEN` for its own checkout.
5. Before execution, require the exact approved destination tree SHA and compare **every blob path, file mode and Git object SHA** to the source; fail closed if any differ. If the destination workflow needs custom scripts, they must be independently reviewed and non-source changes must not be silently mixed into the mirrored root.
6. Run real backend tests with executed steps and store narrowly scoped receipts; review source retention including leftover Git blobs in history, caches, logs and artifacts. Deleting a temporary branch **does not purge Git objects**.

## Required release and sanitizer execution receipts

Use the unchanged [source-actor workflow](../../.github/workflows/source-actor-owned-drop.yml) and [native control workflow](../../.github/workflows/control-plane-ref32-c11.yml) from the exact approved source root.

* Native host control: GCC strict C11 optimized and Clang ASan/UBSan, including the deterministic **50,000-operation** lifetime model, issuer saturation, stale/foreign references and 128-bit coherence. The source test blob is `1fd941af03831b621b83cdd36d948766cfe2770e`; host-only local GCC/Clang already passed, but exact-head Actions execution has **zero steps**.
* Real compiled guest: build `oreslang-llvmc` from the approved source, execute `tests/source_actor_e2e.py` using the actual compiler and Clang in release and ASan/UBSan modes, and demand the specific canonical `source-actor-two-owned-heaps.atto` receipt. Both `.atto` and legacy `.ores` fixture Git blobs are `cce7728c1682bbf2600af65d6ceda9071fe7aa0a`; both must compile and execute separately.
* Validate actor terminal receipts `accepted = processed + abandoned` and separately report actual processed message count. **Admission and STOP alone cannot prove that an actor handler ran**, because early STOP can abandon a queued DATA message. Do not promote a spawn/stop-only fixture to message-processing parity.
* Classify `ActorOwnedI64Heap` guest execution separately from `ControlPlaneArena32` host access: neither implies actual guest-level i128 control-arena lowering.
* Fail if a required step was skipped, marker absent/duplicated, current head changed, source tree differs, compiler binary did not execute, sanitizer failed, or the test consumed unapproved private source.

**Pending independent decisions:** private runner admission, protected source and private destination permission, per-PR742 immutable plan, security-owner approval, retention/cleanup. **No full compiler guest CI execution, Java/.NET parity or process-wide zero-GC certification is claimed.**
