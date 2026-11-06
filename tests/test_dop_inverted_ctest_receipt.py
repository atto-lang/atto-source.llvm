"""Adversarial source-free unit tests; do not qualify a compiler or a guest."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

MODULE_PATH = Path(__file__).resolve().parents[1] / "scripts" / "dop_inverted_ctest_receipt.py"
spec = importlib.util.spec_from_file_location("dop_inverted_ctest_receipt", MODULE_PATH)
receipt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receipt)


def valid_xml():
    root = ET.Element("testsuite", tests="4", failures="0", errors="0", skipped="0")
    for name in sorted(receipt.EXPECTED_TESTS):
        ET.SubElement(root, "testcase", name=name, status="run", time="0.015")
    return root


class StrictCtTests(unittest.TestCase):
    def check(self, node):
        with tempfile.TemporaryDirectory() as directory:
            file = Path(directory) / "results.xml"
            ET.ElementTree(node).write(file, encoding="utf-8")
            return receipt.check_ctest_junit(file)

    def deny(self, node):
        with self.assertRaises(receipt.InvalidEvidence):
            self.check(node)

    def test_valid_case_passes(self):
        self.assertEqual(set(self.check(valid_xml())), receipt.EXPECTED_TESTS)

    def test_missing_guest_rejected(self):
        root = valid_xml()
        root.remove(next(x for x in root if x.get("name") == "native_inverted_production_source_to_native"))
        self.deny(root)

    def test_duplicate_case_rejected(self):
        root = valid_xml()
        root[0].set("name", root[1].get("name"))
        self.deny(root)

    def test_failing_case_rejected(self):
        root = valid_xml()
        ET.SubElement(root[0], "failure", message="segfault")
        self.deny(root)

    def test_skipped_case_rejected(self):
        root = valid_xml()
        ET.SubElement(root[0], "skipped")
        self.deny(root)

    def test_notrun_status_rejected(self):
        root = valid_xml()
        root[0].set("status", "notrun")
        self.deny(root)

    def test_contradictory_summary_rejected(self):
        root = valid_xml()
        root.set("failures", "1")
        self.deny(root)

    def test_summary_omission_rejected(self):
        root = valid_xml()
        del root.attrib["skipped"]
        self.deny(root)

    def test_invalid_duration_rejected(self):
        root = valid_xml()
        root[0].set("time", "nan")
        self.deny(root)

    def test_extra_case_rejected(self):
        root = valid_xml()
        ET.SubElement(root, "testcase", name="other", status="run", time="0.5")
        self.deny(root)

    def test_many_test_suites_rejected(self):
        root = ET.Element("testsuites")
        root.append(valid_xml())
        root.append(valid_xml())
        self.deny(root)

    def test_one_suite_wrapper_accepted(self):
        root = ET.Element("testsuites")
        root.append(valid_xml())
        self.assertEqual(set(self.check(root)), receipt.EXPECTED_TESTS)


if __name__ == "__main__":
    unittest.main()
