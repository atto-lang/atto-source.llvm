# Experimental host AOT LTO (issue #578)

The supported *single-source* guest subset can request a checked host linker experiment:

```sh
ORESLANG_AOT_LTO=off scripts/compile-aot.sh program.ores /tmp/program
ORESLANG_AOT_LTO=full CLANG=clang scripts/compile-aot.sh program.ores /tmp/program
ORESLANG_AOT_LTO=thin CLANG=clang scripts/compile-aot.sh program.ores /tmp/program
bash tests/test_aot_lto_driver.sh
bash tests/test_aot_lto_real_toolchain.sh
```

`off` is the default and uses the existing `-O2` build. `full` requests `-flto -fuse-ld=lld`; `thin` requests `-flto=thin -fuse-ld=lld`. For LTO, the driver compiles and links **two separate, synthetic LLVM IR modules** via the actual requested Clang/LLD combination and executes a fixed-result canary before compiling the guest. Any compilation, linking, or execution mismatch fails with exit 69, preserving the previously published executable. This cannot prove the toolchain actually inlined any guest functions. It is a host-only test; do not treat a non-executable cross-compiled artifact as a successful host canary.

`test_aot_lto_driver.sh` tests failure and flag behavior against a mock Clang. `test_aot_lto_real_toolchain.sh` tests off/full/thin with a **real** Clang/LLD installation but a synthetic LLVM IR-producing compiler fixture, not the Oreslang compiler. A passing workflow proves the requested host linker combination works on that runner—not Oreslang source-module LTO, a benchmark or compiler parity.

Follow-up: pin a real Clang+LLD matrix, add version/cmdline/hash evidence, compile and compare a genuine *multiple-admitted-unit* fixture once module lowering exists, verify codegen correctness, identical held-out workloads, toolchain compatibility and repeated performance measurements. Do not claim multi-module Oreslang LTO from this driver.
