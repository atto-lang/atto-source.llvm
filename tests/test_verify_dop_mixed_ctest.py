#!/usr/bin/env python3
"""Synthetic hostile CTest JUnit mutation suite (source-free, NOT guest proof)."""
import copy
import hashlib
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from verify_dop_mixed_ctest import (  # noqa: E402
    EXPECTED, LEGACY_MARKER, MIXED_MARKER, verify_junit
)

MIXED = "native_inverted_mixed_source_to_native"
LEGACY = "native_inverted_production_source_to_native"


def canonical():
    suite = ET.Element("testsuite", tests="5", failures="0",
                       errors="0", skipped="0", time="3.5")
    for name in sorted(EXPECTED):
        case = ET.SubElement(suite, "testcase", name=name,
                             status="run", time="0.7")
        ET.SubElement(case, "properties")  # actual CTest 3.31 report shape
        output = ET.SubElement(case, "system-out")
        if name == MIXED:
            output.text = "production compiler ran\n" + MIXED_MARKER + "\n"
        elif name == LEGACY:
            output.text = "legacy compile completed\n" + LEGACY_MARKER + "\n"
        else:
            output.text = "host test finished\n"
    return suite


def xml(suite):
    return ET.tostring(suite, encoding="utf-8")


def rejection(suite):
    try:
        verify_junit(xml(suite))
    except ValueError:
        return
    raise AssertionError("forged synthetic CTest report admitted")


