# LLVM exact-source ephemeral runner and V3 regression handoff

This workflow is an **opt-in manual job in the original private source repository**, not a private-source carrier, GitHub Actions runner installer, or release attestation.

## Provisioned by an authorized organization administrator

Use a fresh disposable Linux x64 VM for **one job only**, registered directly to `ores-truffle-oreslang` and restricted through a source-only runner group to `oreslang-source.llvm`. Give it an exclusive label `oreslang-llvm-ephemeral-v1`. Preinstall the GitHub Actions runner, Git, `gh`, Python 3, C/C++ compiler, CMake, Clang and LLVM development libraries; the workflow does not invoke `sudo`, install packages or require a personal access token.

The one-time registration token must remain outside the guest worker. Use a low-privilege networking boundary and **no reusable credentials or repository secrets** on the runner. Ensure logs are forwarded to trusted storage; de-register and destroy the VM immediately after its job. Never use persistent multi-job runners for PR-supplied compiler sources.

The trusted workflow must be independently reviewed and merged into `main` before it becomes dispatchable. Do not enable export publisher V3 while `require_atomic_ref_rollback()` unconditionally denies; incident #1150 (retained Git objects), race issue #1154, protected source-main and approved-destination policy remain independent hard gates.

## Dispatch via the main branch only

Actions -> **LLVM actor and V3 source-exact ephemeral gate** -> Run workflow on `main`. Specify an **open same-repository PR** and its immutable current head SHA; stacked PRs work.

Choose `security-v3` for LLVM security draft #1146. It runs the repository's real full publisher negative/unit test script and two source-free gate suites, with **no export operations**.

Choose `actor-llvm` for the compiled actor draft #1195. The job builds `oreslang-llvmc` from exactly that commit, compiles its Oreslang actor source fixtures to LLVM IR, uses Clang to link the native scheduler, executes the resulting guest programs and optionally repeats under ASan/UBSan. All artifact inputs are from the exact checked-out source and toolchain.

The job first calls GitHub API with a transient read-only `GITHUB_TOKEN` solely to bind PR id/head/repo/private state. Token is **not** passed to the test steps; checkout uses `persist-credentials: false`. A passing job is source-head scoped and proves only the selected suite. An absent runner is **NOT_EXECUTED**; a synthetic provider Git-lease test is not a substitute.

**Prerequisites not supplied here:** organization administrator runner registration, runner group restrictions, disposable VM policy, branch protection and independent reviewer approval.
