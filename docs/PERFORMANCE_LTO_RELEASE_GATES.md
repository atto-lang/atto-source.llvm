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

## October 18 source-free linkage audit

- Registered `tests/test_lto_driver_smoke.py` in `CMakeLists.txt` as `native_lto_driver_source_free_smoke`. Its stub IR emitter is explicitly isolated from the existing real-guest `native_aot_driver` test. CTest source-free success may never count as Oreslang compiler execution.
- Ran the exact checked-in shell driver and new Python smoke test locally on Linux with real Clang/LLD: `off`, `full`, and `thin` modes produced a native executable exiting 42. Deliberately invalid IR, invalid mode, and same input/output were rejected; failed recompilation preserved the prior binary and removed temporary workdirs. Verified that the local files' Git blob hashes match the PR files exactly.
- Host clang/linker availability is not Oreslang source parity. Real `oreslang-llvmc` compiler CTest remains unverified because the source organization Actions fails with `steps:null` (no recorded executed steps). No speedup or cross-unit LTO is claimed.
- Private cross-org source export is security-blocked by platform-conformance PR #86 and incidents #22/#77. Do not use a private test carrier or a generic connector Git-tree mirror without independently reviewed protected-main export authorization and incident clearance, regardless of apparent Actions capacity.

## October 18 — alias-safe atomic AOT publication

Source-to-output lexical path comparison is not sufficient when either side is a symbolic link or hard link to the same inode. The AOT script now also rejects `source_file -ef output_file` **before invoking the compiler**, preventing accidental replacement of guest source through an alias. The source-free CTest fixture now explicitly exercises source→output and output→source symlink aliases and input/output hardlink aliases, verifying exit 64, source preservation, old native binary preservation and no lingering staging directories.

A local execution of the **exact Git blob** `scripts/compile-aot.sh` (verified by `git hash-object`) passed the source-free real-Clang/LLD test in off/full/thin modes, rejected invalid guest IR while preserving the previous binary, and rejected all three alias forms. A synthetic IR emitter was used: this does **not** constitute real Oreslang source compilation or compiler correctness. Source-org Actions still has no executed steps, so hosted exact-head CI remains unverified.