class MixedCertificateMutationTests(unittest.TestCase):
    def test_exact_canonical_reference_passes_as_report_only(self):
        outcome = verify_junit(xml(canonical()))
        self.assertEqual(outcome["verified_tests"], 5)
        self.assertEqual(outcome["evidence_class"],
                         "JUNIT_REPORT_CHECKED_NOT_RUN_JOB_ATTESTED")

    def test_missing_case_denied(self):
        root = canonical()
        root.remove(root[-1])
        rejection(root)

    def test_duplicate_case_denied(self):
        root = canonical()
        root[-1].set("name", root[0].get("name"))
        rejection(root)

    def test_wrong_case_name_denied(self):
        root = canonical()
        root[0].set("name", "forged_guest")
        rejection(root)

    def test_synthetic_pass_does_not_mask_test_failure(self):
        root = canonical()
        ET.SubElement(root.find("testcase"), "failure", message="failed")
        rejection(root)

    def test_skipped_status_denied(self):
        root = canonical()
        root[0].set("status", "notrun")
        rejection(root)

    def test_suite_failure_totals_denied(self):
        root = canonical()
        root.set("failures", "1")
        rejection(root)

    def test_suite_test_count_mismatch_denied(self):
        root = canonical()
        root.set("tests", "6")
        rejection(root)

    def test_missing_errors_total_accepted_like_real_ctest(self):
        root = canonical()
        root.attrib.pop("errors")
        self.assertEqual(verify_junit(xml(root))["verified_tests"], 5)

    def test_nonzero_errors_total_denied(self):
        root = canonical()
        root.set("errors", "1")
        rejection(root)

    def test_nonempty_properties_denied(self):
        root = canonical()
        props = root[0].find("properties")
        ET.SubElement(props, "property", name="forged", value=MIXED_MARKER)
        rejection(root)

    def test_nonzero_disabled_total_denied(self):
        root = canonical()
        root.set("disabled", "1")
        rejection(root)

    def test_nonfinite_seconds_denied(self):
        for bad in ("NaN", "inf", "-inf"):
            with self.subTest(bad=bad):
                root = canonical()
                root[0].set("time", bad)
                rejection(root)

    def test_zero_and_negative_duration_denied(self):
        for bad in ("0", "-1"):
            with self.subTest(bad=bad):
                root = canonical()
                root[0].set("time", bad)
                rejection(root)

    def test_marker_in_stderr_only_denied(self):
        root = canonical()
        case = next(c for c in root if c.get("name") == MIXED)
        case.find("system-out").text = "host reference only\n"
        ET.SubElement(case, "system-err").text = MIXED_MARKER
        rejection(root)

    def test_marker_on_unrelated_case_denied(self):
        root = canonical()
        case = next(c for c in root if c.get("name") == MIXED)
        case.find("system-out").text = "not executed\n"
        root[0].find("system-out").text = MIXED_MARKER
        rejection(root)

    def test_duplicate_marker_denied(self):
        root = canonical()
        case = next(c for c in root if c.get("name") == MIXED)
        case.find("system-out").text = MIXED_MARKER + "\n" + MIXED_MARKER + "\n"
        rejection(root)

    def test_marker_substring_not_exact_line_denied(self):
        root = canonical()
        case = next(c for c in root if c.get("name") == MIXED)
        case.find("system-out").text = "NO_EXECUTION_" + MIXED_MARKER + "\n"
        rejection(root)

    def test_marker_in_xml_property_denied(self):
        root = canonical()
        case = next(c for c in root if c.get("name") == MIXED)
        case.find("system-out").text = "not executed\n"
        ET.SubElement(case, "properties", message=MIXED_MARKER)
        rejection(root)

    def test_nested_fake_case_denied(self):
        root = canonical()
        ET.SubElement(root[0], "testcase", name="forged", status="run", time="0.1")
        rejection(root)

    def test_second_suite_denied(self):
        root = ET.Element("testsuites")
        root.append(canonical())
        root.append(copy.deepcopy(root[0]))
        with self.assertRaises(ValueError):
            verify_junit(xml(root))

    def test_xml_doctype_denied(self):
        raw = b'<!DOCTYPE testsuite [<!ENTITY forged "pass">]>' + xml(canonical())
        with self.assertRaises(ValueError):
            verify_junit(raw)

    def test_bounded_report_size_denied(self):
        with self.assertRaises(ValueError):
            verify_junit(b"x" * 1_048_577)

    def test_empty_and_malformed_xml_denied(self):
        for raw in (b"", b"<testsuite", b"<testsuite></testcase>"):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                verify_junit(raw)


    def test_verified_digest_uses_exact_admitted_bytes(self):
        document = xml(canonical())
        result = verify_junit(document)
        self.assertEqual(result["junit_sha256"],
                         hashlib.sha256(document).hexdigest())

    def test_semantically_equivalent_xml_has_distinct_certified_hash(self):
        original = xml(canonical())
        altered = original + b"\n"
        self.assertEqual(verify_junit(original)["verified_tests"], 5)
        self.assertEqual(verify_junit(altered)["verified_tests"], 5)
        self.assertNotEqual(verify_junit(original)["junit_sha256"],
                            verify_junit(altered)["junit_sha256"])
        # The workflow's step output must only admit one of these digests.

    def test_suite_zero_time_accepted_as_real_ctest_rounding(self):
        root = canonical()
        root.set("time", "0")
        self.assertEqual(verify_junit(xml(root))["verified_tests"], 5)

    def test_suite_missing_time_accepted(self):
        root = canonical()
        root.attrib.pop("time")
        self.assertEqual(verify_junit(xml(root))["verified_tests"], 5)

    def test_suite_nan_infinity_denied(self):
        for value in ("nan", "NaN", "Infinity", "-inf"):
            with self.subTest(value=value):
                root = canonical()
                root.set("time", value)
                rejection(root)

    def test_suite_negative_or_excessive_seconds_denied(self):
        for value in ("-0.1", "-1", "3600.01"):
            with self.subTest(value=value):
                root = canonical()
                root.set("time", value)
                rejection(root)

    def test_suite_malformed_duration_denied(self):
        root = canonical()
        root.set("time", "not-a-time")
        rejection(root)

    def test_ctest_case_class_matches_name(self):
        root = canonical()
        for case in root:
            case.set("classname", case.get("name"))
        self.assertEqual(verify_junit(xml(root))["verified_tests"], 5)

    def test_wrong_ctest_class_name_rejected(self):
        root = canonical()
        root[0].set("classname", "forged_other_test")
        rejection(root)

    def test_unexpected_testcase_attribute_rejected(self):
        root = canonical()
        root[0].set("proof", "approved")
        rejection(root)

    def test_other_native_marker_replayed_in_mixed_guest_denied(self):
        root = canonical()
        mixed = next(case for case in root if case.get("name") == MIXED)
        mixed.find("system-out").text += LEGACY_MARKER + "\n"
        rejection(root)

    def test_other_native_marker_replayed_in_legacy_guest_denied(self):
        root = canonical()
        legacy = next(case for case in root if case.get("name") == LEGACY)
        legacy.find("system-out").text += MIXED_MARKER + "\n"
        rejection(root)

    def test_noncanonical_suite_error_counter_denied(self):
        for value in ("01", "-0", "NaN", "+0", "0.0"):
            with self.subTest(value=value):
                root = canonical()
                root.set("errors", value)
                rejection(root)

    def test_case_classname_may_be_absent_in_synthetic_reference(self):
        root = canonical()
        self.assertTrue(all("classname" not in case.attrib for case in root))
        self.assertEqual(verify_junit(xml(root))["verified_tests"], 5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
