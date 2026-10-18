# LLVM LTO and ThinLTO — experimental release contract

Status: **planning document**; LTO is not enabled. The current `scripts/compile-aot.sh` accepts one source and emits IR, then calls Clang `-O2`; cross-module Oreslang source resolution is not implied.

## Requirements
1. Preserve the current 2-argument single-file CLI and its atomic output publication (a failed build must retain the previous executable).
2. A future opt-in driver accepts exact verified units and an explicit `off|full|thin` LTO mode. The default stays `off` until semantics and evidence pass.
3. Require a compatible, pinned compiler + Clang + linker toolchain, explicit binary and symbol ABI, and reproducible flags. An unavailable ThinLTO cache/linker is a hard `UNSUPPORTED` error when requested.
4. The optimizer must never turn unsupported guest constructs into implicit fallbacks, dynamically load arbitrary modules or cross actor/tenant isolation boundaries.
5. Preserve entrypoint exit-code semantics, source admission, overflow behavior, private symbols and explicit exports.

## Implementation milestones
- [ ] Add toolchain preflight and opt-in mode parser with invalid-input tests
- [ ] Emit valid bitcode from verified IR and test Clang/LLD compatibility
- [ ] Once compiler multi-unit support exists, link two genuine independent units through a controlled symbol interface
- [ ] Observe cross-unit inlining/dead-code elimination via optimization remarks / IR / disassembly; don't rely only on smaller file size
- [ ] Run ABI/correctness cases, link failures, missing symbols and atomic-publish regressions
- [ ] Benchmark clean `-O2`, full LTO, ThinLTO under matched build/runtime conditions

## Evidence template
Store source SHA, IR hashes, bitcode hashes, Clang/LLD versions, flags, host/CPU, link time, peak RSS, executable bytes, 5+ runtime samples, observed inlining and correctness statuses. A dummy second object is **not** proof of cross-unit Oreslang support.

Tracking: https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/578
