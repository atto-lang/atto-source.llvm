# LLVM exact-source ephemeral runner and V3 regression handoff

This workflow is an **opt-in manual job in the original private source repository**, not a private-source carrier, GitHub Actions runner installer, or release attestation.

## Provisioned by an authorized organization administrator

Use a fresh disposable Linux x64 VM for **one job only**, registered directly to `ores-truffle-oreslang` and restricted through a source-only runner group to `oreslang-source.llvm`. Give it an exclusive label `oreslang-llvm-ephemeral-v1`. Preinstall the GitHub Actions runner, Git, `gh`, Python 3, C/C++ compiler, CMake, Clang and LLVM development libraries; the workflow does not invoke `sudo`, install packages or require a personal access token.

The one-time registration token must remain outside the guest worker. Use a low-privilege networking boundary and **no reusable credentials or repository secrets** on the runner. Ensure logs are forwarded to trusted storage; de-register and destroy the VM immediately after its job. Never use persistent multi-job runners for PR-supplied compiler sources.

The trusted workflow must be independently reviewed and merged into `main` before it becomes dispatchable. Do not enable export publisher V3 while `require_atomic_ref_rollback()` unconditionally denies; incident #1150 (retained Git objects), race issue #1154, protected source-main and approved-destination policy remain independent hard gates.

## CGL native Linux compatibility scope

The optional `cgl-compat` suite runs only against an **open, independently approved CGL implementation PR head** in the same private source repository. Do **not** dispatch this suite for #1220 itself or actor-only #1149; their trees do not contain the CGL build/test files. For example, draft [CGL integration #1200](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1200) currently contains the four CMake target names and required CTest registrations; independently review its **current exact SHA**, and repeat inventory checks if its head changes.

The suite runs the actual ELF negative preflight unit tests with assertions enabled, builds `oreslang-llvmc` and all three CGL host/dispatch/trace native targets, executes four named CTests, and uses the native smoke script to compile `.ores` into a Linux ELF program that must exit 42. With `run_sanitizers=true` it separately compiles and executes three linked native CGL tests under Clang ASan/UBSan. Its trusted-main offline regression asserts that real preflight, CTest, ELF smoke and sanitizer commands remain present. Missing target/source files or absent named CTests fail closed. A passing suite proves **Linux native compatibility subset** only, not carrier-grade realtime, PREEMPT_RT latency, CPU isolation, deadline admission, full actor timing guarantees, or production certification. The manual workflow's status is **not** automatically attached to the tested PR SHA.

## Dispatch via the main branch only

Actions -> **LLVM actor and V3 source-exact ephemeral gate** -> Run workflow on `main`. Specify an **open same-repository PR** and its immutable current head SHA; stacked PRs work.

Choose `security-v3` for LLVM security draft #1146. It runs the repository's real full publisher negative/unit test script and two source-free gate suites, with **no export operations**.

Choose `actor-llvm` for the compiled actor draft #1195. The job builds `oreslang-llvmc` from exactly that commit, compiles its Oreslang actor source fixtures to LLVM IR, uses Clang to link the native scheduler, executes the resulting guest programs and optionally repeats under ASan/UBSan. All artifact inputs are from the exact checked-out source and toolchain.

Choose `actor-ledger` for draft #1149 after an independent ORESoftware approval **at the exact current #1149 head**. It first executes the checked-in `scripts/ci/run_actor_ledger_native_exact_head.sh` with the pinned SHA. This enforces a clean checkout and source blob identity before compiling the **real** native scheduler, actor-ledger STOP/WAKE, retained DATA, actor-owned physical heap, pinned loan and cleanup fixtures under GCC, Clang and Clang ASan/UBSan. It then builds the actual LLVM Oreslang compiler, runs the normal source-actor guest suite, and runs `tests/native_actor_compiled_guest_ledger.py` to compile and execute genuine ledger guest fixtures (and optionally guest ASan/UBSan). Native sanitizers are **mandatory** for this suite regardless of the optional guest-sanitizer toggle. The result remains `NOT_EXECUTED` until the disposable source-org runner actually executes all required steps. A manual `workflow_dispatch` run is not automatically a required status on the PR's commit.

The workflow first checks out the **trusted, dispatch-pinned protected `main` SHA** to execute its own source-free authorization mocks. This must occur **before** checking out the untrusted exact PR source: stacked actor PR #1149 does not contain the handoff's mock script or workflow. Both checkouts use `persist-credentials: false`, and the protected-main source helper and workflow blobs are checked against their checkout HEAD. After switching to the exact PR SHA, `actor-ledger` executes the real scheduler/heap native suite **before any CMake build writes untracked files**. The native runner intentionally refuses dirty worktrees; putting it after `cmake -B build-source-exact` would deterministically fail the admission check. The source-free regression now enforces both order constraints.

