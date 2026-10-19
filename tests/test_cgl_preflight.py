"""CGL preflight must never silently accept unsupported native artifacts."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "cgl-preflight.py"
spec = importlib.util.spec_from_file_location("ores_cgl_preflight", SCRIPT)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def elf(kind=2, machine=62, elf_class=2, endian=1):
    header = bytearray(64)
    header[:7] = b"\x7fELF" + bytes((elf_class, endian, 1))
    struct.pack_into("<HH" if endian == 1 else ">HH", header, 16, kind, machine)
    return bytes(header)


class CGLAdmissionTests(unittest.TestCase):
    def read(self, raw):
        with tempfile.TemporaryDirectory() as td:
            target = Path(td) / "payload"
            target.write_bytes(raw)
            return probe.elf_machine(target)

    def test_host_abi_variants(self):
        self.assertEqual(self.read(elf(machine=62)), 62)
        self.assertEqual(self.read(elf(machine=183, endian=2)), 183)
        self.assertEqual(self.read(elf(kind=3)), 62)

    def test_reject_truncated_and_non_elf(self):
        for raw in (b"#!/bin/sh", elf()[:20], b"\0" * 64):
            with self.subTest(raw=raw[:8]), self.assertRaises(ValueError):
                self.read(raw)

    def test_reject_non_executable_and_32_bit(self):
        for raw in (elf(kind=1), elf(elf_class=1), elf(endian=0)):
            with self.subTest(raw=raw[:20]), self.assertRaises(ValueError):
                self.read(raw)

    def test_report_never_claims_cgl_or_hard_rt_certification(self):
        report = probe.preflight()
        self.assertFalse(report["carrier_grade_qualified"])
        self.assertFalse(report["hard_realtime_qualified"])
        self.assertEqual(report["schema"], "oreslang.cgl.compat.v1")


if __name__ == "__main__":
    unittest.main()
