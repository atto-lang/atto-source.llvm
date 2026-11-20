# Review-only: independent authorization for LLVM #1026 native macro CI

Status: **NO AUTHORIZATION TO EXPORT PRIVATE SOURCE** (November 2, 2026).

## Source and proposed private carrier

- Private source: `ores-truffle-oreslang/oreslang-source.llvm`.
- Exact candidate PR: [#1026](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1026),
  head `108340ad49dddeebf6e04748d6a63271db7170aa`,
  root Git tree `7a162398e64695a30ac52a9531471bbdcd124f00`.
- Live source main at audit:
  `b24b078267e4bc8cb0df5225a693c7c6ab902c35`,
  **protected=false**.
- Source `.github/private-ci-approved-repositories.txt`: no approved destination.
- Candidate private funded runner repo `scintilla-run/scintilla-e2e`,
  immutable GitHub repository ID observed `1346629495`. Merely using it
  for other projects is **not** approval for LLVM private source.
- Requested independent security reviewer: `@ORESoftware`.

## Hard conditions before transfer

1. Enforce protected source `main` with the independent review controls
   specified by `docs/security/CI_PRIVATE_SOURCE_MIRRORING.md`; no
   unprotected-source export fallback.
2. Record and independently review this LLVM-specific destination, exact
   numeric repository ID, destination visibility, trusted carrier workflow
   commit and blob, and source snapshot/test plan. Approve a policy change
   via protected source governance before any source-bearing operation.
3. Recheck review, identity and destination privacy immediately before
   exporting, then mirror the **entire exact PR head Git tree**, not an
   unverified subset. Create an isolated temporary branch via GitHub
   Git-data APIs, prove each Git blob and full subtree match and use
   `persist-credentials: false` in local destination checkout.
   Cross-organization private checkouts, PATs and secrets are prohibited.
4. Run actual CMake/CTest on the exact source head. Compile the production
   `oreslang-llvmc`, inspect typed MA001/MA002 AST expansion before IR,
   compile its actual emitted native IR via clang, link and run two guest
   programs returning 42, and reject dynamic Const, overflow, unsupported
   templates/captures and partial IR. Verify limits for 2048 visited
   constant AST nodes, 64 depth, eight declarations and 128 expansions.
5. Record run ID, attempt, Git head/root tree, job IDs, executed step list,
   native test totals, LLVM/clang versions and exact binary artifact hashes.
   `steps=null` is runner admission failure and conveys **zero** compiler
   or guest-AOT validation.

## No implicit release qualification

A funded carrier, passing source-free runner test or previously approved
Cranelift/Java transfer never authorizes LLVM source transfer or establishes
LLVM macro guest compilation. Neither this document nor a synthetic
evidence report changes source policy. Any transfer must remain blocked
until source protections and independent approvals become operational.

This proposal only adds review documentation. It does **not** add a carrier
allowlist entry, modify source enforcement, transfer source, merge a PR or
assert a native compiler test pass.
