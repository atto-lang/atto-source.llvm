#!/usr/bin/env python3
"""Fail-closed CGL Linux/ELF pilot preflight. NOT realtime certification."""
import argparse
import json
import os
from pathlib import Path
import platform
import stat
import struct
import sys
import time

MACHINES = {"x86_64": 62, "aarch64": 183}
MAX_PROGRAM_HEADERS = 128
MAX_INTERPRETER = 4096


def read_preempt_rt(path="/sys/kernel/realtime"):
    """True/False only for exactly valid sysfs 1/0; anything else is unknown."""
    try:
        value = Path(path).read_text(encoding="ascii").strip()
    except (OSError, UnicodeError, ValueError):
        return None
    return {"1": True, "0": False}.get(value)


def inspect_elf(path):
    """Validate a regular, executable, host-ABI ELF64 and its loader pathname.

    The dynamic dependencies and libc symbol versions still require an actual
    successful launch on the target CGL image, not merely this preflight.
    """
    # Nonblocking open is required: opening a FIFO for read can otherwise hang
    # indefinitely BEFORE fstat can reject the non-regular file.
    flags = (os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0) |
             getattr(os, "O_NOFOLLOW", 0))
    fd = os.open(path, flags)
    try:
        with os.fdopen(fd, "rb") as f:
            fd = -1
            st = os.fstat(f.fileno())
            if not stat.S_ISREG(st.st_mode):
                raise ValueError("ELF target is not a regular file")
            if not (st.st_mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)):
                raise ValueError("ELF has no executable permission bits")
            hdr = f.read(64)
            if len(hdr) != 64 or hdr[:4] != b"\x7fELF":
                raise ValueError("not a complete ELF64 header")
            if hdr[4:7] != b"\x02\x01\x01":
                raise ValueError("only ELF64 little-endian version 1 is supported")
            if hdr[7] not in (0, 3):
                raise ValueError("unsupported ELF OS ABI")
            kind, machine, version = struct.unpack_from("<HHI", hdr, 16)
            if kind not in (2, 3) or version != 1:
                raise ValueError("not a supported ELF executable or PIE")
            phoff = struct.unpack_from("<Q", hdr, 32)[0]
            ehsize, phentsize, phnum = struct.unpack_from("<HHH", hdr, 52)
            if ehsize != 64 or phentsize != 56 or not 1 <= phnum <= MAX_PROGRAM_HEADERS:
                raise ValueError("invalid ELF header/program-header sizes")
            if phoff < 64 or phoff > st.st_size or phnum * phentsize > st.st_size - phoff:
                raise ValueError("ELF program-header table exceeds file")
            f.seek(phoff)
            phdrs = f.read(phnum * phentsize)
            has_load = False
            interp = None
            for i in range(phnum):
                p_type, _, p_offset, p_vaddr, _, p_filesz, p_memsz, p_align = struct.unpack_from(
                    "<IIQQQQQQ", phdrs, i * phentsize)
                if p_type == 1:
                    has_load = True
                    if p_offset > st.st_size or p_filesz > st.st_size - p_offset:
                        raise ValueError("ELF PT_LOAD extends beyond file")
                    # The loader cannot map more bytes from the file than the
                    # segment's declared in-memory extent. Prevent permissive
                    # preflight from accepting malformed program headers.
                    if p_memsz < p_filesz:
                        raise ValueError("ELF PT_LOAD file size exceeds memory size")
                    # A 64-bit loader must never interpret a virtual address
                    # range wrapping UINT64_MAX. Python integers do not
                    # overflow, so test the exclusive end explicitly.
                    if p_memsz and p_vaddr > (1 << 64) - p_memsz - 1:
                        raise ValueError("ELF PT_LOAD virtual address range overflows ELF64")
                    # ELF p_align must be 0/1 or a power of two. For a mapped
                    # segment its file offset and virtual address must be
                    # congruent modulo p_align (no truncation or host ABI guess).
                    if p_align > 1 and (p_align & (p_align - 1)) != 0:
                        raise ValueError("ELF PT_LOAD alignment is not a power of two")
                    if p_align > 1 and (p_vaddr % p_align) != (p_offset % p_align):
                        raise ValueError("ELF PT_LOAD alignment/offset mismatch")
                if p_type == 3:
                    if interp is not None:
                        raise ValueError("ELF has multiple PT_INTERP segments")
                    if not 2 <= p_filesz <= MAX_INTERPRETER or p_offset > st.st_size or p_filesz > st.st_size - p_offset:
                        raise ValueError("invalid ELF PT_INTERP bounds")
                    here = f.tell()
                    f.seek(p_offset)
                    raw = f.read(p_filesz)
                    f.seek(here)
                    if not raw.endswith(b"\0") or b"\0" in raw[:-1]:
                        raise ValueError("ELF interpreter is not a single NUL-terminated path")
                    try:
                        interp = os.fsdecode(raw[:-1])
                    except (ValueError, UnicodeError):
                        raise ValueError("invalid ELF interpreter pathname") from None
                    if not interp.startswith("/"):
                        raise ValueError("ELF interpreter must be an absolute path")
            if not has_load:
                raise ValueError("ELF lacks loadable segments")
            if kind == 3 and interp is None:
                raise ValueError("ET_DYN without interpreter is not admitted by pilot")
            if interp is not None and not os.path.isfile(interp):
                raise ValueError("ELF interpreter not present on target host")
            return {"machine": machine, "type": "ET_EXEC" if kind == 2 else "ET_DYN",
                    "interpreter": interp}
    finally:
        if fd != -1:
            os.close(fd)


