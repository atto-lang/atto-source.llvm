"""Fail-closed negative matrix for actual ELF64 host binary preflight."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "cgl-preflight.py"
spec = importlib.util.spec_from_file_location("ores_cgl_preflight", SCRIPT)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def elf(*, machine=62, kind=2, elf_class=2, endian=1, osabi=0,
        version=1, load=True, interpreter=None, phoff=64, phentsize=56,
        phnum=None, load_offset=0, load_filesz=0):
    count = (1 if load else 0) + (1 if interpreter is not None else 0)
    if phnum is None:
        phnum = count
    h = bytearray(64)
    h[:8] = b"\x7fELF" + bytes((elf_class, endian, 1, osabi))
    struct.pack_into("<HHI", h, 16, kind, machine, version)
    struct.pack_into("<Q", h, 32, phoff)
    struct.pack_into("<HHH", h, 52, 64, phentsize, phnum)
    ph = bytearray()
    if load:
        ph += struct.pack("<IIQQQQQQ", 1, 5, load_offset, 0, 0, load_filesz, 0, 4096)
    if interpreter is not None:
        offset = 64 + 56 * count
        ph += struct.pack("<IIQQQQQQ", 3, 4, offset, 0, 0, len(interpreter), len(interpreter), 1)
    return bytes(h + ph) + (interpreter or b"")


class CGLAdmissionTests(unittest.TestCase):
    def inspect(self, data, mode=0o755):
        with tempfile.TemporaryDirectory() as td:
            target = Path(td) / "app"
            target.write_bytes(data)
            target.chmod(mode)
            return probe.inspect_elf(target)

    def test_accept_64_bit_x86_and_arm_elf_exec(self):
        for machine in (62, 183):
            with self.subTest(machine=machine):
                self.assertEqual(self.inspect(elf(machine=machine))["machine"], machine)

    def test_reject_truncated_non_elf(self):
        for data in (b"#!/bin/sh", b"", elf()[:20], b"\0" * 64):
            with self.subTest(data=data[:10]), self.assertRaises(ValueError):
                self.inspect(data)

    def test_reject_class_endianness_osabi_version_and_bad_kind(self):
        for kwargs in ({"elf_class": 1}, {"endian": 2}, {"osabi": 9},
                       {"version": 2}, {"kind": 1}, {"kind": 0}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.inspect(elf(**kwargs))

    def test_reject_program_header_oob(self):
        for kwargs in ({"phoff": 100000}, {"phentsize": 0}, {"phnum": 130}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.inspect(elf(**kwargs))

    def test_reject_missing_load_and_overlong_load(self):
        for data in (elf(load=False), elf(load_filesz=100000)):
            with self.assertRaises(ValueError):
                self.inspect(data)

    def test_reject_shared_object_and_relative_interpreter(self):
        for data in (elf(kind=3), elf(kind=3, interpreter=b"relative/ld.so\0"),
                     elf(kind=3, interpreter=b"/tmp/does-not-exist-CGL\0")):
            with self.assertRaises(ValueError):
                self.inspect(data)

    def test_accept_pie_with_host_interpreter(self):
        interp = "/lib64/ld-linux-x86-64.so.2"
        if not Path(interp).is_file():
            self.skipTest("host dynamic loader not installed")
        outcome = self.inspect(elf(kind=3, interpreter=os.fsencode(interp) + b"\0"))
        self.assertEqual(outcome["type"], "ET_DYN")
        self.assertEqual(outcome["interpreter"], interp)

    def test_reject_non_executable_and_symlink(self):
        with self.assertRaises(ValueError):
            self.inspect(elf(), mode=0o644)
        with tempfile.TemporaryDirectory() as td:
            real = Path(td) / "real"
            real.write_bytes(elf())
            real.chmod(0o755)
            sym = Path(td) / "sym"
            sym.symlink_to(real)
            with self.assertRaises(OSError):
                probe.inspect_elf(sym)

    def test_rt_sysfs_tristate(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "rt"
            for data, expected in (("1\n", True), ("0\n", False),
                                   ("garbage\n", None), ("1 extra\n", None),
                                   ("", None)):
                with self.subTest(data=data):
                    path.write_text(data)
                    self.assertIs(probe.read_preempt_rt(path), expected)
            self.assertIsNone(probe.read_preempt_rt(path / "missing"))

    def test_no_fake_certification_and_rt_fail_closed(self):
        ordinary = probe.preflight()
        self.assertFalse(ordinary["carrier_grade_qualified"])
        self.assertFalse(ordinary["hard_realtime_qualified"])
        self.assertEqual(ordinary["schema"], "oreslang.cgl.compat.v1")
        with patch.object(probe, "read_preempt_rt", return_value=None):
            required = probe.preflight(require_preempt_rt=True)
            self.assertEqual(required["compatibility"], "fail")

    def test_missing_binary_not_silent(self):
        with tempfile.TemporaryDirectory() as td:
            report = probe.preflight(binary=Path(td) / "missing")
            self.assertEqual(report["compatibility"], "fail")


    def test_wrong_architecture_is_not_admitted(self):
        other = 183 if probe.platform.machine().lower() == "x86_64" else 62
        with tempfile.TemporaryDirectory() as td:
            target = Path(td) / "foreign"
            target.write_bytes(elf(machine=other))
            target.chmod(0o755)
            report = probe.preflight(binary=target)
            self.assertEqual(report["compatibility"], "fail")
            self.assertIn("ELF machine does not match host architecture", report["errors"])

    def test_seeded_malformed_elf_mutation_never_admitted(self):
        import random
        rng = random.Random(20261018)
        with tempfile.TemporaryDirectory() as td:
            target = Path(td) / "mutant"
            for _ in range(128):
                target.write_bytes(rng.randbytes(rng.randrange(0, 256)))
                target.chmod(0o755)
                with self.assertRaises(ValueError):
                    probe.inspect_elf(target)


if __name__ == "__main__":
    unittest.main()
