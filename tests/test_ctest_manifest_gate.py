#!/usr/bin/env python3
"""Adversarial validation for the source-owned full-CTest manifest gate."""
import copy
import unittest

from ctest_manifest_gate import (
    CRITICAL_TESTS, ManifestFailure, check_manifest, TIMED_TESTS,
    VISIBILITY_NEGATIVE_FIXTURES, VISIBILITY_POSITIVE_FIXTURE,
)


def fixture():
    entries = []
    for name in CRITICAL_TESTS:
        props = [{"name": "TIMEOUT", "value": 90.0}] if name in TIMED_TESTS else []
        command = ["/usr/bin/python3"]
        if name == "native_contract_harness_python_O":
            command.append("-O")
        if name == "native_contract_harness_python_OO":
            command.append("-OO")
        if name.startswith("native_contract_harness_"):
            command.append("tests/test_contract_harness.py")
        elif name == "native_contract_positive_object_verifies":
            command.extend(["tests/contract.py", "--compiler", "build/oreslang-llvmc",
                            "--fixture", "tests/fixtures/constant-main.ores",
                            "--verify-object", "--clang", "clang"])
        elif name in VISIBILITY_NEGATIVE_FIXTURES:
            command.extend([
                "tests/contract.py", "--compiler", "build/oreslang-llvmc",
                "--fixture", "tests/fixtures/" + VISIBILITY_NEGATIVE_FIXTURES[name],
                "--diagnostic", "private class field",
            ])
        elif name == "native_public_class_field_read":
            command.extend([
                "tests/contract.py", "--compiler", "build/oreslang-llvmc",
                "--fixture", "tests/fixtures/" + VISIBILITY_POSITIVE_FIXTURE,
                "--run", "42", "--clang", "clang", "--no-native-abi",
            ])
        else:
            command.extend(["-c", "pass"])
        entries.append({"name": name, "command": command,
                        "properties": props})
    for index in range(125):
        entries.append({"name": f"native_extra_{index}", "command": ["/tmp/test"]})
    return {"kind": "ctestInfo", "version": {"major": 1, "minor": 0},
            "tests": entries}


