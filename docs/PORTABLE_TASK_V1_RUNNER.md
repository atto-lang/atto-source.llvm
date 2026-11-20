# Portable Task v1 — LLVM native guest runner integration (candidate)

This integration adds **an actual compiler/guest invocation entrypoint**, not a claim of passing guest Task semantics. The shared **candidate** suite is in [platform-conformance PR #465](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/465), pinned here for review to `844c8d35b39d818cf319e5c181f3e13d3d50d1df` at time of writing (subsequent suite head changes must be explicitly re-pinned).

```bash
export ATTO_TASK_SUITE_ROOT=/absolute/path/to/authorized/oreslang-platform-conformance
export ATTO_TASK_SUITE_SHA="$(git -C "$ATTO_TASK_SUITE_ROOT" rev-parse HEAD)"
# First build build/oreslang-llvmc and build/oreslang-no-gc-ir-audit, install clang
bash scripts/run-portable-task-v1-guest.sh
```

The wrapper refuses an absent suite, SHA mismatch, unavailable compiler, or missing build tools; it does not clone a private repo, dispatch Actions, export source, or change branch refs. The shared runner verifies byte-identical fixture Git blobs and compiler/fixture commits, then uses the existing `scripts/compile-aot.sh` frontend/IR/native link and resultant executable with no host-model substitution. Every unsupported fixture is a visible gap; a parser failure cannot satisfy an expected semantic rejection without a named `ATTO_TASK_E_*` diagnostic.

**Not yet executed in source-local CI:** this new wrapper is only a reviewed entrypoint. A zero-step workflow is NOT_EXECUTED. Host-only Task tests and script syntax checks are not guest parity. To release-gate a run, attach exact compiler commit, fixture commit+blob, executable identity, actual completed job steps, native/Truffle guest results and reviewer confirmation. `oreslang-llvmc` SHA-256 is compiler/runtime identity, not proof of a fully linked AOT guest artifact. Cross-org mirrors require separate security approval; no implied authority is granted.