The job first calls GitHub API with a transient read-only `GITHUB_TOKEN` solely to bind PR id/head/repo/private state. Token is **not** passed to the test steps; checkout uses `persist-credentials: false`. A passing job is source-head scoped and proves only the selected suite. An absent runner is **NOT_EXECUTED**; a synthetic provider Git-lease test is not a substitute.

**Prerequisites not supplied here:** organization administrator runner registration, runner group restrictions, disposable VM policy, branch protection and independent reviewer approval.

## Important: dispatch evidence is not a required PR-head status

GitHub associates a manually dispatched workflow run with the selected **main** workflow ref; checking out a different immutable PR SHA inside the job does **not** automatically attach this job's status check to that PR commit. Keep required status checks on a genuinely PR-head-triggered workflow once private GitHub Actions admission is restored, or obtain independent source-SHA receipt verification and explicit security signoff. Never auto-merge the actor PR solely because this manual job passed.

The source SHA, Git root tree, exact executed test step names and job/runner identity must be matched out of band before claiming backend execution. A missing runner, skipped job, incomplete job or zero-step failure is **NOT_EXECUTED**.

## Preflight now fails closed until protected main and reviewed PR head

Before checking out any untrusted PR source, the workflow uses its read-only GitHub token to verify that the source organization's `main` branch reports `protected: true`. Merely dispatching from `main` is insufficient. The workflow also reads the most recent **submitted** review by the dedicated `ORESoftware` identity (up to 99 records) and requires `APPROVED` at exactly the requested PR-head commit, with reviewer and author identities distinct. Missing/ambiguous/oversized review histories, stale commits, changes requested after approval, dismissal, unprotected main, and mismatched source repository all deny execution before checkout.

Currently this means the new workflow **cannot execute**: source `main` is still unprotected, and the relevant PRs have no submitted independent approvals. The admin must first apply and verify repository protection, then a reviewer must approve the *current* tested code head. If the PR changes afterward, the approval must be renewed. This is an intentional security prerequisite, not a runner admission failure. Do not disable this gate to force execution.

An approval obtained through a separate manual process must be independently checked against the exact GitHub PR head and reviewer identity. GitHub's commit-associated manual `workflow_dispatch` check still does not replace branch-protection required checks on the PR head.

## Administrator JIT runner registration (local, off-Actions)

After protected `main`, independent workflow review, and a repo-restricted runner group are **actually enforced**, an authorized administrator may execute `scripts/ci/prepare_source_ephemeral_jit.sh` on a trusted **operator workstation**, not inside Actions. The script requires the organization's exact selected-repository runner-group ID and an *existing empty mode-0700 directory* for output. It checks that the group permits precisely this repository and this workflow at `@refs/heads/main`, refuses public repositories, and requires the protected source `main` and reviewed workflow to exist.

Example operator commands (illustrative only, not executed):

```bash
mkdir -m 700 -p "$HOME/.oreslang-one-job-runner"
bash scripts/ci/prepare_source_ephemeral_jit.sh RUNNER_GROUP_ID \
  "$HOME/.oreslang-one-job-runner/one-time-jit-config.b64"
```

Use an independently authenticated `gh` identity with organization **Self-hosted runners (write)** permission, scoped for this one administrative operation. **Never** put the administrative token in the runner VM, GitHub job secrets or logs. The script sends a single authorized `POST /orgs/ores-truffle-oreslang/actions/runners/generate-jitconfig` request only *after* all local policy checks pass; it writes the returned encoded JIT configuration to a newly created mode-0600 file, and prints no credential. Deliver the configuration securely to a dedicated, freshly provisioned, isolated Linux x64 runner VM with a verified GitHub Actions runner installation.

Run the official runner with the one-time `--jitconfig` value only on that disposable guest; never emit the value through shell tracing, command logging or CI summaries. Enforce single-job deletion, external runner-log forwarding, VM destruction and credential/configuration-file disposal. Beware that operating-system process argument inspection may expose a JIT argument on a multi-user host, so use **single-tenant isolated machines**. The runner label must be `oreslang-llvm-ephemeral-v1` and the GitHub runner group must restrict both repository and workflow. No mixed/reused job environments are permitted.

Official GitHub references: https://docs.github.com/en/rest/actions/self-hosted-runners and https://docs.github.com/en/actions/reference/security/secure-use and https://docs.github.com/en/rest/actions/self-hosted-runner-groups.

