#!/usr/bin/env python3
"""Oreslang source -> standalone Cortex-M ELF, with no implicit libc/runtime.

This is an executable-link and symbol admission gate; QEMU boot is separate.
It deliberately does not link Zephyr, crt0, libc, libgcc or an actor runtime.
"""
import argparse
import pathlib
import shutil
import struct
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
  @ 3 * 9 + 4 = 31. Only guest-compiled code can provide this result.
  cmp r0, #31
  bne guest_fail
guest_pass:
  ldr r1, =pass_message
  movs r0, #4
  bkpt #0xab
  ldr r1, =exit_success
  movs r0, #0x20
  bkpt #0xab
stop_success:
  b stop_success
guest_fail:
  ldr r1, =fail_message
  movs r0, #4
  bkpt #0xab
  ldr r1, =exit_failure
  movs r0, #0x20
  bkpt #0xab
stop_failure:
  b stop_failure
.section .rodata,"a",%progbits
.balign 4
pass_message: .asciz "ORESLANG_BAREMETAL_GUEST_PASS value=31\\n"
fail_message: .asciz "ORESLANG_BAREMETAL_GUEST_FAIL\\n"
.balign 4
exit_success: .word 0x20026, 0
exit_failure: .word 0x20023, 1
"""

LINKER = """ENTRY(Reset_Handler)
MEMORY {
 FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
 RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
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



# The lm3s6965evb board emulated by QEMU has exactly 256 KiB of flash
# at 0x00000000 and 64 KiB of SRAM at 0x20000000. An out-of-range initial
# SP can link successfully but fault as soon as guest code/semihosting
# touches the stack.
FLASH_BEGIN = 0x00000000
FLASH_END = 0x00040000
RAM_BEGIN = 0x20000000
RAM_END = 0x20010000


def verify_vector_table(elf_path):
    raw = elf_path.read_bytes()
    verify(len(raw) >= 52 and raw[:6] == b"\x7fELF\x01\x01",
           "expected ELF32 little-endian firmware")
    verify(struct.unpack_from("<H", raw, 16)[0] == 2,
           "expected ET_EXEC standalone firmware")
    verify(struct.unpack_from("<H", raw, 18)[0] == 40,
           "expected EM_ARM firmware")
    phoff = struct.unpack_from("<I", raw, 28)[0]
    phentsize, count = struct.unpack_from("<HH", raw, 42)
    verify(phentsize >= 32 and 0 < count <= 32,
           "ELF program headers are missing or malformed")
    verify(phoff <= len(raw) and count <= (len(raw)-phoff)//phentsize,
           "ELF program headers exceed file bounds")
    flash_load = False
    for i in range(count):
        off = phoff + i * phentsize
        kind, file_off, vaddr, paddr, filesz, memsz = struct.unpack_from(
            "<IIIIII", raw, off)
        if kind != 1:  # PT_LOAD
            continue
        verify(file_off <= len(raw) and filesz <= len(raw)-file_off,
               "truncated firmware load segment")
        verify(memsz >= filesz, "firmware PT_LOAD memsz smaller than filesz")
        verify((FLASH_BEGIN <= vaddr and vaddr + memsz <= FLASH_END)
               or (RAM_BEGIN <= vaddr and vaddr + memsz <= RAM_END),
               "ELF PT_LOAD outside physical Cortex-M3 board memory")
        # No RAM initializers or zeroing runtime has been implemented.
        # A scalar guest needing .data/.bss would otherwise link and
        # silently run with undefined startup state.
        if RAM_BEGIN <= vaddr < RAM_END:
            verify(memsz == 0,
                   "freestanding scalar startup cannot initialize RAM PT_LOAD")
        if vaddr == FLASH_BEGIN:
            verify(filesz >= 8, "vector table is not part of the flash image")
            sp, reset = struct.unpack_from("<II", raw, file_off)
            verify(sp == RAM_END and sp % 8 == 0,
                   "reset stack pointer outside QEMU board RAM")
            verify(reset & 1 and FLASH_BEGIN < (reset & ~1) < FLASH_END,
                   "reset vector must be an in-flash Thumb code address")
            flash_load = True
    verify(flash_load, "no flash PT_LOAD contains the reset vector")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--readelf", default=shutil.which("readelf"))
    parser.add_argument("--qemu", default=None,
                        help="optional qemu-system-arm executable; required for guest execution")
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
             "-Wl,--no-undefined",
             str(root / "guest.o"), str(root / "startup.o"),
             "-Wl,-T," + str(root / "linker.ld"), "-o", str(elf)])

        header = run([args.readelf, "-h", str(elf)])
        symbols = run([args.readelf, "-Ws", str(elf)])
        sections = run([args.readelf, "-SW", str(elf)])
        program = run([args.readelf, "-lW", str(elf)])
        dynamic = run([args.readelf, "-d", str(elf)])
        verify_vector_table(elf)

        # Self-test the independently parsed ELF vector gate: a valid link
        # must be distinguished from corrupted or out-of-board firmware.
        original = elf.read_bytes()
        phoff = struct.unpack_from("<I", original, 28)[0]
        entsize, phcount = struct.unpack_from("<HH", original, 42)
        vectors_offset = None
        for i in range(phcount):
            off = phoff + i * entsize
            kind, file_off, vaddr = struct.unpack_from("<III", original, off)
            if kind == 1 and vaddr == FLASH_BEGIN:
                vectors_offset = file_off
                break
        verify(vectors_offset is not None, "missing vector-test segment")
        for label, delta, value, diagnostic in (
                ("invalid-sp", 0, RAM_END + 0x10000, "reset stack pointer"),
                ("invalid-reset", 4, 0, "reset vector")):
            mutated = bytearray(original)
            struct.pack_into("<I", mutated, vectors_offset + delta, value)
            corrupt_path = root / (label + ".elf")
            corrupt_path.write_bytes(mutated)
            try:
                verify_vector_table(corrupt_path)
            except RuntimeError as err:
                verify(diagnostic in str(err),
                       "ELF vector audit rejected wrong reason: " + str(err))
            else:
                raise RuntimeError("ELF vector audit admitted " + label)
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

        if args.qemu:
            # The positive guest must return 3*9+4 = 31. The negative build
            # changes an assembly input to 5 without changing the compiled
            # guest: the same runtime must take the failure path instead of
            # blindly producing the success token.
            def qemu_execute(firmware):
                command = [args.qemu, "-M", "lm3s6965evb", "-cpu", "cortex-m3",
                           "-nographic", "-monitor", "none", "-serial", "none",
                           "-semihosting-config", "enable=on,target=native",
                           "-kernel", str(firmware)]
                try:
                    result = subprocess.run(command, capture_output=True,
                                            text=True, timeout=20)
                except subprocess.TimeoutExpired as exc:
                    raise RuntimeError("QEMU did not exit (guest trap/spin)") from exc
                return result.returncode, result.stdout + result.stderr

            code, output = qemu_execute(elf)
            verify(code == 0 and "ORESLANG_BAREMETAL_GUEST_PASS value=31" in output,
                   "source-compiled guest did not complete correctly: " + output)
            verify("ORESLANG_BAREMETAL_GUEST_FAIL" not in output,
                   "positive guest reported result mismatch: " + output)

            (root / "startup-negative.S").write_text(
                STARTUP.replace("movs r1, #4", "movs r1, #5", 1))
            run(common + ["-c", str(root / "startup-negative.S"), "-o",
                          str(root / "startup-negative.o")])
            negative_elf = root / "negative.elf"
            run([args.clang, "--target=thumbv7m-none-eabi",
                 "-fuse-ld=lld", "-nostdlib", "-nodefaultlibs",
                 "-Wl,--no-undefined",
                 str(root / "guest.o"), str(root / "startup-negative.o"),
                 "-Wl,-T," + str(root / "linker.ld"),
                 "-o", str(negative_elf)])
            neg_code, neg_output = qemu_execute(negative_elf)
            verify(neg_code != 0
                   and "ORESLANG_BAREMETAL_GUEST_FAIL" in neg_output
                   and "ORESLANG_BAREMETAL_GUEST_PASS" not in neg_output,
                   "negative guest did not take mismatch branch: "
                   + str(neg_code) + " " + neg_output)
            print("Oreslang Cortex-M no-OS QEMU positive/negative guest PASS")

    print("Oreslang Cortex-M standalone no-libc ELF link and symbol gate PASS")
    if not args.qemu:
        print("Scope: compiler + object + standalone link; QEMU not requested")


if __name__ == "__main__":
    main()
