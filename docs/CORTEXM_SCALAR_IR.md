# Experimental Cortex-M3 scalar target

`oreslang-llvmc --target=thumbv7m-none-eabi input.ores` emits verified
LLVM IR with a Cortex-M3-compatible little-endian 32-bit ARM target triple
and datalayout. Use a Clang toolchain with ARM backend to generate the object.

This is an intentionally **narrow guest native-AOT** target:
- only top-level `int` functions with primitive value arguments and return;
- constant arithmetic, param references and verified direct function calls;
- no main() guest entry wrapper (Zephyr supplies main);
- no actors, classes, locals, branches, throwing, borrowing, async,
  ownership regions, imports, or dynamic runtime services;
- unsupported code must fail before emitting LLVM IR.

The `cortexm_freestanding_source_object` compiler test creates a real
32-bit EM_ARM object via Clang and verifies reject-path diagnostics, but
is **not** Zephyr firmware boot evidence. Firmware assembly, Zephyr and QEMU
are owned by `oreslang-platform.embedded`.

The target ABI table labels this `experimental-cortexm-scalar-ir`;
no broad ARM native runtime parity is implied. Compiler and platform
target profiles must be versioned before expanding support.

## Static resource admission (audit 2026-10-14)

The Cortex-M target rejects source larger than 128 KiB, more than 64 top-level
functions, more than 8 parameters/arguments per function/call, more than 4096
expression nodes, nesting deeper than 32 AST edges, call paths deeper than 16,
and recursion (including mutual recursion). Distinct overloads by name are
not admitted to keep the embedded call graph unambiguous. These are coarse
compiler resource controls, **not** a proof of a precise stack/flash upper
bound; inspect Zephyr map/stack usage before claiming a memory budget.