**Validation limit:** The JIT script's shell syntax and 15 source-free local Bash test cases have been executed per backend (12 independent denial cases, inside-Actions denial, secure one-time creation, and no overwrite). This does **not** confirm real organization's runner-group ID, plan eligibility, runner-image integrity, review enforcement, JIT registration, source JUnit/native execution or real GitHub management writes. No runner or secret was provisioned by this PR.

## Source-free regression command

On a trusted workstation or any ordinary CI job that does not receive an administrative GitHub token, verify the JIT provisioning fail-closed logic with the checked-in, **fully mocked** CLI:

```bash
bash -n scripts/ci/prepare_source_ephemeral_jit.sh
bash -n scripts/ci/test_prepare_source_ephemeral_jit.sh
bash scripts/ci/test_prepare_source_ephemeral_jit.sh scripts/ci/prepare_source_ephemeral_jit.sh
```

The test harness replaces `gh` with a local stub. It exercises unprotected main, overbroad runner groups, extra repository visibility, failed POST, attempts to provision inside Actions, successful 0600 credential-file creation, and refusal to overwrite an existing config. **It neither creates a real runner nor uses the private-source publisher.**

### Additional admission security checks

The administrative helper does not accept a GitHub branch response containing only `protected: true` as sufficient. It reads live source-main branch protection and requires enabled independent CODEOWNER review, stale-review dismissal, administrator enforcement, and no force-push, branch deletion, or explicit PR-review bypass. It refuses to generate a JIT credential if any required control is absent. It additionally computes `git hash-object` of its own local script and checks it against the reviewed immutable blob at `/scripts/ci/prepare_source_ephemeral_jit.sh` on protected `main`, so an unreviewed or modified workstation copy cannot generate the runner credential. Treat the local helper as untrusted until this byte-exact check succeeds.

**Executed local evidence:** the committed script and checked-in mock test both matched their GitHub blob SHA exactly; `bash -n` passed and the test runner returned `SOURCEFREE_MOCK_JIT_ADMISSION=PASS`. No live runner-authorization API calls were made. This does not authorize executing PR code without independent approval.

## Trusted-main filter-free checkout admission

The verifier is copied via `git cat-file blob HEAD:scripts/ci/verify_llvm_exact_checkout.py`
from independently reviewed protected `main` into `$RUNNER_TEMP` **before**
the PR checkout, then executed from that trusted copy after the PR checkout.
Main-file hash checks use `git hash-object --no-filters`, not a filter-applying
Git operation. The verifier uses Git raw tree/index metadata and batched
raw blob reads, no-follow regular file opening, and rejects index/content/mode
drift, untracked and ignored files, symlinks, gitlinks, FIFOs and hardlinks.
The 13 synthetic Git tests include an actively malicious clean filter: the
verifier does not execute it, while `git diff` does.

This is **post-checkout** integrity evidence only. The checkout operation
itself must run with sanitized Git config, no reusable secrets and a disposable
source-organization runner. Filesystem mutation after the check is not
prevented. No run, source transfer, compiler execution or export approval
is inferred from the source-free tests.

## Pre-materialization smudge-filter isolation

The PR-object fetch uses pinned `actions/checkout@11bd719...` with
`sparse-checkout-cone-mode: false` and **the two literal sparse patterns**
`/*` then `!/*`: all tracked files are excluded *before Git checkout*.
The explicit `blob:limit=134217728` partial-clone filter fetches the blobs
within the trusted materializer's 128 MiB per-blob budget, without leaving a
persisted checkout token; out-of-budget and missing blobs deny execution.

Trusted-main source-free tests validate this Git behavior against a malicious
configured smudge filter, including binary bytes, executable bits, symlinks,
pre-existing unexpected paths, hostile Git environment overrides and direct
post-materialization execution of the exact-checkout verifier. The trusted
materializer is copied by `git cat-file blob HEAD:...` to `$RUNNER_TEMP`
*before* fetching the PR. It refuses to run unless the source checkout contains
**only** its real `.git` directory and every indexed source entry is
skip-worktree. It then writes regular source files directly from Git blob bytes,
using no-follow/exclusive filesystem operations and strict object budgets.
Only then does it clear index skip-worktree bits and run the existing
raw-blob integrity verifier.

A later unauthorized filesystem change or an attacker who compromises the
disposable VM is outside this preflight's guarantee. This mechanism does not
authorize cross-organization export or runner registration. Admin-protected
source main, independently approved exact PR head and source-local disposable
runner remain mandatory.
