# Cortex-M3 source-to-firmware verification — 2026-10-14

- Compiler source under test: `oreslang-source.llvm@ba8552bc9ca8cc4e9edc3b7f665ed2a1ed9a5b39`
- Embedded app under test: `oreslang-platform.embedded@5705a25b0dc5ee9ddb1a7c15ebe747142d51975c`
- Private, exact-source test mirror: `canonical-cloud/canonical-docs`, branch `ci/ores-cortexm-aot-20261014`
- [Successful full workflow](https://github.com/canonical-cloud/canonical-docs/actions/runs/38646752678).
- Compiler: C++17 `oreslang-llvmc` built against LLVM 18.1.3, Clang 18.
- Target: `thumbv7m-none-eabi`, ARMv7-M soft-float AAPCS32, Zephyr 4.3.0 `qemu_cortex_m3`.
- Evidence: CMake executed the Oreslang compiler on the real `.ores` fixture, compiled IR into an ARM ELF object with Clang, linked `zephyr/zephyr.elf`, and booted QEMU.
- Console: `*** Booting Zephyr OS build v4.3.0 ***` followed by `ORESLANG_CORTEXM_AOT_PASS value=42`.
- Compiler negative cases reject native `main`, unsupported locals, unsupported target and owned-int experiment.

**Boundary:** this verifies scalar function execution only. It does not verify
source actors, Zephyr MPU boundaries, interrupts, native error/suspension, GC,
full OresVM services, timing guarantees, or on-device hardware.
