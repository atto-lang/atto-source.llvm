"""Source-free adversarial tests for Atto LLVM LRU native CTest receipt admission."""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from atto_lru_junit_gate import REQUIRED, verify


class LruJunitReceiptTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix="atto-llvm-junit-gate-")
        self.addCleanup(self.work.cleanup)
        self.output = Path(self.work.name) / "ctest.xml"
        self.root = ET.Element("testsuite", {
            "tests": "5", "failures": "0", "errors": "0",
            "disabled": "0", "skipped": "0"
        })
        for name in sorted(REQUIRED):
            ET.SubElement(self.root, "testcase", {
                "name": name, "classname": name, "time": "0.01",
                "status": "run"
            })

    def check(self):
        self.output.write_bytes(ET.tostring(self.root, encoding="utf-8"))
        with contextlib.redirect_stdout(io.StringIO()):
            verify(self.output)

    def test_exact_five_real_runs_admitted(self):
        self.check()

    def test_missing_native_guest_denied(self):
        self.root.remove(list(self.root)[0])
        with self.assertRaisesRegex(ValueError, "wrong, missing"):
            self.check()

    def test_duplicate_native_guest_denied(self):
        list(self.root)[1].set("name", list(self.root)[0].get("name"))
        with self.assertRaisesRegex(ValueError, "wrong, missing"):
            self.check()

    def test_declared_wrong_count_denied(self):
        self.root.set("tests", "4")
        with self.assertRaisesRegex(ValueError, "declared test count"):
            self.check()

    def test_disabled_with_no_skipped_child_denied(self):
        list(self.root)[0].set("status", "disabled")
        with self.assertRaisesRegex(ValueError, "not actually run"):
            self.check()

    def test_notrun_without_failure_child_denied(self):
        list(self.root)[0].set("status", "notrun")
        with self.assertRaisesRegex(ValueError, "not actually run"):
            self.check()

    def test_missing_status_denied(self):
        del list(self.root)[0].attrib["status"]
        with self.assertRaisesRegex(ValueError, "not actually run"):
            self.check()

    def test_skipped_child_denied(self):
        ET.SubElement(list(self.root)[0], "skipped")
        with self.assertRaisesRegex(ValueError, "failed/skipped"):
            self.check()

    def test_failure_child_denied(self):
        ET.SubElement(list(self.root)[0], "failure")
        with self.assertRaisesRegex(ValueError, "failed/skipped"):
            self.check()

    def test_suite_disabled_denied(self):
        self.root.set("disabled", "1")
        with self.assertRaisesRegex(ValueError, "nonzero disabled"):
            self.check()

    def test_suite_skipped_denied(self):
        self.root.set("skipped", "1")
        with self.assertRaisesRegex(ValueError, "nonzero skipped"):
            self.check()

    def test_symlinked_report_denied(self):
        real = Path(self.work.name) / "real.xml"
        real.write_bytes(ET.tostring(self.root, encoding="utf-8"))
        self.output.symlink_to(real)
        with self.assertRaisesRegex(ValueError, "symlinked"):
            verify(self.output)

    def test_doctype_entity_denied(self):
        self.output.write_text(
            '<!DOCTYPE testsuite [<!ENTITY danger "forge">]>'
            '<testsuite tests="5"/>', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "DTD/entities"):
            verify(self.output)

    def test_lowercase_doctype_denied(self):
        self.output.write_text(
            '<!doctype testsuite><testsuite tests="5"/>', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "DTD/entities"):
            verify(self.output)

    def test_malformed_xml_denied(self):
        self.output.write_text("<testsuite>")
        with self.assertRaises(ET.ParseError):
            verify(self.output)


if __name__ == "__main__":
    unittest.main()
