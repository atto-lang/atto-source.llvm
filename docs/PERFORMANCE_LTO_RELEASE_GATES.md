# LLVM LTO and ThinLTO — experimental release contract

Status: **experimental opt-in driver support on this draft branch**. Full and ThinLTO flags are wired into the single-source native AOT driver. This is not cross-Oreslang-module LTO, and no speedup has been measured.

## Supported initial slice

The original two-argument driver remains unchanged:

```sh
# Requires native oreslang-llvmc and compatible Clang.
scripts/compile-aot.sh tests/fixtures/forward-call-main.ores /tmp/ores-aot
ORESLANG_LTO=full scripts/compile-aot.sh tests/fixtures/forward-call-main.ores /tmp/ores-aot-full
ORESLANG_LTO=thin scripts/compile-aot.sh tests/fixtures/forward-call-main.ores /tmp/ores-aot-thin
```

- `ORESLANG_LTO=off` is the default and uses the original `clang -O2` invocation.
- `full` and `thin` require Clang plus LLD and pass `-O2 -flto=full|thin -fuse-ld=lld`.
- Invalid modes and missing LLD reject the build; a failed compile/link never overwrites the previous published executable.
- The current Oreslang compiler emits **one** verified IR module for one admitted source file. Flags alone do not demonstrate inter-Oreslang-module inlining or optimization across libraries.
- The real compiler E2E test in `tests/test_aot_driver.py` exercises both modes when LLD exists, entrypoint exit 42, invalid mode rejection and atomic publication on unsupported guest sources. The opt-in modes were also link-smoke-tested locally with a **fake** IR emitter, which verifies driver plumbing but NOT real Oreslang parity.

## Next milestones

- [x] Opt-in LTO modes with fail-closed toolchain preflight and atomic existing CLI behavior (branch implementation)
- [x] Extend real-source AOT E2E test for full/ThinLTO, conditionally when LLD installed (test added; CI confirmation outstanding)
- [ ] Support at least two separately verified Oreslang translation units with an explicit closed-world ABI and linkage manifest.
- [ ] Use compatible bitcode outputs and linker diagnostics to demonstrate real cross-unit inlining/unused-symbol stripping.
- [ ] Pin and record toolchain versions, source SHA, IR/bitcode hashes, compile/link time and artifact size.
- [ ] Capture held-out 5+ process runs under equivalent conditions; compare `off`, `full`, `thin` with actual source-generated code.
- [ ] Run ownership, actor isolation, effects and import-admission regressions before any default mode change.

## Evidence policy

Document source SHA, IR hashes, bitcode hashes, Clang/LLD versions, flags, host/CPU, link time, peak RSS, binary bytes, observed optimization remarks and correctness statuses. A single LLVM IR module or a dummy second object is **not** proof of cross-module Oreslang support. No performance claims without real measurements.

Tracking: https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/578
