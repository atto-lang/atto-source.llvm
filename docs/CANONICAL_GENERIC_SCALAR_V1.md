# Canonical scalar generics: LLVM consumer

The **sole normative test corpus** is [oreslang-common-source](https://github.com/ores-truffle-oreslang/oreslang-common-source/tree/0e9ee65a3877fa92bcef238234d8380fc5de0967/contracts/v1/generic-scalar), merged in PR #32. Do not copy its 20 fixtures into this repository. The local `contracts/v1/generics-stage1` six-case manifest and `tests/native_scalar_generic_source_execution.py` are **legacy LLVM regression tests**, not the cross-backend language contract. A passing legacy suite must not substitute for the canonical corpus.

The executable consumer at `scripts/verify_canonical_generic_scalar.py` requires the common-source checkout at exact commit `0e9ee65a3877fa92bcef238234d8380fc5de0967` (root tree `858c37d48a4c3a886260b26320bb0dd931b228ab`) and canonical contract tree `fd5301ebd354049917e2ecaf1da920b661d6d829`. It also verifies the common verifier's SHA-256 over manifest+fixtures: `80a5053f8ba147346793a209e4f8ec04b8daf4cedb6a149fb1d127ea4af4ffb5`, exactly 20 cases. The Git checkout must be clean; code does not pull, overwrite, or download sources or authentication credentials.

```bash
# In an independently checked-out, pinned oreslang-common-source directory:
git -C ../oreslang-common-source checkout --detach 0e9ee65a3877fa92bcef238234d8380fc5de0967

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc --parallel 2

python3 scripts/verify_canonical_generic_scalar.py \
  --common-source-root ../oreslang-common-source \
  --compiler ./build/oreslang-llvmc --clang "$(command -v clang)"
```

The runner executes the **canonical** `verify.py --backend llvm`, compiling to real LLVM IR, linking with Clang, executing each guest and checking negative diagnostics. All 20 LLVM cases are required; missing, mutated, or untrusted external corpus bytes fail closed. If your GitHub Actions organization has no runner allocation, use a **private** carrier with a SHA-identical `common` subtree and an LLVM source tree mirrored through the GitHub Git object API; do not transfer tokens/secrets. A successful carrier run is evidence about exact source, not a replacement for required origin branch policies.

Stage-two `Option<T>` / `Result<T,E>` / collection or actor generics must not be certified by this int64-only stage-one corpus. Add separate normative fixtures and implement ownership and actor-isolation proofs first.
