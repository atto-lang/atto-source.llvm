# Private CI carrier candidate for LLVM PR #931 — HOLD

Status: **NON-AUTHORIZING / REVIEW ONLY**

This document records a source-free private carrier candidate for
`ores-truffle-oreslang/oreslang-source.llvm` PR #931.

It does **not** modify either authoritative authorization ledger:

- `.github/private-ci-approved-repositories.txt` remains unchanged and empty.
- `.github/private-ci-carrier-contracts.json` remains unchanged.

No private Oreslang source may be exported on the basis of this document.

## Candidate carrier

- Repository: `ores-otel-test/next-loggers-runtime-conformance`
- Repository ID: `1328290859`
- Visibility observed: private
- Default branch: `main`
- Candidate carrier PR: `#13`
- Last fully-green source-free candidate snapshot: `41ffba3b7af1f3c2892045326b1602a052713710`
- Candidate base `main`: `d182db2edab44ac422c85c1a7fa3b12844b95e6f`
- Workflow blob at that snapshot: `a3b70b07d3366316741e41c54773939ec2967f49`
- LLVM #931 test-plan blob at that snapshot: `e5dbdda7955db22f82ff2cd13a3aba328e2a5356`

The verifier and trusted-plan runner are copied from the reviewed v3 carrier
template `opto-sync-test/go-java-validation@f8e9f99f6c1be2a20350fe65d1a553e342fe4266`.

## Source-free execution evidence

Carrier PR #13 triggered the source-free security workflow:

- workflow run: `40138013497`
- verifier job: `121003575474`
- result: success
- executed steps included:
  - source-free verifier regression suite
  - source-free trusted-plan runner regression suite
  - source-free live carrier-boundary regression suite
  - source-free LLVM #931 test-plan audit
- source-bearing `verify-carrier` job: skipped
- cleanup job: skipped
- complete ordinary-workflow isolation audit: passed
- workflow-isolation manifest blob:
  `1dcd1e2a51e019097888b36aa7afd1a14dffa894`
- workflow-isolation audit script blob:
  `e62fcbb54fa19254246b9377d7ca54a42333de52`
- live-boundary verifier blob:
  `879ddb8b91a7c9d6283651f4e3eb029312a2a807`
- LLVM #931 plan-audit blob:
  `d8267c5c1388b5c5dca54be72a67be91f0631f99`

This proves the candidate carrier controls can execute source-free. It is not
compiler/runtime evidence and is not authorization to export source.

The candidate pins the complete ordinary workflow inventory inherited from
carrier `main`. The source-free isolation audit fails closed on workflow
addition/removal, blob drift, or a trigger change that could cause an ordinary
workflow to consume a raw `ci/oreslang-*` source-bearing branch. The reviewed
inventory contains only PR-only, workflow-call-only, tag-only, `main`-push,
or unrelated path-filtered push workflows.

## Proposed #931 test plan after authorization

The reviewed-shape test plan permits only source repository
`ores-truffle-oreslang/oreslang-source.llvm` and PR `931`.

It requires:

1. CMake configure in `RelWithDebInfo`.
2. Full native build.
3. Focused lifecycle/Future/CONTROL CTest selection, including:
   - native readiness Future release + ASan/UBSan;
   - source actor `get_done()`;
   - stop-before-ready normal + ASan/UBSan + TSan;
   - continuation READY/cancel/claim normal + ASan/UBSan + TSan;
   - VM main CONTROL bootstrap/shutdown;
   - injected CONTROL pthread-create failure/retry;
   - scalar Future single-winner/teardown/callback tests.
4. Full CTest suite with output on failure.

## Current live coarse protection

Authenticated branch metadata on 2026-10-24 reports:

- source `ores-truffle-oreslang/oreslang-source.llvm/main`:
  `protected=false`, SHA `a80fe00c5a68b8232e21cad0b58fd4858fa9ec53`;
- carrier `ores-otel-test/next-loggers-runtime-conformance/main`:
  `protected=false`, SHA `d182db2edab44ac422c85c1a7fa3b12844b95e6f`.

Either value independently blocks export. The connected GitHub App cannot read
the detailed branch-protection endpoint (HTTP 403), so detailed enforcement
cannot be certified from this session even after the coarse flag becomes true.

## Latest immutable source-free carrier receipt

The candidate PR is actively evolving, so authorization review must never
implicitly follow its moving head. The last fully-green source-free snapshot
recorded here is `41ffba3b7af1f3c2892045326b1602a052713710`.

Exact evidence for that immutable snapshot:

- private-carrier security run `40249641772`: success;
- verifier job `121338519173`: success with real executed steps;
- generic CI run `40249641777`: success;
- Zed `pack & r2g` job `121338741662`: success;
- package checker: `[zed-conformance] PASS: 17/17 checks passed`;
- `r2g` packed 601 files, installed into a mock consumer, and passed the
  existing artifact smoke test;
- `r2g installed consumer`, `Polyglot packaging`, and `ores-lint`:
  success;
- source-bearing `verify-carrier`: skipped;
- source-bearing cleanup: skipped.

Exact blobs at that snapshot:

- CODEOWNERS: `00409bc79ab2c4f40fa020e7388d36cd3bf5926f`;
- workflow: `a3b70b07d3366316741e41c54773939ec2967f49`;
- LLVM #931 test plan: `e5dbdda7955db22f82ff2cd13a3aba328e2a5356`;
- workflow-isolation manifest:
  `1dcd1e2a51e019097888b36aa7afd1a14dffa894`;
- Zed conformance checker:
  `e55e7f2d513ddc0af86f81e624ffad39baf70c92`.

Any newer carrier head must independently re-pass the required controls before
it can replace this receipt. This receipt is source-free capability evidence,
not private-source transfer authorization.


## Blocking gates

Every item below must be independently verified before the authoritative
carrier contract or allowlist may be changed:

- platform-conformance #22 is CLOSED with `state_reason=completed`;
- platform-conformance #77 is CLOSED with `state_reason=completed`;
- platform-conformance #127 is CLOSED with `state_reason=completed`;
- source `main` live protection satisfies the v3 policy gate;
- carrier `main` live protection satisfies the same gate;
- carrier PR #13 receives independent security review and is merged;
- resulting carrier `main` SHA and workflow/test-plan blobs are re-read;
- a separate protected-source review updates
  `.github/private-ci-carrier-contracts.json`;
- a separate protected-source review updates
  `.github/private-ci-approved-repositories.txt`.

Until then the trusted publisher must fail closed before reading or writing
private source bytes. Connector/API Git object writes are not a bypass.
