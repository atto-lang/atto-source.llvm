#!/usr/bin/env python3
"""Hermetic CTest inventory/JUnit reconciliation: never assert guest execution."""
import copy
import sys
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_ctest_execution import ExecutionFailure, verify_execution, REQUIRED_GUEST


def fixture(n=4):
    names = [REQUIRED_GUEST] + [f"native_compiler_test_{i}" for i in range(n - 1)]
    manifest = {"kind": "ctestInfo", "tests": [{"name": x} for x in names]}
    suite = ET.Element("testsuite", {"tests": str(n), "failures": "0", "skipped": "0"})
    for name in names:
        ET.SubElement(suite, "testcase", {"name": name, "time": "0.003", "status": "run"})
    return manifest, suite


def xml_bytes(suite):
    return ET.tostring(suite, encoding="utf-8")


class JunitGateTests(unittest.TestCase):
    def validate(self, manifest, suite):
        return verify_execution(manifest, xml_bytes(suite), min_tests=4)

    def fail(self, manifest, suite):
        with self.assertRaises(ExecutionFailure):
            self.validate(manifest, suite)

    def test_exact_all_executed(self):
        m, s = fixture()
        self.assertEqual(self.validate(m, s), 4)

    def test_missing_guest_execution(self):
        m, s = fixture()
        s.remove(s[0]); s.set("tests", "3")
        self.fail(m, s)

    def test_successful_ctest_with_skipped_case_rejected(self):
        m, s = fixture()
        ET.SubElement(s[0], "skipped")
        self.fail(m, s)

    def test_notrun_without_skipped_element_rejected(self):
        m, s = fixture()
        s[0].set("status", "notrun")
        self.fail(m, s)

    def test_skipped_suite_counter_rejected(self):
        m, s = fixture()
        s.set("skipped", "1")
        self.fail(m, s)

    def test_duplicate_pass_record_cannot_replace_missing_test(self):
        m, s = fixture()
        s[1].set("name", s[0].get("name"))
        self.fail(m, s)

    def test_failed_and_error_cases_rejected(self):
        for tag in ("failure", "error"):
            m, s = fixture()
            ET.SubElement(s[1], tag)
            self.fail(m, s)

    def test_inverted_will_fail_case_rejected(self):
        m, s = fixture()
        ET.SubElement(s[1], "skipped", {"message": "WILL_FAIL"})
        self.fail(m, s)

    def test_missing_required_registration_rejected(self):
        m, s = fixture()
        m["tests"][0]["name"] = "fake_compiler_test"
        self.fail(m, s)

    def test_manifest_duplicate_rejected(self):
        m, s = fixture()
        m["tests"][1]["name"] = m["tests"][0]["name"]
        self.fail(m, s)

    def test_junit_suite_counter_lie_rejected(self):
        m, s = fixture()
        s.set("tests", "1000")
        self.fail(m, s)

    def test_fewer_than_minimum_tests_rejected(self):
        m, s = fixture(2)
        self.fail(m, s)

    def test_doctype_rejected(self):
        m, s = fixture()
        with self.assertRaises(ExecutionFailure):
            verify_execution(m, b'<!DOCTYPE foo [<!ENTITY x "hi">]>' + xml_bytes(s), min_tests=4)

    def test_bad_xml_rejected(self):
        m, _ = fixture()
        with self.assertRaises(ExecutionFailure):
            verify_execution(m, b"<not xml", min_tests=4)

    def test_unknown_testcase_child_rejected(self):
        m, s = fixture()
        ET.SubElement(s[1], "untrusted-success")
        self.fail(m, s)

    def test_real_testsuites_wrapper(self):
        m, s = fixture()
        wrapper = ET.Element("testsuites"); wrapper.append(s)
        self.assertEqual(self.validate(m, wrapper), 4)


if __name__ == "__main__":
    unittest.main()
