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


## Executable guest-result evidence (new optional QEMU CTest)

The startup now calls the Oreslang-compiled scalar function with `9,4`,
expects the value `31`, and distinguishes its success and mismatch paths.
It invokes ARM semihosting solely for **test output and termination**; the
program does not require a host OS, Zephyr, libc, POSIX, or a runtime scheduler.
Semihosting is a test-observation dependency, not a freestanding `std`
service. The program is not a production MCU firmware image.

When `qemu-system-arm` is available, CMake adds
`cortexm_no_libc_source_guest_qemu`. That test:

1. Compiles the guest source through `oreslang-llvmc` and links its ARM ELF
   from repository-owned vector/reset and linker script.
2. Starts `qemu-system-arm -M lm3s6965evb -cpu cortex-m3` directly, without
   any Zephyr firmware or OS kernel.
3. Requires both a clean emulator exit and the guest-derived
   `ORESLANG_BAREMETAL_GUEST_PASS value=31` marker.
4. Rebuilds only the startup with a modified scalar input `9,5`, leaving
   the Oreslang guest object unchanged. Requires the mismatch marker and
   absence of the success marker to reject a fake unconditional print.

Run explicitly:

```sh
python3 tests/cortexm_no_libc_elf.py \
  --compiler /path/to/oreslang-llvmc \
  --qemu /path/to/qemu-system-arm
ctest --test-dir /path/to/build \
  -R 'cortexm_no_libc_(elf_link_and_symbol_audit|source_guest_qemu)' \
  --output-on-failure
```

If QEMU is absent, only the standalone ELF link/symbol CTest is registered;
**do not call this an emulator pass**. If a QEMU-enabled test times out,
misses its expected output, or reports a guest mismatch, it fails closed.

At the current revision, only local **synthetic LLVM IR** was assembled and
linked successfully. Neither actual `oreslang-llvmc` output nor this QEMU
guest run has executed, so both remain pending exact-head validation.

**Evidence boundaries:** local toolchain linking was tested using synthetic
LLVM IR, **not** the compiler-generated Oreslang guest. This PR's exact-head
CTests and source compilation have not yet executed on GitHub Actions. The original link-only test did not report guest results. The new reset handler
contains a guest-result check and semihosting observation, but no actual QEMU
execution evidence is available until an exact-head emulator run completes.
This is neither real MCU hardware nor actor/async/panic parity. The next gate must execute the actual linked guest under
QEMU or hardware and observe an independently checked result.
