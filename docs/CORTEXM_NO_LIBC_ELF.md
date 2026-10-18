# Cortex-M source-compiled no-libc ELF link gate

This is a strictly narrower stage than full Oreslang `no_std` runtime support.

The test `tests/cortexm_no_libc_elf.py` runs the existing Oreslang
Cortex-M scalar compiler on a guest `.ores` fixture, creates an ARM ELF
object using Clang, supplies a **repository-owned assembly reset entry point**
and minimal linker script, and links a standalone executable with
`-nostdlib -nodefaultlibs`. It must not link Zephyr, crt0, glibc, libgcc,
the hosted actor runtime or a hidden C `main`.

Evidence required from the final ELF:
- `readelf -h` identifies an executable ARM ELF (not a shared library).
- `Reset_Handler` and `ores_embedded_probe` exist.
- No `PT_INTERP`, dynamic-loader section or DT_NEEDED dependencies.
- No **named undefined symbols**, including `__aeabi_unwind_cpp_pr0`.
- No libc allocation, threading, printing or exception runtime symbols.
- A vector section and guest text section exist.

ARM object generation may produce an `.ARM.exidx` section even if no
external runtime is needed. Presence alone is **not** a failure. The
contract rejects undefined unwind personalities, and compiler flags disable
exceptions and unnecessary unwind tables. This distinction was verified in
a local Clang 17 + LLD synthetic-IR experiment: default code generation
left `__aeabi_unwind_cpp_pr0` undefined, while no-unwind options removed
that dependency; the static exidx section remained.

Run after building `oreslang-llvmc`:

```sh
python3 tests/cortexm_no_libc_elf.py --compiler /path/to/oreslang-llvmc
ctest --test-dir /path/to/build -R cortexm_no_libc_elf_link_and_symbol_audit --output-on-failure
```

**Evidence boundaries:** local toolchain linking was tested using synthetic
LLVM IR, **not** the compiler-generated Oreslang guest. This PR's exact-head
CTests and source compilation have not yet executed on GitHub Actions. The
fixture loops after reset; it does not report a guest result and is **not**
QEMU boot, semihosted exit, MCU hardware, actor, panic/exception or runtime
execution evidence. The next gate must execute the actual linked guest under
QEMU or hardware and observe an independently checked result.
