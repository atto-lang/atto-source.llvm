# Two source-defined Oreslang actors on Cortex-M3 — 2026-10-14

**Verified** with [private exact-source GitHub Actions run 38657315451](https://github.com/canonical-cloud/canonical-docs/actions/runs/38657315451)
on `canonical-cloud/canonical-docs:ci/ores-cortexm-static-actors-20261014`.
Source input was copied through the GitHub connector into a private branch;
no cross-organization checkout secrets or tokens were needed.

- Compiler source: `oreslang-source.llvm@2d4a3d8714b1abdc7f580d064655f7f8f92a2acf`.
- Embedded platform source: `oreslang-platform.embedded@2cbd4465df3822dd5207b5a562a81e3abbafac98`.
- Compiler host uses LLVM 18 + Clang 18, Cortex-M target `thumbv7m-none-eabi`.
- Built ARM ELF object containing source-generated actor constructors/handlers.
- Zephyr 4.3.0 linked `zephyr/zephyr.elf` and QEMU booted `qemu_cortex_m3`.
- Host static runtime C tests passed, 3/3 CTest tests in a sanitizer-backed build.
- Source actor admission and ARM IR tests passed.

Actual emulator output:

```text
ORESLANG_CORTEXM_ACTORS_PASS count=2 delivery=4 allocation=static
```

LLVM generated native constructors/handlers and the freestanding source bootstrap. The firmware checks both source actors' delivered message
counts and final persistent i64 results (Producer = 3; Consumer = 60),
zero remaining actor slots and no sticky runtime fault.

**Limits:** Source bootstrap can spawn, send literals and stop; handlers
can update their own scalar fields and return a scalar. This is not
handler-originated actor-to-actor messaging, async/await, actor groups,
supervision, MPU isolation, true concurrency, hardware testing or
a formal WCET guarantee. The adapter uses static storage and contains no
heap allocation calls. This alone does not prove the entire Zephyr system
is allocation-free or certify a measured RAM/stack budget.

Exact source revision scoped by this report: `2d4a3d8714b1abdc7f580d064655f7f8f92a2acf`.
