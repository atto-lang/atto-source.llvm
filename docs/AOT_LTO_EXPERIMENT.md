# Experimental host AOT LTO (issue #578)

The supported *single-source* guest subset can request a checked host linker experiment:

```sh
ORESLANG_AOT_LTO=off scripts/compile-aot.sh program.ores /tmp/program
ORESLANG_AOT_LTO=full CLANG=clang scripts/compile-aot.sh program.ores /tmp/program
ORESLANG_AOT_LTO=thin CLANG=clang scripts/compile-aot.sh program.ores /tmp/program
bash tests/test_aot_lto_driver.sh
```

`off` is the default and uses the existing `-O2` build. `full` requests `-flto -fuse-ld=lld`; `thin` requests `-flto=thin -fuse-ld=lld`. An actual compiler/linker canary must succeed; otherwise the command fails with exit 69 without replacing the last published executable. The smoke test **mocks** compiler/linker and proves argument/failure behavior only: it is not a measured cross-module optimization.

Follow-up: pin a real Clang+LLD matrix, add version/cmdline/hash evidence, compile and compare a genuine *multiple-admitted-unit* fixture once module lowering exists, verify codegen correctness, identical held-out workloads, toolchain compatibility and repeated performance measurements. Do not claim multi-module Oreslang LTO from this driver.