def elf_machine(path):
    """Compatibility API retained for existing tests."""
    return inspect_elf(path)["machine"]


def preflight(binary=None, require_preempt_rt=False):
    errors = []
    arch = platform.machine().lower()
    linux = sys.platform == "linux"
    if not linux:
        errors.append("host is not Linux")
    if arch not in MACHINES:
        errors.append("architecture is outside the x86_64/aarch64 pilot")
    affinity = []
    try:
        affinity = sorted(os.sched_getaffinity(0))
        if not affinity:
            errors.append("effective CPU affinity is empty")
    except (AttributeError, OSError) as exc:
        errors.append("cannot inspect effective CPU affinity: " + type(exc).__name__)
    try:
        clock = time.get_clock_info("monotonic")
        if not clock.monotonic:
            errors.append("monotonic clock is not monotonic")
        resolution = clock.resolution
    except (OSError, ValueError) as exc:
        errors.append("monotonic clock unavailable: " + type(exc).__name__)
        resolution = None
    try:
        scheduler_policy = os.sched_getscheduler(0)
    except (AttributeError, OSError) as exc:
        scheduler_policy = None
        errors.append("cannot inspect effective scheduler: " + type(exc).__name__)
    rt_flag = read_preempt_rt()
    if require_preempt_rt and rt_flag is not True:
        errors.append("PREEMPT_RT cannot be verified from /sys/kernel/realtime")
    elf = None
    if binary is not None:
        try:
            elf = inspect_elf(binary)
            if arch in MACHINES and elf["machine"] != MACHINES[arch]:
                errors.append("ELF machine does not match host architecture")
        except (OSError, ValueError) as exc:
            errors.append("ELF inspection failed: " + str(exc))
    return {
        "schema": "oreslang.cgl.compat.v1", "compatibility": "pass" if not errors else "fail",
        "carrier_grade_qualified": False, "hard_realtime_qualified": False,
        "host": {"linux": linux, "architecture": arch, "kernel": platform.release(),
                 "affinity_cpu_count": len(affinity), "scheduler_policy": scheduler_policy,
                 "monotonic_resolution_seconds": resolution, "preempt_rt_sysfs": rt_flag},
        "elf": elf, "elf_machine": elf["machine"] if elf else None,
        "errors": errors,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", help="inspect locally built host ELF executable")
    parser.add_argument("--require-preempt-rt", action="store_true",
                        help="fail closed unless PREEMPT_RT sysfs flag is 1")
    args = parser.parse_args(argv)
    report = preflight(args.binary, args.require_preempt_rt)
    print(json.dumps(report, sort_keys=True))
    return 0 if report["compatibility"] == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
