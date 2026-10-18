#!/usr/bin/env python3
"""Compile and execute standalone Cortex-M C actor kernel firmware.

These are C actor callbacks, not compiler-generated Oreslang actor handlers.
"""
import argparse
import pathlib
import shutil
import struct
import subprocess
import tempfile

FLASH_END = 0x40000
RAM_BEGIN, RAM_END = 0x20000000, 0x20010000

def run(command, timeout=90, check=True):
    command = [str(arg) for arg in command]
    proc = subprocess.run(command, text=True, capture_output=True, timeout=timeout)
    output = proc.stdout + proc.stderr
    if check and proc.returncode:
        raise RuntimeError("command failed: " + " ".join(command) + "\n" + output)
    return proc.returncode, output

def require(value, reason):
    if not value:
        raise RuntimeError(reason)

def verify_elf(image, readelf):
    raw = image.read_bytes()
    require(len(raw) >= 52 and raw[:6] == b"\x7fELF\x01\x01",
            "expected ELF32 little endian")
    require(struct.unpack_from("<HH", raw, 16) == (2, 40),
            "expected standalone ARM executable")
    entry, phoff = struct.unpack_from("<II", raw, 24)
    phsize, count = struct.unpack_from("<HH", raw, 42)
    require(phsize >= 32 and 0 < count <= 32 and phoff <= len(raw)
            and count <= (len(raw) - phoff)//phsize, "invalid program header table")
    executable = []
    loads = []
    physical = []
    vector = None
    for index in range(count):
        typ, offset, vaddr, paddr, filesz, memsz, flags, _ = struct.unpack_from(
            "<IIIIIIII", raw, phoff + index*phsize)
        if typ != 1:
            continue
        require(offset <= len(raw) and filesz <= len(raw)-offset and filesz <= memsz,
                "out-of-bounds ELF segment")
        require(not (flags & 1 and flags & 2), "writable executable load segment")
        if filesz:
            require(paddr < FLASH_END and paddr+filesz <= FLASH_END,
                    "file-backed physical load outside board FLASH")
            for first, last in physical:
                require(not (paddr < last and first < paddr+filesz),
                        "overlapping physical FLASH loads")
            physical.append((paddr,paddr+filesz))
        if vaddr < FLASH_END:
            require(vaddr+memsz <= FLASH_END,
                    "FLASH load outside board memory")
        elif RAM_BEGIN <= vaddr < RAM_END:
            require(vaddr+memsz <= RAM_END, "RAM load outside board memory")
            require(filesz == 0 or paddr+filesz <= FLASH_END,
                    "initialized RAM lacks flash backing")
        else:
            raise RuntimeError("ELF load outside board memory")
        for first, last in loads:
            require(not (vaddr < last and first < vaddr+memsz),
                    "overlapping ELF load segments")
        loads.append((vaddr, vaddr+memsz))
        if flags & 1:
            executable.append((vaddr, vaddr+filesz))
        if vaddr == 0 and filesz >= 8:
            require(vector is None, "duplicate vector segment")
            vector = struct.unpack_from("<II", raw, offset)
    require(vector is not None, "missing vector table")
    sp, reset = vector
    require(sp == RAM_END and reset & 1 and reset == entry,
            "invalid SP or Thumb entry/reset vector")
    require(any(lo <= (reset & ~1) < hi for lo, hi in executable),
            "reset handler outside executable segment")
    _, symbols = run([readelf, "-Ws", image])
    unresolved = [line for line in symbols.splitlines()
                  if len(line.split()) >= 8 and line.split()[6] == "UND"
                  and line.split()[7]]
    require(not unresolved, "undefined ELF symbols: " + repr(unresolved))
    _, dynamic = run([readelf, "-d", image])
    require("NEEDED" not in dynamic, "dynamic runtime dependency")
    print("CORTEXM_ACTOR_ELF_VALID_PASS", image.name)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--readelf", default=shutil.which("readelf"))
    parser.add_argument("--qemu", default=shutil.which("qemu-system-arm"))
    parser.add_argument("--link-only", action="store_true")
    args = parser.parse_args()
    require(args.clang and args.readelf, "clang/readelf tools required")
    require(args.link_only or args.qemu, "qemu-system-arm required")
    root = pathlib.Path(__file__).resolve().parent.parent
    kernel = root/"src/main/cpp/oreslang/runtime/FreestandingActorKernel.c"
    firmware = root/"tests/freestanding_actor_kernel_firmware.c"
    startup = root/"tests/freestanding_actor_kernel_startup.S"
    linker = root/"tests/freestanding_actor_kernel_linker.ld"
    for file in (kernel, firmware, startup, linker):
        require(file.is_file(), "missing source: " + str(file))
    with tempfile.TemporaryDirectory(prefix="ores-cortexm-actor-") as temp:
        work = pathlib.Path(temp)
        common = [args.clang, "--target=thumbv7m-none-eabi", "-mcpu=cortex-m3",
                  "-mthumb", "-Oz", "-ffreestanding", "-fno-builtin",
                  "-fno-stack-protector", "-fno-unwind-tables",
                  "-fno-asynchronous-unwind-tables", "-I", root/"src/main/cpp"]
        run(common + ["-c", kernel, "-o", work/"kernel.o"])
        run(common + ["-c", startup, "-o", work/"startup.o"])
        for negative in (False, True):
            name = "negative" if negative else "positive"
            obj, image = work/(name + ".o"), work/(name + ".elf")
            run(common + (["-DNEGATIVE_GUEST"] if negative else [])
                + ["-c", firmware, "-o", obj])
            run([args.clang, "--target=thumbv7m-none-eabi", "-fuse-ld=lld",
                 "-nostdlib", "-nodefaultlibs", "-Wl,--no-undefined",
                 "-Wl,-T,"+str(linker), "-Wl,-Map,"+str(work/(name+".map")),
                 work/"kernel.o", work/"startup.o", obj, "-o", image])
            verify_elf(image, args.readelf)
            if args.link_only:
                continue
            command = [args.qemu, "-M", "lm3s6965evb", "-cpu", "cortex-m3",
                       "-nographic", "-monitor", "none", "-serial", "none",
                       "-semihosting-config", "enable=on,target=native",
                       "-kernel", image]
            try:
                code, output = run(command, timeout=20, check=False)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("QEMU actor kernel timeout: " + name) from exc
            passed, failed = "ORES_CORTEXM_ACTOR_KERNEL_PASS", "ORES_CORTEXM_ACTOR_KERNEL_FAIL"
            if negative:
                require(code != 0 and failed in output and passed not in output,
                        "negative QEMU control did not fail: "+repr((code, output)))
            else:
                require(code == 0 and passed in output and failed not in output,
                        "positive QEMU actor kernel did not complete: "+repr((code, output)))
            print("CORTEXM_ACTOR_QEMU_" + name.upper() + "_PASS", code)
    print("CORTEXM_ACTOR_KERNEL_" + ("LINK" if args.link_only else "QEMU") + "_PASS")

if __name__ == "__main__":
    main()
