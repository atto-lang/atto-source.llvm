# Credential-free exact-source CI acceptance gate

Use this gate for cross-organization CI mirrors of private Oreslang source trees.
It is a proof checker for *fetched GitHub REST evidence*, not a GitHub runner,
a mirror transport, or a substitute for comprehensive tests.

## Input artifacts

Use the connected GitHub app's approved GET endpoints to export JSON, not a
personal access token, private-repo URL embedded in a test workflow, or
cross-org `GITHUB_TOKEN`.

1. `source-tree.json`: GET `/repos/SOURCE_ORG/SOURCE_REPO/git/trees/PR_HEAD_SHA?recursive=1`.
2. `mirror-tree.json`: GET `/repos/FUNDED_ORG/FUNDED_REPO/git/trees/MIRROR_HEAD_SHA?recursive=1`.
3. `workflow-evidence.json`: an object with `run` (GET `/repos/.../actions/runs/ID`)
   and `jobs` (the array returned by GET `/repos/.../actions/runs/ID/jobs`).
   Save a separate record for each workflow that must pass.

Verify each returned `truncated` field is false, then run:

```sh
python3 scripts/exact_source_gate.py \
  --source-tree source-tree.json \
  --mirror-tree mirror-tree.json \
  --mirror-prefix oreslang-java-mirror/ \
  --mirror-head MIRROR_COMMIT_SHA \
  --workflow-evidence maven-evidence.json \
  --require-job exact-java \
  --require-step 'Run full Maven verification'
```

For a native LLVM mirror, require its actual CMake, CTest and sanitizer job
names and executed step names; do **not** substitute a successful Java smoke
test for LLVM verification. Add `--workflow-evidence`, `--require-job`, and
`--require-step` multiple times as needed. Step names must match the actual
run's jobs and successful steps exactly.

## Fail-closed properties

The verifier rejects missing/extra files, changed blob hashes, changed
executable/symlink/submodule modes, truncated source trees, stale runs,
workflow failures, skipped jobs, and zero-step infrastructure failures.
It compares every file under the mirror prefix, not a hand-picked list.

The input JSON must come from GitHub with trusted provenance. The checker
cannot authenticate fabricated JSON and cannot prove tests have correct
assertions merely because their *named* steps succeeded. CI reviewers still
must inspect code, dependencies, test quality, permissions, cancellation and
ownership correctness. Avoid using GitHub Actions status-only checks as
substitutes for the full green code-path test suite.

**Important:** make a new snapshot if either PR source head or integration
branch changes. A successfully tested older mirror does not certify newer
source code.

Local regression suite:

```sh
python3 -m unittest discover -s tests -p 'test_exact_source_gate.py' -v
```

Job receipts must include the GitHub job ID, run ID, run attempt and head SHA. Each job must belong to the supplied run and attempt; duplicate job IDs and jobs borrowed from other runs are rejected. Fetch the run and jobs from the same authenticated GitHub Actions run endpoint.
