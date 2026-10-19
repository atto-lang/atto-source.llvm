#!/usr/bin/env python3
"""Non-privileged Linux/CGL host and ELF compatibility probe; NOT RT certification."""
import argparse
import json
import os
from pathlib import Path
import platform
import struct
import sys
import time

MACHINES = {"x86_64": 62, "aarch64": 183}


def elf_machine(path):
    """Reject foreign architecture, non-ELF, non-64-bit, or non-executable ELF."""
    with Path(path).open("rb") as f:
        h = f.read(64)
    if len(h) < 64 or h[:4] != b"\x7fELF":
        raise ValueError("not a complete ELF64 header")
    if h[4] != 2 or h[5] not in (1, 2) or h[6] != 1:
        raise ValueError("unsupported ELF class, endian, or version")
    fmt = "<HH" if h[5] == 1 else ">HH"
    kind, machine = struct.unpack_from(fmt, h, 16)
    if kind not in (2, 3):
        raise ValueError("ELF is not an executable or PIE/shared-object image")
    return machine


def preflight(binary=None, require_preempt_rt=False):
    errors = []
    arch = platform.machine().lower()
    linux = sys.platform.startswith("linux")
    if not linux:
        errors.append("host is not Linux")
    if arch not in MACHINES:
        errors.append("architecture is outside the x86_64/aarch64 initial pilot")
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
    except (OSError, ValueError) as exc:
        errors.append("monotonic clock unavailable: " + type(exc).__name__)
    rt_flag = None
    try:
        rt_flag = Path("/sys/kernel/realtime").read_text(encoding="ascii").strip() == "1"
    except (OSError, ValueError):
        pass
    if require_preempt_rt and rt_flag is not True:
        errors.append("PREEMPT_RT cannot be verified from /sys/kernel/realtime")
    machine = None
    if binary:
        try:
            machine = elf_machine(binary)
            if arch in MACHINES and machine != MACHINES[arch]:
                errors.append("ELF machine does not match host architecture")
        except (OSError, ValueError) as exc:
            errors.append("ELF inspection failed: " + str(exc))
    return {
        "schema": "oreslang.cgl.compat.v1",
        "compatibility": "pass" if not errors else "fail",
        "carrier_grade_qualified": False,
        "hard_realtime_qualified": False,
        "host": {"linux": linux, "architecture": arch,
                 "kernel": platform.release(), "affinity_cpu_count": len(affinity),
                 "preempt_rt_sysfs": rt_flag},
        "elf_machine": machine,
        "errors": errors,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", help="inspect a locally built ELF executable")
    parser.add_argument("--require-preempt-rt", action="store_true",
                        help="fail closed unless PREEMPT_RT sysfs flag is 1")
    args = parser.parse_args(argv)
    report = preflight(args.binary, args.require_preempt_rt)
    print(json.dumps(report, sort_keys=True))
    return 0 if report["compatibility"] == "pass" else 2


if __name__ == "__main__":
    raise SystemExit(main())
