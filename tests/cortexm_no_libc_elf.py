#!/usr/bin/env python3
"""Oreslang source -> standalone Cortex-M ELF, with no implicit libc/runtime.

This is an executable-link and symbol admission gate; QEMU boot is separate.
It deliberately does not link Zephyr, crt0, libc, libgcc or an actor runtime.
"""
import argparse
import pathlib
import shutil
import subprocess
import tempfile


def run(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError("command failed: " + " ".join(map(str, argv)) +
                           "\n" + result.stdout + "\n" + result.stderr)
    return result.stdout


def verify(cond, reason):
    if not cond:
        raise RuntimeError(reason)


STARTUP = """.syntax unified
.cpu cortex-m3
.thumb
.section .isr_vector,"a",%progbits
.word __stack_top
.word Reset_Handler
.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
.thumb_func
Reset_Handler:
  movs r0, #9
  movs r1, #4
  bl ores_embedded_probe
halt:
  b halt
"""

LINKER = """ENTRY(Reset_Handler)
MEMORY {
 FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 512K
 RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
__stack_top = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
 .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
 .text : { *(.text*) *(.rodata*) } > FLASH
 .ARM.exidx : { *(.ARM.exidx*) } > FLASH
 .data : { *(.data*) } > RAM AT > FLASH
 .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
}
"""

GUEST = """pub fnc ores_embedded_probe(int a, int b): int {
    return sum(a * 3, b);
}
fnc sum(int a, int b): int {
    return a + b;
}
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--readelf", default=shutil.which("readelf"))
    args = parser.parse_args()
    verify(bool(args.clang), "Clang with an ARM backend required")
    verify(bool(args.readelf), "readelf required: no skipping ELF symbol audit")

    with tempfile.TemporaryDirectory(prefix="ores-no-libc-cortexm-") as tmp:
        root = pathlib.Path(tmp)
        guest = root / "probe.ores"
        guest.write_text(GUEST)
        ir = run([args.compiler, "--target=thumbv7m-none-eabi", str(guest)])
        verify('target triple = "thumbv7m-none-eabi"' in ir,
               "compiler did not select the Cortex-M target")
        verify("@ores_embedded_probe" in ir,
               "Oreslang guest probe was not emitted")
        verify("define i32 @main" not in ir,
               "guest must not emit a hosted main()")
        (root / "guest.ll").write_text(ir)
        (root / "startup.S").write_text(STARTUP)
        (root / "linker.ld").write_text(LINKER)

        common = [args.clang, "--target=thumbv7m-none-eabi",
                  "-mcpu=cortex-m3", "-mthumb", "-ffreestanding",
                  "-fno-exceptions", "-fno-unwind-tables",
                  "-fno-asynchronous-unwind-tables"]
        run(common + ["-O2", "-x", "ir", "-c", str(root / "guest.ll"),
                      "-o", str(root / "guest.o")])
        run(common + ["-c", str(root / "startup.S"), "-o",
                      str(root / "startup.o")])
        elf = root / "baremetal.elf"
        run([args.clang, "--target=thumbv7m-none-eabi",
             "-fuse-ld=lld", "-nostdlib", "-nodefaultlibs",
             str(root / "guest.o"), str(root / "startup.o"),
             "-Wl,-T," + str(root / "linker.ld"), "-o", str(elf)])

        header = run([args.readelf, "-h", str(elf)])
        symbols = run([args.readelf, "-Ws", str(elf)])
        sections = run([args.readelf, "-SW", str(elf)])
        program = run([args.readelf, "-lW", str(elf)])
        dynamic = run([args.readelf, "-d", str(elf)])
        verify("Machine:                           ARM" in header,
               "bare-metal ELF has wrong architecture")
        verify("EXEC (Executable file)" in header,
               "bare-metal ELF is not a static executable")
        verify("Reset_Handler" in symbols and "ores_embedded_probe" in symbols,
               "missing owned startup or Oreslang guest symbol")
        verify("INTERP" not in program and "DYNAMIC" not in program,
               "hosted ELF interpreter/dynamic-loader section detected")
        verify("There is no dynamic section" in dynamic,
               "dynamic dependency section present")
        # ARM ELF may retain linker-generated .ARM.exidx with static
        # cantunwind entries; this is not itself a libc/OS dependency.
        # A *named undefined unwind personality* is disallowed below.
        verify(".isr_vector" in sections and ".text" in sections,
               "missing bare-metal vector table or guest text sections")

        # The ELF null symbol is UND with no name and is harmless; every
        # *named* undefined symbol is a disallowed missing runtime dependency.
        undefined = []
        for line in symbols.splitlines():
            columns = line.split()
            if len(columns) >= 8 and columns[6] == "UND":
                undefined.append(columns[-1])
        verify(not undefined,
               "unresolved bare-metal symbols: " + ", ".join(undefined))
        for blocked in ("malloc", "calloc", "free", "memcpy", "printf",
                        "__aeabi_unwind_cpp_pr0", "__cxa_", "pthread_"):
            verify(blocked not in symbols,
                   "unexpected hosted/runtime symbol: " + blocked)

    print("Oreslang Cortex-M standalone no-libc ELF link and symbol gate PASS")
    print("Scope: compiler + object + standalone link; no emulator execution claimed")


if __name__ == "__main__":
    main()
