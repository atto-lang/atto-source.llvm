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
  @ Source Oreslang int is i64 on thumbv7m-none-eabi. AAPCS32 passes
  @ first i64 in r0:r1, second i64 in r2:r3; return i64 in r0:r1.
  movs r0, #9
  movs r1, #0
  movs r2, #4
  movs r3, #0
  bl ores_embedded_probe
  @ 3 * 9 + 4 = 31. Verify BOTH halves of the 64-bit result.
  cmp r1, #0
  bne guest_fail
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
    entry = struct.unpack_from("<I", raw, 24)[0]
    phoff = struct.unpack_from("<I", raw, 28)[0]
    phentsize, count = struct.unpack_from("<HH", raw, 42)
    verify(phentsize >= 32 and 0 < count <= 32,
           "ELF program headers are missing or malformed")
    verify(phoff <= len(raw) and count <= (len(raw)-phoff)//phentsize,
           "ELF program headers exceed file bounds")
    flash_load = False
    seen_loads = []
    executable = []
    reset_address = None
    for i in range(count):
        off = phoff + i * phentsize
        kind, file_off, vaddr, paddr, filesz, memsz, flags, alignment = (
            struct.unpack_from("<IIIIIIII", raw, off))
        # Reject loader-managed program headers in the *binary*, not only
        # human-readable readelf output. A freestanding ELF cannot depend on
        # PT_INTERP / PT_DYNAMIC, even with ordinary-looking load segments.
        if kind in (2, 3):
            raise RuntimeError("hosted ELF interpreter/dynamic-loader program header forbidden")
        if kind != 1:  # PT_LOAD
            continue
        verify(file_off <= len(raw) and filesz <= len(raw)-file_off,
               "truncated firmware load segment")
        verify(memsz >= filesz, "firmware PT_LOAD memsz smaller than filesz")
        verify(paddr == vaddr,
               "firmware physical load address differs from virtual address")
        verify((FLASH_BEGIN <= paddr and paddr + memsz <= FLASH_END)
               or (RAM_BEGIN <= paddr and paddr + memsz <= RAM_END),
               "ELF PT_LOAD outside physical Cortex-M3 board memory")
        verify(not (flags & 0x1 and flags & 0x2),
               "writable executable ELF PT_LOAD is forbidden")
        for other_start, other_end in seen_loads:
            if memsz and other_end > other_start:
                verify(paddr >= other_end or paddr + memsz <= other_start,
                       "overlapping firmware PT_LOAD segments")
        seen_loads.append((paddr, paddr + memsz))
        if flags & 0x1 and filesz:
            executable.append((vaddr, vaddr + filesz))
        # Without .data copying and .bss zeroing in the minimal startup,
        # RAM load segments cannot be treated as working freestanding code.
        if RAM_BEGIN <= paddr < RAM_END:
            verify(memsz == 0,
                   "freestanding scalar startup cannot initialize RAM PT_LOAD")
        if paddr == FLASH_BEGIN:
            verify(not flash_load, "duplicate vector-table flash PT_LOAD")
            verify(filesz >= 8, "vector table is not part of the flash image")
            sp, reset = struct.unpack_from("<II", raw, file_off)
            verify(sp == RAM_END and sp % 8 == 0,
                   "reset stack pointer outside QEMU board RAM")
            verify(reset & 1 and FLASH_BEGIN < (reset & ~1) < FLASH_END,
                   "reset vector must be an in-flash Thumb code address")
            reset_address = reset
            flash_load = True
    verify(flash_load, "no flash PT_LOAD contains the reset vector")
    verify(any(lo <= (reset_address & ~1) < hi for lo, hi in executable),
           "reset vector does not point into an executable load segment")
    verify(entry == reset_address,
           "ELF entry point must match the Thumb reset vector")

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
        verify("define i64 @ores_embedded_probe(i64 %a, i64 %b)" in ir,
               "Cortex-M Oreslang int ABI must be i64 parameters and return")
        verify("define i64 @sum(i64 %a, i64 %b)" in ir,
               "Cortex-M helper must retain the i64 int ABI")
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
        vector_phoff = None
        text_phoff = None
        for i in range(phcount):
            off = phoff + i * entsize
            kind, file_off, vaddr, paddr, size, memsz, flags, alignment = (
                struct.unpack_from("<IIIIIIII", original, off))
            if kind != 1:
                continue
            if paddr == FLASH_BEGIN:
                vectors_offset, vector_phoff = file_off, off
            if flags & 0x1 and text_phoff is None:
                text_phoff = off
        verify(vectors_offset is not None and text_phoff is not None,
               "missing vector/text self-test segments")
        # The corruption harness must prove that a superficially valid ELF
        # cannot smuggle a different physical image, entrypoint or permissions.
        mutations = (
            ("invalid-sp", vectors_offset, RAM_END + 0x10000,
             "reset stack pointer"),
            ("invalid-reset", vectors_offset + 4, 0, "reset vector"),
            ("nonexec-reset", vectors_offset + 4, FLASH_END - 3,
             "not point into an executable"),
            ("wrong-physical-address", vector_phoff + 12, FLASH_END + 4,
             "physical load address"),
            ("mismatched-entry", 24, 0x101,
             "ELF entry point must match"),
            ("writable-executable", text_phoff + 24, 7,
             "writable executable"),
            ("forged-interpreter", vector_phoff, 3,
             "interpreter/dynamic-loader program header forbidden"),
            ("forged-dynamic", vector_phoff, 2,
             "interpreter/dynamic-loader program header forbidden"),
        )
        for label, offset, value, diagnostic in mutations:
            mutated = bytearray(original)
            struct.pack_into("<I", mutated, offset, value)
            corrupt_path = root / (label + ".elf")
            corrupt_path.write_bytes(mutated)
            try:
                verify_vector_table(corrupt_path)
            except RuntimeError as err:
                verify(diagnostic in str(err),
                       "ELF audit rejected " + label + " for wrong reason: " + str(err))
            else:
                raise RuntimeError("ELF audit admitted " + label)
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
                STARTUP.replace("movs r2, #4", "movs r2, #5", 1))
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
            # Exercise *nonzero high halves* of both i64 arguments and the
            # i64 return. 3*(2**32+9) + (2**32+4) = 4*2**32 + 31.
            # The previous 9,4 case would not detect a lost high register.
            wide_source = STARTUP
            for old, new in (
                    ("movs r1, #0", "movs r1, #1"),
                    ("movs r3, #0", "movs r3, #1"),
                    ("cmp r1, #0", "cmp r1, #4"),
                    ("ORESLANG_BAREMETAL_GUEST_PASS value=31",
                     "ORESLANG_BAREMETAL_GUEST_PASS value=17179869215")):
                verify(wide_source.count(old) == 1,
                       "wide AAPCS32 ABI fixture mismatch: " + old)
                wide_source = wide_source.replace(old, new, 1)
            (root / "startup-wide.S").write_text(wide_source)
            run(common + ["-c", str(root / "startup-wide.S"), "-o",
                          str(root / "startup-wide.o")])
            wide_elf = root / "wide.elf"
            run([args.clang, "--target=thumbv7m-none-eabi",
                 "-fuse-ld=lld", "-nostdlib", "-nodefaultlibs",
                 "-Wl,--no-undefined",
                 str(root / "guest.o"), str(root / "startup-wide.o"),
                 "-Wl,-T," + str(root / "linker.ld"),
                 "-o", str(wide_elf)])
            verify_vector_table(wide_elf)
            wide_code, wide_output = qemu_execute(wide_elf)
            verify(wide_code == 0 and
                   "ORESLANG_BAREMETAL_GUEST_PASS value=17179869215"
                   in wide_output and "ORESLANG_BAREMETAL_GUEST_FAIL"
                   not in wide_output,
                   "QEMU failed nonzero-high i64 argument/return case: " + wide_output)
            # Signed and cross-word carry cases exercise the ABI independently
            # of the common zero-upper-half path, without i64 overflow.
            scalar_call = """  movs r0, #9
  movs r1, #0
  movs r2, #4
  movs r3, #0
  bl ores_embedded_probe
  @ 3 * 9 + 4 = 31. Verify BOTH halves of the 64-bit result.
  cmp r1, #0
  bne guest_fail
  cmp r0, #31
  bne guest_fail"""
            verify(STARTUP.count(scalar_call) == 1,
                   "baseline AAPCS32 startup instructions changed unexpectedly")
            for case, left, right in (
                    ("signed_negative", -3, 4),
                    ("crossword_carry", 0xffffffff, 0xffffffff),
                    ("negative_high_word", -(2**32 + 2), 2**32 + 5)):
                result = 3 * left + right
                verify(-(1 << 63) <= result < (1 << 63),
                       "test must not rely on unspecified signed i64 overflow")
                word_mask = (1 << 32) - 1
                def halves(value):
                    value &= (1 << 64) - 1
                    return value & word_mask, value >> 32
                alo, ahi = halves(left)
                blo, bhi = halves(right)
                rlo, rhi = halves(result)
                call = "\n".join((
                    "  ldr r0, =0x%08x" % alo,
                    "  ldr r1, =0x%08x" % ahi,
                    "  ldr r2, =0x%08x" % blo,
                    "  ldr r3, =0x%08x" % bhi,
                    "  bl ores_embedded_probe",
                    "  ldr r12, =0x%08x" % rhi,
                    "  cmp r1, r12",
                    "  bne guest_fail",
                    "  ldr r12, =0x%08x" % rlo,
                    "  cmp r0, r12",
                    "  bne guest_fail"))
                label = "ORESLANG_BAREMETAL_GUEST_PASS case=" + case
                custom = STARTUP.replace(scalar_call, call, 1)
                custom = custom.replace(
                    "ORESLANG_BAREMETAL_GUEST_PASS value=31", label, 1)
                source_path = root / ("startup-" + case + ".S")
                obj_path = root / ("startup-" + case + ".o")
                image = root / (case + ".elf")
                source_path.write_text(custom)
                run(common + ["-c", str(source_path), "-o", str(obj_path)])
                run([args.clang, "--target=thumbv7m-none-eabi",
                     "-fuse-ld=lld", "-nostdlib", "-nodefaultlibs",
                     "-Wl,--no-undefined", str(root / "guest.o"),
                     str(obj_path), "-Wl,-T," + str(root / "linker.ld"),
                     "-o", str(image)])
                verify_vector_table(image)
                status, observed = qemu_execute(image)
                verify(status == 0 and label in observed
                       and "ORESLANG_BAREMETAL_GUEST_FAIL" not in observed,
                       "QEMU failed signed/carry i64 case " + case
                       + ": " + observed)
            print("Oreslang Cortex-M i64 signed/carry cases PASS (3/3)")
            print("Oreslang Cortex-M i64 high-register ABI proof PASS")
            print("Oreslang Cortex-M no-OS QEMU positive/negative guest PASS")

    print("Oreslang Cortex-M standalone no-libc ELF link and symbol gate PASS")
    if not args.qemu:
        print("Scope: compiler + object + standalone link; QEMU not requested")


if __name__ == "__main__":
    main()