class ManifestAuditTests(unittest.TestCase):
    def setUp(self):
        self.manifest = fixture()

    def fail_on(self, expected):
        with self.assertRaisesRegex(ManifestFailure, expected):
            check_manifest(self.manifest)

    def test_complete_manifest_is_admitted(self):
        self.assertEqual(check_manifest(self.manifest), 125 + len(CRITICAL_TESTS))

    def test_empty_manifest_rejected(self):
        self.manifest["tests"] = []
        self.fail_on("expected at least")

    def test_non_json_object_rejected(self):
        with self.assertRaises(ManifestFailure):
            check_manifest("success")

    def test_invalid_tests_key_rejected(self):
        self.manifest["tests"] = {}
        self.fail_on("tests array")

    def test_required_positive_compiler_object_gate_must_exist(self):
        self.manifest["tests"] = [
            x for x in self.manifest["tests"]
            if x["name"] != "native_contract_positive_object_verifies"
        ]
        self.fail_on("required CTest was not registered")

    def test_python_optimized_gates_cannot_be_missing(self):
        for critical in CRITICAL_TESTS[:3]:
            corpus = fixture()
            corpus["tests"] = [x for x in corpus["tests"] if x["name"] != critical]
            with self.assertRaisesRegex(ManifestFailure, "required CTest"):
                check_manifest(corpus)

    def test_optimized_python_flags_are_not_optional(self):
        for name, flag in (
                ("native_contract_harness_python_O", "-O"),
                ("native_contract_harness_python_OO", "-OO")):
            document = fixture()
            entry = next(x for x in document["tests"] if x["name"] == name)
            entry["command"].remove(flag)
            with self.assertRaisesRegex(ManifestFailure, "not invoked with Python"):
                check_manifest(document)

    def test_optimized_harness_must_execute_correct_script(self):
        self.manifest["tests"][0]["command"][-1] = "tests/empty.py"
        self.fail_on("wrong script")

    def test_native_object_gates_cannot_be_omitted(self):
        for flag in ("--verify-object", "--clang", "--compiler", "--fixture"):
            document = fixture()
            entry = next(x for x in document["tests"]
                         if x["name"] == "native_contract_positive_object_verifies")
            entry["command"].remove(flag)
            with self.assertRaisesRegex(ManifestFailure, "source object CTest is missing"):
                check_manifest(document)

    def test_native_object_fixture_cannot_be_replaced_with_stub(self):
        name = "native_contract_positive_object_verifies"
        entry = next(x for x in self.manifest["tests"] if x["name"] == name)
        idx = entry["command"].index("tests/fixtures/constant-main.ores")
        entry["command"][idx] = "tests/fixtures/empty.ores"
        self.fail_on("does not compile positive guest source")

    def test_private_field_negative_gates_are_all_required(self):
        for critical in VISIBILITY_NEGATIVE_FIXTURES:
            corpus = fixture()
            corpus["tests"] = [item for item in corpus["tests"]
                               if item["name"] != critical]
            with self.subTest(critical=critical), self.assertRaisesRegex(
                    ManifestFailure, "required CTest was not registered"):
                check_manifest(corpus)

    def test_private_negative_cannot_use_another_fixture(self):
        critical = next(iter(VISIBILITY_NEGATIVE_FIXTURES))
        entry = next(item for item in self.manifest["tests"]
                     if item["name"] == critical)
        i = entry["command"].index("--fixture")
        entry["command"][i + 1] = "tests/fixtures/constant-main.ores"
        self.fail_on("private-field negative gate changed")

    def test_private_negative_must_preserve_diagnostic(self):
        critical = next(iter(VISIBILITY_NEGATIVE_FIXTURES))
        entry = next(item for item in self.manifest["tests"]
                     if item["name"] == critical)
        i = entry["command"].index("--diagnostic")
        entry["command"][i + 1] = "other error"
        self.fail_on("private-field negative gate changed")

    def test_public_field_positive_must_execute(self):
        entry = next(item for item in self.manifest["tests"]
                     if item["name"] == "native_public_class_field_read")
        entry["command"].remove("--no-native-abi")
        self.fail_on("public-field positive execution gate changed")

    def test_public_field_positive_wrong_fixture_rejected(self):
        entry = next(item for item in self.manifest["tests"]
                     if item["name"] == "native_public_class_field_read")
        i = entry["command"].index("--fixture")
        entry["command"][i + 1] = "tests/fixtures/constant-main.ores"
        self.fail_on("public-field positive execution gate changed")

    def test_duplicate_test_names_rejected(self):
        self.manifest["tests"].append(copy.deepcopy(self.manifest["tests"][0]))
        self.fail_on("duplicate CTest name")

    def test_unresolved_generator_expression_rejected(self):
        self.manifest["tests"][0]["command"] = ["$<TARGET_FILE:missing>"]
        self.fail_on("unresolved/empty")

    def test_missing_executable_rejected(self):
        self.manifest["tests"][0]["command"] = []
        self.fail_on("unresolved/empty")

    def test_cmake_notfound_executable_rejected(self):
        self.manifest["tests"][0]["command"] = ["CLANG-NOTFOUND"]
        self.fail_on("unresolved/empty")

    def test_disabled_critical_test_rejected(self):
        self.manifest["tests"][0]["properties"].append(
            {"name": "DISABLED", "value": True})
        self.fail_on("disabled/inverted")

    def test_will_fail_inverted_critical_test_rejected(self):
        self.manifest["tests"][0]["properties"].append(
            {"name": "WILL_FAIL", "value": "ON"})
        self.fail_on("disabled/inverted")

    def test_skip_return_code_is_not_positive_evidence(self):
        self.manifest["tests"][0]["properties"].append(
            {"name": "SKIP_RETURN_CODE", "value": 77})
        self.fail_on("skip policy")

    def test_skip_regex_is_not_positive_evidence(self):
        self.manifest["tests"][0]["properties"].append(
            {"name": "SKIP_REGULAR_EXPRESSION", "value": "unavailable"})
        self.fail_on("skip policy")

    def test_missing_timeout_rejected(self):
        self.manifest["tests"][0]["properties"] = []
        self.fail_on("lacks numeric timeout")

    def test_invalid_timeout_rejected(self):
        for value in (0, -1, 301, "NaN", "Infinity"):
            corpus = fixture()
            corpus["tests"][0]["properties"][0]["value"] = value
            with self.assertRaisesRegex(ManifestFailure, "timeout is unsafe"):
                check_manifest(corpus)

    def test_duplicate_properties_rejected(self):
        p = self.manifest["tests"][0]["properties"][0]
        self.manifest["tests"][0]["properties"].append(copy.deepcopy(p))
        self.fail_on("duplicate property")

    def test_minimum_count_rejected(self):
        self.manifest["tests"] = self.manifest["tests"][:10]
        self.fail_on("expected at least")

    def test_bad_entry_type_rejected(self):
        self.manifest["tests"].append(None)
        self.fail_on("valid name")


if __name__ == "__main__":
    unittest.main()
